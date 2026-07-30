#include "lock_controller.h"

#include "pll_config.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define LOCK_PI_F                         (3.14159265358979323846f)
#define LOCK_TWO_PI_F                     (2.0f * LOCK_PI_F)
#define LOCK_FINE_PHASE_RAD               (4.0f * LOCK_PI_F / 180.0f)
#define LOCK_MID_PHASE_RAD                (15.0f * LOCK_PI_F / 180.0f)
#define LOCK_FINE_GRADIENT_HZ             (0.50f)
#define LOCK_FINE_STEP_HZ                 (0.05f)
#define LOCK_DIRECT_PHASE_GAIN            (0.65f)
#define LOCK_DIRECT_FINE_STEP_RAD          (0.15f * LOCK_PI_F / 180.0f)
#define LOCK_DIRECT_MID_STEP_RAD           (1.0f * LOCK_PI_F / 180.0f)
#define LOCK_DIRECT_COARSE_STEP_RAD        (10.0f * LOCK_PI_F / 180.0f)
#define LOCK_DIRECT_LOCK_STEP_RAD          (0.10f * LOCK_PI_F / 180.0f)
#define LOCK_ACQUIRE_THRESHOLD_RAD        (3.0f * LOCK_PI_F / 180.0f)
#define LOCK_RELEASE_THRESHOLD_RAD        (12.0f * LOCK_PI_F / 180.0f)
#define LOCK_ACQUIRE_TIME_S               (0.050f)
#define LOCK_RELEASE_TIME_S               (0.030f)
#define LOCK_INPUT_CHANGE_MIN_HZ          (40.0f)
#define LOCK_INPUT_CHANGE_FRACTION        (0.00200f)
#define LOCK_INPUT_STABLE_MIN_HZ          (2.0f)
#define LOCK_INPUT_STABLE_FRACTION        (0.00002f)
#define LOCK_LOW_INPUT_STABLE_HZ          (1.0f)
#define LOCK_PHASE_RATE_FILTER_TAU_S      (0.006f)
#define LOCK_PHASE_RATE_GAIN              (0.50f)
#define LOCK_PHASE_RATE_LIMIT_HZ          (500.0f)
#define LOCK_LOW_RATE_FILTER_TAU_S        (0.020f)
#define LOCK_LOW_RATE_LEARN_TAU_S         (0.020f)
#define LOCK_HIGH_HOLD_INPUT_HZ           (68000.0f)
#define LOCK_HIGH_HOLD_EXIT_HZ            (65000.0f)
#define LOCK_HIGH_HOLD_SETTLE_S           (0.150f)
#define LOCK_HIGH_HOLD_FILTER_TAU_S       (0.020f)
#define LOCK_HIGH_HOLD_PHASE_DEADBAND_RAD (0.20f * LOCK_PI_F / 180.0f)
#define LOCK_HIGH_HOLD_PHASE_GAIN         (0.25f)
#define LOCK_HIGH_HOLD_PHASE_STEP_RAD     (0.06f * LOCK_PI_F / 180.0f)
#define LOCK_FREQUENCY_MISSING_RESET_S    (0.100f)
#define LOCK_MAX_DDS_FREQUENCY_HZ         (400000000.0f)
#define LOCK_LOW_ENTER_HZ                 (35000.0f)
#define LOCK_LOW_EXIT_HZ                  (45000.0f)
#define LOCK_HIGH_ENTER_HZ                (90000.0f)
#define LOCK_HIGH_EXIT_HZ                 (75000.0f)

typedef struct {
  float phase_filter_tau_s;
  float frequency_track_tau_s;
  float proportional_hz_per_rad;
  float integral_hz_per_rad_s;
  float input_change_hold_s;
  float mid_step_hz;
  float coarse_step_hz;
} lock_band_parameters_t;

static const lock_band_parameters_t s_band_parameters[] = {
    /* LOW: reject block-to-block jitter and avoid repeated reacquisition. */
    {0.010f, 0.015f, 0.0f, 5.0f, 0.060f, 0.10f, 0.50f},
    /* MID: keep the previously board-verified high-frequency FTW loop. */
    {0.004f, 0.004f, 20.0f, 120.0f, 0.060f, 0.50f, 5.0f},
    /* HIGH: retain the fast loop already verified above 41 kHz. */
    {0.004f, 0.004f, 20.0f, 120.0f, 0.060f, 0.50f, 5.0f}
};

