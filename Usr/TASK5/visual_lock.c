#include "visual_lock.h"

#include <stddef.h>
#include <string.h>

#define VISUAL_LOCK_GRID_MILLIHZ              (10UL)
#define VISUAL_LOCK_PROBE_MILLIHZ             (100UL)
#define VISUAL_LOCK_MAX_OFFSET_MILLIHZ        (5000UL)
#define VISUAL_LOCK_SETTLE_MS                 (200UL)
#define VISUAL_LOCK_OBSERVATION_SAMPLES       (4U)
#define VISUAL_LOCK_MIN_QUALITY               (30U)
#define VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ  (20U)
#define VISUAL_LOCK_WORSEN_MARGIN_MILLIHZ     (60U)
#define VISUAL_LOCK_WORSEN_WINDOWS            (2U)
#define VISUAL_LOCK_SPEED_ENTER_MILLIHZ       (60U)
#define VISUAL_LOCK_SPEED_EXIT_MILLIHZ        (180U)
#define VISUAL_LOCK_STABLE_WINDOWS            (5U)
#define VISUAL_LOCK_REACQUIRE_WINDOWS         (3U)
#define VISUAL_LOCK_STEP_DIVISOR              (2UL)
#define VISUAL_LOCK_MAX_STEP_MILLIHZ          (1000UL)

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

static uint32_t VisualLock_LowerLimit(
    const visual_lock_controller_t *controller)
{
  return (controller->seed_frequency_millihz >
          VISUAL_LOCK_MAX_OFFSET_MILLIHZ)
             ? controller->seed_frequency_millihz -
                   VISUAL_LOCK_MAX_OFFSET_MILLIHZ
             : 0UL;
}

static uint32_t VisualLock_UpperLimit(
    const visual_lock_controller_t *controller)
{
  return controller->seed_frequency_millihz +
         VISUAL_LOCK_MAX_OFFSET_MILLIHZ;
}

static uint32_t VisualLock_ClampFrequency(
    const visual_lock_controller_t *controller,
    int64_t frequency_millihz)
{
  uint32_t lower = VisualLock_LowerLimit(controller);
  uint32_t upper = VisualLock_UpperLimit(controller);

  if (frequency_millihz < (int64_t)lower) {
    return lower;
  }
  if (frequency_millihz > (int64_t)upper) {
    return upper;
  }
  return (uint32_t)frequency_millihz;
}

static void VisualLock_ClearObservation(
    visual_lock_controller_t *controller)
{
  controller->accumulated_speed_millihz = 0UL;
  controller->observation_count = 0U;
}

static void VisualLock_Publish(
    const visual_lock_controller_t *controller,
    visual_lock_output_t *output)
{
  output->frequency_millihz = controller->command_frequency_millihz;
  output->phase_offset_mdeg = controller->phase_offset_mdeg;
  output->locked = controller->state == VISUAL_LOCK_LOCKED;
  output->error = controller->state == VISUAL_LOCK_ERROR;
}

static void VisualLock_Command(visual_lock_controller_t *controller,
                               int64_t frequency_millihz,
                               uint32_t now_ms,
                               visual_lock_output_t *output)
{
  uint32_t command = VisualLock_Quantize(
      VisualLock_ClampFrequency(controller, frequency_millihz));

  command = VisualLock_ClampFrequency(controller, command);
  if (command != controller->command_frequency_millihz) {
    controller->command_frequency_millihz = command;
    controller->settle_deadline_ms = now_ms + VISUAL_LOCK_SETTLE_MS;
    VisualLock_ClearObservation(controller);
    output->command_changed = true;
  }
  VisualLock_Publish(controller, output);
}

static bool VisualLock_ValidSample(const visual_lock_sample_t *sample)
{
  return ((sample->flags & VISUAL_LOCK_SAMPLE_PHASE_VALID) != 0U) &&
         (sample->quality >= VISUAL_LOCK_MIN_QUALITY) &&
         (sample->phase_mdeg <= 180000U) &&
         (sample->speed_millihz <=
          VISUAL_LOCK_MAX_OFFSET_MILLIHZ * 2UL);
}

static bool VisualLock_AddObservation(
    visual_lock_controller_t *controller,
    uint16_t speed_millihz,
    uint16_t *average_millihz)
{
  controller->accumulated_speed_millihz += speed_millihz;
  controller->observation_count++;
  if (controller->observation_count <
      VISUAL_LOCK_OBSERVATION_SAMPLES) {
    return false;
  }
  *average_millihz = (uint16_t)(
      (controller->accumulated_speed_millihz +
       (controller->observation_count / 2U)) /
      controller->observation_count);
  VisualLock_ClearObservation(controller);
  return true;
}

