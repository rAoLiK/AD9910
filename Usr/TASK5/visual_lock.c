#include "visual_lock.h"

#include <stddef.h>
#include <string.h>

#define VISUAL_LOCK_GRID_MILLIHZ              (100UL)
#define VISUAL_LOCK_PROBE_MILLIHZ             (100UL)
#define VISUAL_LOCK_MAX_OFFSET_MILLIHZ        (5000UL)
#define VISUAL_LOCK_SETTLE_MS                  (120UL)
#define VISUAL_LOCK_PROBE_SAMPLES              (4U)
#define VISUAL_LOCK_MIN_QUALITY                (30U)
#define VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ   (40U)
#define VISUAL_LOCK_WORSEN_MARGIN_MILLIHZ      (100U)
#define VISUAL_LOCK_WORSEN_SAMPLES             (3U)
#define VISUAL_LOCK_SPEED_ENTER_MILLIHZ        (80U)
#define VISUAL_LOCK_SPEED_EXIT_MILLIHZ         (180U)
#define VISUAL_LOCK_FREQUENCY_STABLE_SAMPLES   (6U)
#define VISUAL_LOCK_LINE_PHASE_MDEG            (3000U)
#define VISUAL_LOCK_LINE_STABLE_SAMPLES        (8U)
#define VISUAL_LOCK_PHASE_PROBE_MDEG            (5000L)
#define VISUAL_LOCK_PHASE_WORSEN_MDEG           (1000U)
#define VISUAL_LOCK_KP_NUM                      (3L)
#define VISUAL_LOCK_KP_DEN                      (4L)
#define VISUAL_LOCK_KI_NUM                      (1L)
#define VISUAL_LOCK_KI_DEN                      (2L)

