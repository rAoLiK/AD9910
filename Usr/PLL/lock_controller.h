#ifndef LOCK_CONTROLLER_H
#define LOCK_CONTROLLER_H

#include "phase_detector.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool command_valid;
  bool phase_locked;
  float dds_frequency_hz;
  uint32_t requested_sample_rate_hz;
  float phase_error_rad;
} lock_controller_output_t;

typedef struct {
  uint8_t multiplier;
  float target_phase_rad;
  float filtered_phase_error_rad;
  float frequency_trim_hz;
  float locked_time_s;
  float unlocked_time_s;
  bool filter_initialized;
  bool phase_locked;
} lock_controller_t;

void LockController_Init(lock_controller_t *controller);
bool LockController_SetMultiplier(lock_controller_t *controller,
                                  uint8_t multiplier);
void LockController_SetTargetPhaseDeg(lock_controller_t *controller,
                                      float phase_deg);
uint8_t LockController_GetMultiplier(const lock_controller_t *controller);
float LockController_GetTargetPhaseDeg(const lock_controller_t *controller);
void LockController_ResetLoop(lock_controller_t *controller);
void LockController_Step(lock_controller_t *controller,
                         const phase_measurement_t *measurement,
                         float elapsed_seconds,
                         lock_controller_output_t *output);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_CONTROLLER_H */