static void VisualLock_BeginTracking(
    visual_lock_controller_t *controller,
    int32_t direction,
    uint16_t speed_millihz)
{
  controller->state = VISUAL_LOCK_TRACK_FREQUENCY;
  controller->frequency_direction = direction;
  controller->best_frequency_millihz =
      controller->command_frequency_millihz;
  controller->best_speed_millihz = speed_millihz;
  controller->worsening_count = 0U;
  controller->frequency_stable_count = 0U;
  controller->unlock_count = 0U;
  VisualLock_ClearObservation(controller);
}

static void VisualLock_BeginBaseline(
    visual_lock_controller_t *controller,
    uint32_t center_millihz,
    uint32_t now_ms,
    visual_lock_output_t *output)
{
  controller->state = VISUAL_LOCK_BASELINE;
  controller->probe_center_millihz = VisualLock_ClampFrequency(
      controller, center_millihz);
  controller->frequency_direction = 0L;
  controller->worsening_count = 0U;
  controller->frequency_stable_count = 0U;
  controller->unlock_count = 0U;
  VisualLock_ClearObservation(controller);
  VisualLock_Command(controller, controller->probe_center_millihz,
                     now_ms, output);
}

static void VisualLock_ReverseAtBest(
    visual_lock_controller_t *controller,
    uint32_t now_ms,
    visual_lock_output_t *output)
{
  controller->frequency_direction = -controller->frequency_direction;
  controller->direction_reversal_count++;
  controller->worsening_count = 0U;
  controller->frequency_stable_count = 0U;
  VisualLock_Command(controller, controller->best_frequency_millihz,
                     now_ms, output);
}

static void VisualLock_TrackFrequency(
    visual_lock_controller_t *controller,
    uint16_t speed_millihz,
    uint32_t now_ms,
    visual_lock_output_t *output)
{
  uint32_t step_millihz;
  int64_t candidate;
  uint32_t bounded_candidate;

  if (speed_millihz < controller->best_speed_millihz) {
    controller->best_speed_millihz = speed_millihz;
    controller->best_frequency_millihz =
        controller->command_frequency_millihz;
    controller->worsening_count = 0U;
  } else if ((speed_millihz >
              (uint16_t)(controller->best_speed_millihz +
                         VISUAL_LOCK_WORSEN_MARGIN_MILLIHZ)) &&
             (controller->command_frequency_millihz !=
              controller->best_frequency_millihz)) {
    if (controller->worsening_count < UINT8_MAX) {
      controller->worsening_count++;
    }
    if (controller->worsening_count >=
        VISUAL_LOCK_WORSEN_WINDOWS) {
      VisualLock_ReverseAtBest(controller, now_ms, output);
      return;
    }
  } else {
    controller->worsening_count = 0U;
  }

  if (speed_millihz <= VISUAL_LOCK_SPEED_ENTER_MILLIHZ) {
    if (controller->frequency_stable_count < UINT8_MAX) {
      controller->frequency_stable_count++;
    }
    if (controller->frequency_stable_count >=
        VISUAL_LOCK_STABLE_WINDOWS) {
      controller->state = VISUAL_LOCK_LOCKED;
      controller->best_frequency_millihz =
          controller->command_frequency_millihz;
      controller->best_speed_millihz = speed_millihz;
      controller->unlock_count = 0U;
      VisualLock_Publish(controller, output);
      return;
    }
    /* Do not dither across the optimum while collecting stable evidence.
     * With a 0.01 Hz command grid, holding this point removes the visible
     * half-cycle hunt produced by continuous same-direction steps. */
    VisualLock_Publish(controller, output);
    return;
  } else {
    controller->frequency_stable_count = 0U;
  }

  step_millihz = speed_millihz / VISUAL_LOCK_STEP_DIVISOR;
  if (step_millihz < VISUAL_LOCK_GRID_MILLIHZ) {
    step_millihz = VISUAL_LOCK_GRID_MILLIHZ;
  }
  if (step_millihz > VISUAL_LOCK_MAX_STEP_MILLIHZ) {
    step_millihz = VISUAL_LOCK_MAX_STEP_MILLIHZ;
  }
  candidate = (int64_t)controller->command_frequency_millihz +
              ((int64_t)controller->frequency_direction *
               (int64_t)step_millihz);
  bounded_candidate = VisualLock_ClampFrequency(controller, candidate);
  if ((uint32_t)candidate != bounded_candidate) {
    controller->boundary_reversal_count++;
    VisualLock_ReverseAtBest(controller, now_ms, output);
    return;
  }
  VisualLock_Command(controller, candidate, now_ms, output);
}