static bool VisualLock_DeadlineReached(uint32_t now_ms,
                                       uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t VisualLock_Quantize(uint32_t frequency_millihz)
{
  return ((frequency_millihz + (VISUAL_LOCK_GRID_MILLIHZ / 2UL)) /
          VISUAL_LOCK_GRID_MILLIHZ) * VISUAL_LOCK_GRID_MILLIHZ;
}

static int32_t VisualLock_WrapPhase(int32_t phase_mdeg)
{
  while (phase_mdeg >= 360000L) {
    phase_mdeg -= 360000L;
  }
  while (phase_mdeg < 0L) {
    phase_mdeg += 360000L;
  }
  return phase_mdeg;
}

static void VisualLock_ClearObservation(visual_lock_controller_t *controller)
{
  controller->accumulated_speed_millihz = 0UL;
  controller->observation_count = 0U;
}

static void VisualLock_Publish(const visual_lock_controller_t *controller,
                               visual_lock_output_t *output)
{
  output->frequency_millihz = controller->command_frequency_millihz;
  output->phase_offset_mdeg = controller->phase_offset_mdeg;
  output->locked = controller->state == VISUAL_LOCK_LOCKED;
  output->error = controller->state == VISUAL_LOCK_ERROR;
}

static void VisualLock_Command(visual_lock_controller_t *controller,
                               uint32_t frequency_millihz,
                               int32_t phase_offset_mdeg,
                               uint32_t now_ms,
                               visual_lock_output_t *output)
{
  frequency_millihz = VisualLock_Quantize(frequency_millihz);
  phase_offset_mdeg = VisualLock_WrapPhase(phase_offset_mdeg);
  if ((frequency_millihz != controller->command_frequency_millihz) ||
      (phase_offset_mdeg != controller->phase_offset_mdeg)) {
    controller->command_frequency_millihz = frequency_millihz;
    controller->phase_offset_mdeg = phase_offset_mdeg;
    controller->settle_deadline_ms = now_ms + VISUAL_LOCK_SETTLE_MS;
    output->command_changed = true;
  }
  VisualLock_Publish(controller, output);
}

static bool VisualLock_ValidSample(const visual_lock_sample_t *sample)
{
  return ((sample->flags & VISUAL_LOCK_SAMPLE_PHASE_VALID) != 0U) &&
         (sample->quality >= VISUAL_LOCK_MIN_QUALITY) &&
         (sample->phase_mdeg <= 180000U) &&
         (sample->speed_millihz <= VISUAL_LOCK_MAX_OFFSET_MILLIHZ * 2UL);
}

static bool VisualLock_AddObservation(visual_lock_controller_t *controller,
                                      uint16_t speed_millihz,
                                      uint16_t *average_millihz)
{
  controller->accumulated_speed_millihz += speed_millihz;
  controller->observation_count++;
  if (controller->observation_count < VISUAL_LOCK_PROBE_SAMPLES) {
    return false;
  }
  *average_millihz = (uint16_t)(
      (controller->accumulated_speed_millihz +
       (controller->observation_count / 2U)) /
      controller->observation_count);
  VisualLock_ClearObservation(controller);
  return true;
}

static void VisualLock_BeginTracking(visual_lock_controller_t *controller,
                                     int32_t direction,
                                     uint16_t speed_millihz,
                                     uint32_t now_ms)
{
  controller->state = VISUAL_LOCK_TRACK_FREQUENCY;
  controller->frequency_direction = direction;
  controller->track_anchor_millihz =
      controller->command_frequency_millihz;
  controller->frequency_integral_millihz = 0L;
  controller->best_frequency_millihz =
      controller->command_frequency_millihz;
  controller->best_speed_millihz = speed_millihz;
  controller->last_control_ms = now_ms;
  controller->worsening_count = 0U;
  controller->frequency_stable_count = 0U;
}

static void VisualLock_ReverseAtBest(visual_lock_controller_t *controller,
                                     uint32_t now_ms,
                                     visual_lock_output_t *output)
{
  controller->frequency_direction = -controller->frequency_direction;
  controller->direction_reversal_count++;
  controller->track_anchor_millihz = controller->best_frequency_millihz;
  controller->frequency_integral_millihz = 0L;
  controller->worsening_count = 0U;
  controller->frequency_stable_count = 0U;
  controller->last_control_ms = now_ms;
  VisualLock_Command(controller, controller->best_frequency_millihz,
                     controller->phase_offset_mdeg, now_ms, output);
}

static void VisualLock_TrackFrequency(visual_lock_controller_t *controller,
                                     const visual_lock_sample_t *sample,
                                     visual_lock_output_t *output)
{
  uint32_t elapsed_ms = sample->timestamp_ms - controller->last_control_ms;
  int32_t proportional;
  int32_t correction;
  int64_t candidate;
  uint32_t lower_limit = controller->seed_frequency_millihz -
                         VISUAL_LOCK_MAX_OFFSET_MILLIHZ;
  uint32_t upper_limit = controller->seed_frequency_millihz +
                         VISUAL_LOCK_MAX_OFFSET_MILLIHZ;

  if (sample->speed_millihz < controller->best_speed_millihz) {
    controller->best_speed_millihz = sample->speed_millihz;
    controller->best_frequency_millihz =
        controller->command_frequency_millihz;
    controller->worsening_count = 0U;
  } else if ((sample->speed_millihz >
              (uint16_t)(controller->best_speed_millihz +
                         VISUAL_LOCK_WORSEN_MARGIN_MILLIHZ)) &&
             (controller->command_frequency_millihz !=
              controller->best_frequency_millihz)) {
    if (controller->worsening_count < UINT8_MAX) {
      controller->worsening_count++;
    }
    if (controller->worsening_count >= VISUAL_LOCK_WORSEN_SAMPLES) {
      VisualLock_ReverseAtBest(controller, sample->timestamp_ms, output);
      return;
    }
  } else {
    controller->worsening_count = 0U;
  }

  if (sample->speed_millihz <= VISUAL_LOCK_SPEED_ENTER_MILLIHZ) {
    if (controller->frequency_stable_count < UINT8_MAX) {
      controller->frequency_stable_count++;
    }
    if (controller->frequency_stable_count >=
        VISUAL_LOCK_FREQUENCY_STABLE_SAMPLES) {
      controller->state = VISUAL_LOCK_ALIGN_LINE;
      controller->best_phase_mdeg = sample->phase_mdeg;
      controller->best_phase_offset_mdeg = controller->phase_offset_mdeg;
      controller->phase_direction = 0L;
      controller->line_stable_count = 0U;
      VisualLock_Publish(controller, output);
      return;
    }
  } else {
    controller->frequency_stable_count = 0U;
  }

  if (elapsed_ms < 50UL) {
    return;
  }
  if (elapsed_ms > 500UL) {
    elapsed_ms = 500UL;
  }
  controller->last_control_ms = sample->timestamp_ms;
  controller->frequency_integral_millihz +=
      (int32_t)(((int64_t)sample->speed_millihz *
                 (int64_t)elapsed_ms * VISUAL_LOCK_KI_NUM) /
                (1000LL * VISUAL_LOCK_KI_DEN));
  if (controller->frequency_integral_millihz >
      (int32_t)VISUAL_LOCK_MAX_OFFSET_MILLIHZ) {
    controller->frequency_integral_millihz =
        (int32_t)VISUAL_LOCK_MAX_OFFSET_MILLIHZ;
  }
  proportional =
      (int32_t)(((int32_t)sample->speed_millihz *
                 VISUAL_LOCK_KP_NUM) / VISUAL_LOCK_KP_DEN);
  correction = proportional + controller->frequency_integral_millihz;
  candidate = (int64_t)controller->track_anchor_millihz +
              ((int64_t)controller->frequency_direction * correction);

  if ((candidate < (int64_t)lower_limit) ||
      (candidate > (int64_t)upper_limit)) {
    /* The problem guarantees |df| <= 5 Hz. Restore the seed immediately and
     * search the opposite direction instead of walking farther away. */
    controller->boundary_reversal_count++;
    controller->frequency_direction = -controller->frequency_direction;
    controller->direction_reversal_count++;
    controller->track_anchor_millihz = controller->seed_frequency_millihz;
    controller->frequency_integral_millihz = 0L;
    controller->best_frequency_millihz = controller->seed_frequency_millihz;
    controller->best_speed_millihz = controller->baseline_speed_millihz;
    controller->worsening_count = 0U;
    VisualLock_Command(controller, controller->seed_frequency_millihz,
                       controller->phase_offset_mdeg,
                       sample->timestamp_ms, output);
    return;
  }

  VisualLock_Command(controller, (uint32_t)candidate,
                     controller->phase_offset_mdeg,
                     sample->timestamp_ms, output);
}

static void VisualLock_AlignLine(visual_lock_controller_t *controller,
                                 const visual_lock_sample_t *sample,
                                 visual_lock_output_t *output)
{
  int32_t phase_step_mdeg;

  if (sample->speed_millihz > VISUAL_LOCK_SPEED_EXIT_MILLIHZ) {
    controller->state = VISUAL_LOCK_TRACK_FREQUENCY;
    controller->track_anchor_millihz = controller->best_frequency_millihz;
    controller->frequency_integral_millihz = 0L;
    controller->frequency_stable_count = 0U;
    controller->last_control_ms = sample->timestamp_ms;
    return;
  }
  if ((sample->flags & VISUAL_LOCK_SAMPLE_FAMILY_VALID) == 0U) {
    controller->line_stable_count = 0U;
    return;
  }

  if (sample->phase_mdeg < controller->best_phase_mdeg) {
    controller->best_phase_mdeg = sample->phase_mdeg;
    controller->best_phase_offset_mdeg = controller->phase_offset_mdeg;
  } else if ((controller->phase_direction != 0L) &&
             (sample->phase_mdeg >
              controller->best_phase_mdeg +
                  VISUAL_LOCK_PHASE_WORSEN_MDEG)) {
    controller->phase_direction = -controller->phase_direction;
    VisualLock_Command(controller, controller->best_frequency_millihz,
                       controller->best_phase_offset_mdeg,
                       sample->timestamp_ms, output);
    return;
  }

  if ((sample->phase_mdeg <= VISUAL_LOCK_LINE_PHASE_MDEG) &&
      (sample->speed_millihz <= VISUAL_LOCK_SPEED_ENTER_MILLIHZ)) {
    if (controller->line_stable_count < UINT8_MAX) {
      controller->line_stable_count++;
    }
    if (controller->line_stable_count >= VISUAL_LOCK_LINE_STABLE_SAMPLES) {
      controller->state = VISUAL_LOCK_LOCKED;
      VisualLock_Publish(controller, output);
    }
    /* The requested line is already present. Do not disturb it merely to
     * discover the phase-actuation sign; keep observing until it is stable. */
    return;
  } else {
    controller->line_stable_count = 0U;
  }

  if (controller->phase_direction == 0L) {
    controller->phase_direction = 1L;
    controller->best_phase_mdeg = sample->phase_mdeg;
    controller->best_phase_offset_mdeg = controller->phase_offset_mdeg;
    VisualLock_Command(controller, controller->best_frequency_millihz,
                       controller->phase_offset_mdeg +
                           VISUAL_LOCK_PHASE_PROBE_MDEG,
                       sample->timestamp_ms, output);
    return;
  }

  if (sample->phase_mdeg > 30000U) {
    phase_step_mdeg = 10000L;
  } else if (sample->phase_mdeg > 8000U) {
    phase_step_mdeg = 2000L;
  } else {
    phase_step_mdeg = 500L;
  }
  VisualLock_Command(controller, controller->best_frequency_millihz,
                     controller->phase_offset_mdeg +
                         controller->phase_direction * phase_step_mdeg,
                     sample->timestamp_ms, output);
}

void VisualLock_Init(visual_lock_controller_t *controller,
                     uint32_t seed_frequency_millihz,
                     uint32_t now_ms)
{
  if (controller == NULL) {
    return;
  }
  memset(controller, 0, sizeof(*controller));
  controller->state = VISUAL_LOCK_BASELINE;
  controller->seed_frequency_millihz =
      VisualLock_Quantize(seed_frequency_millihz);
  controller->command_frequency_millihz =
      controller->seed_frequency_millihz;
  controller->best_frequency_millihz =
      controller->seed_frequency_millihz;
  controller->track_anchor_millihz =
      controller->seed_frequency_millihz;
  controller->best_speed_millihz = UINT16_MAX;
  controller->best_phase_mdeg = UINT32_MAX;
  controller->settle_deadline_ms = now_ms;
  controller->last_control_ms = now_ms;
}

void VisualLock_Reset(visual_lock_controller_t *controller)
{
  if (controller != NULL) {
    memset(controller, 0, sizeof(*controller));
  }
}

void VisualLock_Step(visual_lock_controller_t *controller,
                     const visual_lock_sample_t *sample,
                     visual_lock_output_t *output)
{
  uint16_t average_speed;

  if (output != NULL) {
    memset(output, 0, sizeof(*output));
  }
  if ((controller == NULL) || (sample == NULL) || (output == NULL)) {
    return;
  }
  VisualLock_Publish(controller, output);
  if ((controller->state == VISUAL_LOCK_LOCKED) ||
      (controller->state == VISUAL_LOCK_ERROR) ||
      !VisualLock_ValidSample(sample)) {
    if (!VisualLock_ValidSample(sample) &&
        (controller->rejected_sample_count < UINT8_MAX)) {
      controller->rejected_sample_count++;
    }
    return;
  }
  controller->valid_sample_count++;
  if (!VisualLock_DeadlineReached(sample->timestamp_ms,
                                  controller->settle_deadline_ms)) {
    return;
  }

  switch (controller->state) {
    case VISUAL_LOCK_BASELINE:
      if (!VisualLock_AddObservation(controller, sample->speed_millihz,
                                     &average_speed)) {
        break;
      }
      controller->baseline_speed_millihz = average_speed;
      controller->best_speed_millihz = average_speed;
      controller->best_frequency_millihz =
          controller->seed_frequency_millihz;
      if (average_speed <= VISUAL_LOCK_SPEED_ENTER_MILLIHZ) {
        controller->frequency_stable_count =
            VISUAL_LOCK_FREQUENCY_STABLE_SAMPLES;
        controller->state = VISUAL_LOCK_ALIGN_LINE;
        controller->best_phase_mdeg = sample->phase_mdeg;
        break;
      }
      controller->state = VISUAL_LOCK_PROBE_POSITIVE;
      VisualLock_Command(controller,
                         controller->seed_frequency_millihz +
                             VISUAL_LOCK_PROBE_MILLIHZ,
                         controller->phase_offset_mdeg,
                         sample->timestamp_ms, output);
      break;

    case VISUAL_LOCK_PROBE_POSITIVE:
      if (!VisualLock_AddObservation(controller, sample->speed_millihz,
                                     &average_speed)) {
        break;
      }
      controller->positive_probe_speed_millihz = average_speed;
      if (average_speed + VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ <
          controller->baseline_speed_millihz) {
        VisualLock_BeginTracking(controller, 1L, average_speed,
                                 sample->timestamp_ms);
      } else {
        controller->state = VISUAL_LOCK_PROBE_NEGATIVE;
        VisualLock_Command(controller,
                           controller->seed_frequency_millihz -
                               VISUAL_LOCK_PROBE_MILLIHZ,
                           controller->phase_offset_mdeg,
                           sample->timestamp_ms, output);
      }
      break;

    case VISUAL_LOCK_PROBE_NEGATIVE:
      if (!VisualLock_AddObservation(controller, sample->speed_millihz,
                                     &average_speed)) {
        break;
      }
      if ((average_speed <= controller->positive_probe_speed_millihz) ||
          (average_speed + VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ <
           controller->baseline_speed_millihz)) {
        VisualLock_BeginTracking(controller, -1L, average_speed,
                                 sample->timestamp_ms);
      } else {
        VisualLock_Command(controller,
                           controller->seed_frequency_millihz +
                               VISUAL_LOCK_PROBE_MILLIHZ,
                           controller->phase_offset_mdeg,
                           sample->timestamp_ms, output);
        VisualLock_BeginTracking(
            controller, 1L, controller->positive_probe_speed_millihz,
            sample->timestamp_ms);
      }
      break;

    case VISUAL_LOCK_TRACK_FREQUENCY:
      VisualLock_TrackFrequency(controller, sample, output);
      break;

    case VISUAL_LOCK_ALIGN_LINE:
      VisualLock_AlignLine(controller, sample, output);
      break;

    case VISUAL_LOCK_IDLE:
    case VISUAL_LOCK_LOCKED:
    case VISUAL_LOCK_ERROR:
    default:
      break;
  }
  VisualLock_Publish(controller, output);
}
