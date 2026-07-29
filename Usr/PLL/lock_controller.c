#include "lock_controller.h"

#include "pll_config.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define LOCK_PI_F                         (3.14159265358979323846f)
#define LOCK_TWO_PI_F                     (2.0f * LOCK_PI_F)
#define LOCK_PHASE_FILTER_TAU_S           (0.010f)
#define LOCK_PROPORTIONAL_HZ_PER_RAD      (20.0f)
#define LOCK_INTEGRAL_HZ_PER_RAD_S        (80.0f)
#define LOCK_ACQUIRE_THRESHOLD_RAD        (3.0f * LOCK_PI_F / 180.0f)
#define LOCK_RELEASE_THRESHOLD_RAD        (12.0f * LOCK_PI_F / 180.0f)
#define LOCK_ACQUIRE_TIME_S               (0.050f)
#define LOCK_RELEASE_TIME_S               (0.030f)
#define LOCK_MAX_DDS_FREQUENCY_HZ         (400000000.0f)

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
  if (controller == NULL) {
    return;
  }

  controller->filtered_phase_error_rad = 0.0f;
  controller->frequency_trim_hz = 0.0f;
  controller->locked_time_s = 0.0f;
  controller->unlocked_time_s = 0.0f;
  controller->filter_initialized = false;
  controller->phase_locked = false;
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
  LockController_ResetLoop(controller);
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
  float base_frequency;
  float desired_rate;
  float maximum_trim;
  float alpha;
  float error;

  if (output != NULL) {
    memset(output, 0, sizeof(*output));
  }
  if ((controller == NULL) || (measurement == NULL) || (output == NULL) ||
      !measurement->frequency_valid) {
    return;
  }

  if (elapsed_seconds < 0.00005f) {
    elapsed_seconds = 0.00005f;
  } else if (elapsed_seconds > 0.25f) {
    elapsed_seconds = 0.25f;
  }

  base_frequency =
      (float)controller->multiplier *
      measurement->reference_frequency_hz;
  if ((base_frequency <= 0.0f) ||
      (base_frequency > LOCK_MAX_DDS_FREQUENCY_HZ)) {
    return;
  }

  desired_rate =
      base_frequency * PLL_SAMPLES_PER_DDS_CYCLE;
  if (desired_rate < (float)PLL_MIN_SAMPLE_RATE_HZ) {
    desired_rate = (float)PLL_MIN_SAMPLE_RATE_HZ;
  } else if (desired_rate > (float)PLL_MAX_SAMPLE_RATE_HZ) {
    desired_rate = (float)PLL_MAX_SAMPLE_RATE_HZ;
  }

  output->command_valid = true;
  output->requested_sample_rate_hz =
      (uint32_t)(desired_rate + 0.5f);
  output->dds_frequency_hz = base_frequency;

  if (!measurement->phase_valid) {
    controller->locked_time_s = 0.0f;
    controller->unlocked_time_s += elapsed_seconds;
    controller->phase_locked = false;
    output->phase_locked = false;
    return;
  }

  error = LockController_Wrap(
      measurement->generalized_phase_rad -
      controller->target_phase_rad);
  if (!controller->filter_initialized) {
    controller->filtered_phase_error_rad = error;
    controller->filter_initialized = true;
  } else {
    float delta = LockController_Wrap(
        error - controller->filtered_phase_error_rad);
    alpha = elapsed_seconds /
            (LOCK_PHASE_FILTER_TAU_S + elapsed_seconds);
    controller->filtered_phase_error_rad =
        LockController_Wrap(
            controller->filtered_phase_error_rad + alpha * delta);
  }

  maximum_trim = 100.0f + (base_frequency * 0.05f);
  controller->frequency_trim_hz -=
      LOCK_INTEGRAL_HZ_PER_RAD_S *
      controller->filtered_phase_error_rad * elapsed_seconds;
  if (controller->frequency_trim_hz > maximum_trim) {
    controller->frequency_trim_hz = maximum_trim;
  } else if (controller->frequency_trim_hz < -maximum_trim) {
    controller->frequency_trim_hz = -maximum_trim;
  }

  output->dds_frequency_hz =
      base_frequency + controller->frequency_trim_hz -
      (LOCK_PROPORTIONAL_HZ_PER_RAD *
       controller->filtered_phase_error_rad);
  if (output->dds_frequency_hz < 1.0f) {
    output->dds_frequency_hz = 1.0f;
  } else if (output->dds_frequency_hz >
             LOCK_MAX_DDS_FREQUENCY_HZ) {
    output->dds_frequency_hz = LOCK_MAX_DDS_FREQUENCY_HZ;
  }
  output->phase_error_rad = controller->filtered_phase_error_rad;

  if (fabsf(error) <= LOCK_ACQUIRE_THRESHOLD_RAD) {
    controller->locked_time_s += elapsed_seconds;
    controller->unlocked_time_s = 0.0f;
    if (controller->locked_time_s >= LOCK_ACQUIRE_TIME_S) {
      controller->phase_locked = true;
    }
  } else if (fabsf(error) >= LOCK_RELEASE_THRESHOLD_RAD) {
    controller->unlocked_time_s += elapsed_seconds;
    controller->locked_time_s = 0.0f;
    if (controller->unlocked_time_s >= LOCK_RELEASE_TIME_S) {
      controller->phase_locked = false;
    }
  }

  output->phase_locked = controller->phase_locked;
}