void VisualLock_InitTarget(visual_lock_controller_t *controller,
                           uint32_t seed_frequency_millihz,
                           int32_t target_phase_offset_mdeg,
                           uint32_t now_ms)
{
  if (controller == NULL) {
    return;
  }
  memset(controller, 0, sizeof(*controller));
  controller->state = VISUAL_LOCK_BASELINE;
  controller->seed_frequency_millihz =
      VisualLock_Quantize(seed_frequency_millihz);
  controller->probe_center_millihz =
      controller->seed_frequency_millihz;
  controller->command_frequency_millihz =
      controller->seed_frequency_millihz;
  controller->best_frequency_millihz =
      controller->seed_frequency_millihz;
  controller->phase_offset_mdeg =
      VisualLock_WrapPhase(target_phase_offset_mdeg);
  controller->best_speed_millihz = UINT16_MAX;
  controller->settle_deadline_ms = now_ms;
}

void VisualLock_Init(visual_lock_controller_t *controller,
                     uint32_t seed_frequency_millihz,
                     uint32_t now_ms)
{
  VisualLock_InitTarget(
      controller, seed_frequency_millihz, 0L, now_ms);
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
  if ((controller->state == VISUAL_LOCK_IDLE) ||
      (controller->state == VISUAL_LOCK_ERROR)) {
    return;
  }
  if (!VisualLock_ValidSample(sample)) {
    if (controller->rejected_sample_count < UINT8_MAX) {
      controller->rejected_sample_count++;
    }
    return;
  }
  if (controller->valid_sample_count < UINT8_MAX) {
    controller->valid_sample_count++;
  }
  if (!VisualLock_DeadlineReached(sample->timestamp_ms,
                                  controller->settle_deadline_ms)) {
    return;
  }

  if (!VisualLock_AddObservation(controller, sample->speed_millihz,
                                 &average_speed)) {
    return;
  }

  switch (controller->state) {
    case VISUAL_LOCK_BASELINE:
      controller->baseline_speed_millihz = average_speed;
      controller->best_speed_millihz = average_speed;
      controller->best_frequency_millihz =
          controller->command_frequency_millihz;
      if (average_speed <= VISUAL_LOCK_SPEED_ENTER_MILLIHZ) {
        controller->state = VISUAL_LOCK_LOCKED;
        controller->unlock_count = 0U;
        break;
      }
      controller->state = VISUAL_LOCK_PROBE_POSITIVE;
      VisualLock_Command(
          controller,
          (int64_t)controller->probe_center_millihz +
              VISUAL_LOCK_PROBE_MILLIHZ,
          sample->timestamp_ms, output);
      break;

    case VISUAL_LOCK_PROBE_POSITIVE:
      controller->positive_probe_speed_millihz = average_speed;
      if (average_speed + VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ <
          controller->baseline_speed_millihz) {
        VisualLock_BeginTracking(controller, 1L, average_speed);
      } else {
        controller->state = VISUAL_LOCK_PROBE_NEGATIVE;
        VisualLock_Command(
            controller,
            (int64_t)controller->probe_center_millihz -
                VISUAL_LOCK_PROBE_MILLIHZ,
            sample->timestamp_ms, output);
      }
      break;

    case VISUAL_LOCK_PROBE_NEGATIVE:
      if ((average_speed + VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ <
           controller->positive_probe_speed_millihz) ||
          (average_speed + VISUAL_LOCK_DIRECTION_MARGIN_MILLIHZ <
           controller->baseline_speed_millihz)) {
        VisualLock_BeginTracking(controller, -1L, average_speed);
      } else {
        VisualLock_Command(
            controller,
            (int64_t)controller->probe_center_millihz +
                VISUAL_LOCK_PROBE_MILLIHZ,
            sample->timestamp_ms, output);
        VisualLock_BeginTracking(
            controller, 1L,
            controller->positive_probe_speed_millihz);
      }
      break;

    case VISUAL_LOCK_TRACK_FREQUENCY:
      VisualLock_TrackFrequency(controller, average_speed,
                                sample->timestamp_ms, output);
      break;

    case VISUAL_LOCK_LOCKED:
      if (average_speed > VISUAL_LOCK_SPEED_EXIT_MILLIHZ) {
        if (controller->unlock_count < UINT8_MAX) {
          controller->unlock_count++;
        }
        if (controller->unlock_count >=
            VISUAL_LOCK_REACQUIRE_WINDOWS) {
          VisualLock_BeginBaseline(
              controller, controller->command_frequency_millihz,
              sample->timestamp_ms, output);
        }
      } else {
        controller->unlock_count = 0U;
        if (average_speed < controller->best_speed_millihz) {
          controller->best_speed_millihz = average_speed;
          controller->best_frequency_millihz =
              controller->command_frequency_millihz;
        }
      }
      break;

    case VISUAL_LOCK_IDLE:
    case VISUAL_LOCK_ERROR:
    default:
      break;
  }
  VisualLock_Publish(controller, output);
}