static float LockController_Wrap(float radians)
{
  while (radians > LOCK_PI_F) {
    radians -= LOCK_TWO_PI_F;
  }
  while (radians <= -LOCK_PI_F) {
    radians += LOCK_TWO_PI_F;
  }
  return radians;
}

static float LockController_Clamp(float value, float limit)
{
  if (value > limit) {
    return limit;
  }
  if (value < -limit) {
    return -limit;
  }
  return value;
}

static void LockController_UpdateBand(lock_controller_t *controller,
                                      float reference_frequency_hz,
                                      bool initialize)
{
  if (initialize) {
    if (reference_frequency_hz < 40000.0f) {
      controller->band = LOCK_BAND_LOW;
    } else if (reference_frequency_hz < 80000.0f) {
      controller->band = LOCK_BAND_MID;
    } else {
      controller->band = LOCK_BAND_HIGH;
    }
    return;
  }

  switch (controller->band) {
    case LOCK_BAND_LOW:
      if (reference_frequency_hz > LOCK_LOW_EXIT_HZ) {
        controller->band = LOCK_BAND_MID;
      }
      break;
    case LOCK_BAND_HIGH:
      if (reference_frequency_hz < LOCK_HIGH_EXIT_HZ) {
        controller->band = LOCK_BAND_MID;
      }
      break;
    case LOCK_BAND_MID:
    default:
      if (reference_frequency_hz < LOCK_LOW_ENTER_HZ) {
        controller->band = LOCK_BAND_LOW;
      } else if (reference_frequency_hz > LOCK_HIGH_ENTER_HZ) {
        controller->band = LOCK_BAND_HIGH;
      }
      break;
  }
}

static void LockController_ResetPhaseTracking(
    lock_controller_t *controller)
{
  controller->filtered_phase_error_rad = 0.0f;
  controller->previous_phase_error_rad = 0.0f;
  controller->filtered_frequency_error_hz = 0.0f;
  controller->phase_elapsed_s = 0.0f;
  controller->locked_time_s = 0.0f;
  controller->unlocked_time_s = 0.0f;
  controller->last_frequency_step_hz = 0.0f;
  controller->last_phase_step_rad = 0.0f;
  controller->filter_initialized = false;
  controller->have_previous_phase_error = false;
  controller->fine_mode = false;
  controller->phase_locked = false;
  controller->frequency_hold_mode = false;
}

static void LockController_AnchorFrequency(
    lock_controller_t *controller,
    float frequency_hz)
{
  float old_command = controller->command_frequency_hz;

  controller->coarse_frequency_hz = frequency_hz;
  controller->tracked_frequency_hz = frequency_hz;
  controller->command_frequency_hz = frequency_hz;
  controller->frequency_trim_hz = 0.0f;
  controller->frequency_change_time_s = 0.0f;
  controller->missing_frequency_time_s = 0.0f;
  controller->frequency_initialized = true;
  controller->frequency_change_pending = false;
  LockController_ResetPhaseTracking(controller);
  controller->last_frequency_step_hz =
      (old_command > 0.0f) ? (frequency_hz - old_command) : 0.0f;
}

void LockController_Init(lock_controller_t *controller)
{
  if (controller == NULL) {
    return;
  }

  memset(controller, 0, sizeof(*controller));
  controller->multiplier = 1U;
}

void LockController_ResetLoop(lock_controller_t *controller)
{
  uint8_t multiplier;
  float target_phase;

  if (controller == NULL) {
    return;
  }

  multiplier = controller->multiplier;
  target_phase = controller->target_phase_rad;
  memset(controller, 0, sizeof(*controller));
  controller->multiplier = multiplier;
  controller->target_phase_rad = target_phase;
}

bool LockController_SetMultiplier(lock_controller_t *controller,
                                  uint8_t multiplier)
{
  if ((controller == NULL) ||
      ((multiplier != 1U) && (multiplier != 2U))) {
    return false;
  }

  if (controller->multiplier != multiplier) {
    controller->multiplier = multiplier;
    LockController_ResetLoop(controller);
  }
  return true;
}

void LockController_SetTargetPhaseDeg(lock_controller_t *controller,
                                      float phase_deg)
{
  if (controller == NULL) {
    return;
  }

  controller->target_phase_rad =
      LockController_Wrap(phase_deg * LOCK_PI_F / 180.0f);
  /*
   * Keep the learned frequency when only the requested phase changes.
   * Clearing the frequency integrator here creates an avoidable cycle slip.
   */
  LockController_ResetPhaseTracking(controller);
}

uint8_t LockController_GetMultiplier(const lock_controller_t *controller)
{
  return (controller == NULL) ? 1U : controller->multiplier;
}

float LockController_GetTargetPhaseDeg(const lock_controller_t *controller)
{
  if (controller == NULL) {
    return 0.0f;
  }
  return controller->target_phase_rad * 180.0f / LOCK_PI_F;
}

void LockController_Step(lock_controller_t *controller,
                         const phase_measurement_t *measurement,
                         float elapsed_seconds,
                         lock_controller_output_t *output)
{
  float observed_frequency;
  float desired_rate;
  float change_threshold;
  float stable_threshold;
  float multiplier_scale;
  float coarse_reference_frequency;
  float alpha;
  const lock_band_parameters_t *parameters;

  if (output != NULL) {
    memset(output, 0, sizeof(*output));
  }
  if ((controller == NULL) || (measurement == NULL) || (output == NULL)) {
    return;
  }
  output->band = controller->band;

  if (elapsed_seconds < 0.00002f) {
    elapsed_seconds = 0.00002f;
  } else if (elapsed_seconds > 0.050f) {
    elapsed_seconds = 0.050f;
  }

  if (!measurement->frequency_valid) {
    controller->missing_frequency_time_s += elapsed_seconds;
    if (controller->missing_frequency_time_s >=
        LOCK_FREQUENCY_MISSING_RESET_S) {
      LockController_ResetLoop(controller);
    }
    return;
  }

  controller->missing_frequency_time_s = 0.0f;
  observed_frequency =
      (float)controller->multiplier *
      measurement->reference_frequency_hz;
  if ((observed_frequency <= 0.0f) ||
      (observed_frequency > LOCK_MAX_DDS_FREQUENCY_HZ)) {
    return;
  }

  LockController_UpdateBand(
      controller, measurement->reference_frequency_hz,
      !controller->frequency_initialized);
  parameters = &s_band_parameters[(uint32_t)controller->band];
  output->band = controller->band;

  if (!controller->frequency_initialized) {
    LockController_AnchorFrequency(controller, observed_frequency);
  } else {
    alpha = elapsed_seconds /
            (parameters->frequency_track_tau_s + elapsed_seconds);
    controller->tracked_frequency_hz +=
        alpha * (observed_frequency - controller->tracked_frequency_hz);

    /*
     * Define source-change thresholds in the reference-input domain, then
     * scale them into the DDS domain used by the controller.  Otherwise MUL 2
     * halves the effective input tolerance and can keep a noisy source in
     * CHANGE_PENDING indefinitely.
     */
    multiplier_scale = (float)controller->multiplier;
    coarse_reference_frequency =
        controller->coarse_frequency_hz / multiplier_scale;
    change_threshold =
        coarse_reference_frequency *
        LOCK_INPUT_CHANGE_FRACTION;
    if (change_threshold < LOCK_INPUT_CHANGE_MIN_HZ) {
      change_threshold = LOCK_INPUT_CHANGE_MIN_HZ;
    }
    change_threshold *= multiplier_scale;

    stable_threshold =
        coarse_reference_frequency *
        LOCK_INPUT_STABLE_FRACTION;
    if (stable_threshold < LOCK_INPUT_STABLE_MIN_HZ) {
      stable_threshold = LOCK_INPUT_STABLE_MIN_HZ;
    }
    if ((controller->band == LOCK_BAND_LOW) &&
        (stable_threshold > LOCK_LOW_INPUT_STABLE_HZ)) {
      stable_threshold = LOCK_LOW_INPUT_STABLE_HZ;
    }
    stable_threshold *= multiplier_scale;

    if (!controller->frequency_change_pending) {
      if (fabsf(controller->tracked_frequency_hz -
                controller->coarse_frequency_hz) >
          change_threshold) {
        /*
         * Enter a distinct source-change episode.  Do not repeatedly anchor
         * while a bench generator slews through intermediate frequencies.
         */
        controller->frequency_change_pending = true;
        controller->frequency_change_time_s = 0.0f;
        controller->frequency_change_candidate_hz =
            observed_frequency;
        LockController_ResetPhaseTracking(controller);
      }
    } else if (fabsf(controller->tracked_frequency_hz -
                     controller->coarse_frequency_hz) <=
               (0.5f * change_threshold)) {
      /* A short estimator excursion returned inside the loop capture range. */
      controller->frequency_change_pending = false;
      controller->frequency_change_time_s = 0.0f;
      LockController_ResetPhaseTracking(controller);
    } else {
      if (fabsf(observed_frequency -
                controller->frequency_change_candidate_hz) <=
          stable_threshold) {
        controller->frequency_change_time_s += elapsed_seconds;
      } else {
        /*
         * Restart the stability window around the newest frequency.  A slow
         * ramp therefore produces one final anchor only after it stops.
         */
        controller->frequency_change_candidate_hz =
            observed_frequency;
        controller->frequency_change_time_s = 0.0f;
      }

      if (controller->frequency_change_time_s >=
          parameters->input_change_hold_s) {
        LockController_AnchorFrequency(controller,
                                       observed_frequency);
        output->frequency_reanchored = true;
      }
    }
  }

  desired_rate =
      observed_frequency * PLL_SAMPLES_PER_DDS_CYCLE;
  if (desired_rate < (float)PLL_MIN_SAMPLE_RATE_HZ) {
    desired_rate = (float)PLL_MIN_SAMPLE_RATE_HZ;
  } else if (desired_rate > (float)PLL_MAX_SAMPLE_RATE_HZ) {
    desired_rate = (float)PLL_MAX_SAMPLE_RATE_HZ;
  }

  output->command_valid = true;
  output->requested_sample_rate_hz =
      (uint32_t)(desired_rate + 0.5f);
  output->dds_frequency_hz = controller->command_frequency_hz;
  output->dds_phase_offset_rad = controller->phase_offset_rad;
  output->phase_error_rad =
      controller->filtered_phase_error_rad;
  output->direct_phase_mode =
      controller->band == LOCK_BAND_LOW;
  output->frequency_hold_mode =
      controller->frequency_hold_mode;
  output->frequency_change_pending =
      controller->frequency_change_pending;

  controller->phase_elapsed_s += elapsed_seconds;
  if (!measurement->phase_valid) {
    controller->locked_time_s = 0.0f;
    controller->unlocked_time_s += elapsed_seconds;
    if (controller->unlocked_time_s >= LOCK_RELEASE_TIME_S) {
      controller->phase_locked = false;
      controller->frequency_hold_mode = false;
    }
    output->phase_locked = controller->phase_locked;
    output->frequency_hold_mode =
        controller->frequency_hold_mode;
    return;
  }

  /*
   * The demodulator deliberately reuses its previous phase between DFT
   * windows.  Updating the loop only for a fresh phase prevents one noisy
   * high-frequency estimate from being integrated many times.
   */
  if (measurement->phase_updated) {
    float phase_dt = controller->phase_elapsed_s;
    float error = LockController_Wrap(
        measurement->generalized_phase_rad -
        controller->target_phase_rad);
    float desired_frequency;
    float gradient;
    float maximum_step;
    float maximum_trim;
    float step;
    float phase_step = 0.0f;
    float phase_rate_correction_hz = 0.0f;
    bool high_frequency_hold;

    if (controller->frequency_hold_mode) {
      if (!controller->phase_locked ||
          controller->frequency_change_pending ||
          (measurement->reference_frequency_hz <
           LOCK_HIGH_HOLD_EXIT_HZ)) {
        controller->frequency_hold_mode = false;
      }
    } else if (controller->phase_locked &&
               !controller->frequency_change_pending &&
               (controller->locked_time_s >=
                LOCK_HIGH_HOLD_SETTLE_S) &&
               (measurement->reference_frequency_hz >=
                LOCK_HIGH_HOLD_INPUT_HZ)) {
      controller->frequency_hold_mode = true;
    }
    high_frequency_hold = controller->frequency_hold_mode;

    controller->phase_elapsed_s = 0.0f;
    if (phase_dt < 0.00010f) {
      phase_dt = 0.00010f;
    } else if (phase_dt > 0.050f) {
      phase_dt = 0.050f;
    }

    if (!controller->filter_initialized) {
      controller->filtered_phase_error_rad = error;
      controller->filter_initialized = true;
    } else {
      float delta = LockController_Wrap(
          error - controller->filtered_phase_error_rad);
      float phase_filter_tau_s =
          high_frequency_hold
              ? LOCK_HIGH_HOLD_FILTER_TAU_S
              : parameters->phase_filter_tau_s;

      alpha = phase_dt / (phase_filter_tau_s + phase_dt);
      controller->filtered_phase_error_rad =
          LockController_Wrap(
              controller->filtered_phase_error_rad + alpha * delta);
    }

    if (controller->have_previous_phase_error) {
      float phase_delta =
          error - controller->previous_phase_error_rad;
      float rate_filter_tau_s = LOCK_PHASE_RATE_FILTER_TAU_S;
      float instant_frequency_error_hz;

      if (controller->band == LOCK_BAND_LOW) {
        /*
         * The phase measured now already includes the POW correction applied
         * after the previous window.  Remove that known phase actuation so a
         * static phase acquisition is not mistaken for frequency error.
         */
        phase_delta -= controller->last_phase_step_rad;
        rate_filter_tau_s = LOCK_LOW_RATE_FILTER_TAU_S;
      }
      phase_delta = LockController_Wrap(phase_delta);
      instant_frequency_error_hz =
          phase_delta / (LOCK_TWO_PI_F * phase_dt);

      instant_frequency_error_hz =
          LockController_Clamp(instant_frequency_error_hz,
                               LOCK_PHASE_RATE_LIMIT_HZ);
      alpha = phase_dt /
              (rate_filter_tau_s + phase_dt);
      controller->filtered_frequency_error_hz +=
          alpha * (instant_frequency_error_hz -
                   controller->filtered_frequency_error_hz);
    }
    controller->previous_phase_error_rad = error;
    controller->have_previous_phase_error = true;

    if (controller->band == LOCK_BAND_LOW) {
      /*
       * Transfer persistent POW rotation into FTW trim.  This makes POW settle
       * instead of endlessly chasing a small coarse-frequency bias.
       */
      alpha = phase_dt /
              (LOCK_LOW_RATE_LEARN_TAU_S + phase_dt);
      controller->frequency_trim_hz -=
          alpha * controller->filtered_frequency_error_hz;
    } else {
      phase_rate_correction_hz =
          LOCK_PHASE_RATE_GAIN *
          controller->filtered_frequency_error_hz;
    }

    maximum_trim = controller->coarse_frequency_hz * 0.002f;
    if (maximum_trim < 200.0f) {
      maximum_trim = 200.0f;
    }
    /*
     * In direct-POW mode, integrating a large acquisition phase error as a
     * frequency error creates trim wind-up.  Learn frequency only near the
     * requested phase; POW handles the large initial phase displacement.
     */
    if (!high_frequency_hold &&
        ((controller->band != LOCK_BAND_LOW) ||
         (fabsf(controller->filtered_phase_error_rad) <=
          LOCK_FINE_PHASE_RAD))) {
      controller->frequency_trim_hz -=
          parameters->integral_hz_per_rad_s *
          controller->filtered_phase_error_rad * phase_dt;
    }
    controller->frequency_trim_hz =
        LockController_Clamp(controller->frequency_trim_hz,
                             maximum_trim);

    desired_frequency =
        controller->coarse_frequency_hz +
        controller->frequency_trim_hz -
        (parameters->proportional_hz_per_rad *
         controller->filtered_phase_error_rad) -
        phase_rate_correction_hz;
    gradient =
        desired_frequency - controller->command_frequency_hz;

    if (high_frequency_hold) {
      float phase_actuation_error =
          controller->filtered_phase_error_rad;

      controller->fine_mode = true;
      step = 0.0f;
      if (phase_actuation_error > LOCK_HIGH_HOLD_PHASE_DEADBAND_RAD) {
        phase_actuation_error -= LOCK_HIGH_HOLD_PHASE_DEADBAND_RAD;
      } else if (phase_actuation_error <
                 -LOCK_HIGH_HOLD_PHASE_DEADBAND_RAD) {
        phase_actuation_error += LOCK_HIGH_HOLD_PHASE_DEADBAND_RAD;
      } else {
        phase_actuation_error = 0.0f;
      }
      phase_step = LockController_Clamp(
          -LOCK_HIGH_HOLD_PHASE_GAIN * phase_actuation_error,
          LOCK_HIGH_HOLD_PHASE_STEP_RAD);
    } else {
      controller->fine_mode =
          (fabsf(controller->filtered_phase_error_rad) <=
           LOCK_FINE_PHASE_RAD) &&
          (fabsf(gradient) <= LOCK_FINE_GRADIENT_HZ);
      if (controller->fine_mode) {
        maximum_step = LOCK_FINE_STEP_HZ;
      } else if (fabsf(controller->filtered_phase_error_rad) <=
                 LOCK_MID_PHASE_RAD) {
        maximum_step = parameters->mid_step_hz;
      } else {
        maximum_step = parameters->coarse_step_hz;
      }
      step = LockController_Clamp(gradient, maximum_step);
    }

    controller->command_frequency_hz += step;
    if (controller->command_frequency_hz < 1.0f) {
      controller->command_frequency_hz = 1.0f;
    } else if (controller->command_frequency_hz >
               LOCK_MAX_DDS_FREQUENCY_HZ) {
      controller->command_frequency_hz =
          LOCK_MAX_DDS_FREQUENCY_HZ;
    }
    controller->last_frequency_step_hz = step;

    if (high_frequency_hold) {
      controller->phase_offset_rad = LockController_Wrap(
          controller->phase_offset_rad + phase_step);
    } else if (controller->band == LOCK_BAND_LOW) {
      float maximum_phase_step;

      if (controller->fine_mode) {
        maximum_phase_step = LOCK_DIRECT_FINE_STEP_RAD;
      } else if (fabsf(controller->filtered_phase_error_rad) <=
                 LOCK_MID_PHASE_RAD) {
        maximum_phase_step = LOCK_DIRECT_MID_STEP_RAD;
      } else {
        maximum_phase_step = LOCK_DIRECT_COARSE_STEP_RAD;
      }
      phase_step = LockController_Clamp(
          -LOCK_DIRECT_PHASE_GAIN *
              controller->filtered_phase_error_rad,
          maximum_phase_step);
      controller->phase_offset_rad = LockController_Wrap(
          controller->phase_offset_rad + phase_step);
    }
    controller->last_phase_step_rad = phase_step;

    if ((fabsf(error) <= LOCK_ACQUIRE_THRESHOLD_RAD) &&
        (fabsf(step) <= (2.0f * LOCK_FINE_STEP_HZ)) &&
        (fabsf(phase_step) <= LOCK_DIRECT_LOCK_STEP_RAD)) {
      controller->locked_time_s += phase_dt;
      controller->unlocked_time_s = 0.0f;
      if (controller->locked_time_s >= LOCK_ACQUIRE_TIME_S) {
        controller->phase_locked = true;
      }
    } else if (fabsf(error) >= LOCK_RELEASE_THRESHOLD_RAD) {
      controller->unlocked_time_s += phase_dt;
      controller->locked_time_s = 0.0f;
      if (controller->unlocked_time_s >= LOCK_RELEASE_TIME_S) {
        controller->phase_locked = false;
        controller->frequency_hold_mode = false;
      }
    } else {
      controller->locked_time_s = 0.0f;
    }
  }

  output->dds_frequency_hz = controller->command_frequency_hz;
  output->dds_phase_offset_rad = controller->phase_offset_rad;
  output->frequency_step_hz =
      controller->last_frequency_step_hz;
  output->phase_step_rad = controller->last_phase_step_rad;
  output->phase_error_rad =
      controller->filtered_phase_error_rad;
  output->fine_mode = controller->fine_mode;
  output->direct_phase_mode =
      controller->band == LOCK_BAND_LOW;
  output->frequency_hold_mode =
      controller->frequency_hold_mode;
  output->frequency_change_pending =
      controller->frequency_change_pending;
  output->band = controller->band;
  output->phase_locked = controller->phase_locked;
}
