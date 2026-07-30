#ifndef LOCK_CONTROLLER_H
#define LOCK_CONTROLLER_H

#include "phase_detector.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LOCK_BAND_LOW = 0,
  LOCK_BAND_MID,
  LOCK_BAND_HIGH
} lock_band_t;

typedef struct {
  bool command_valid;
  bool phase_locked;
  bool fine_mode;
  bool direct_phase_mode;
  bool frequency_hold_mode;
  /* One-shot event: a confirmed source-frequency change was re-anchored. */
  bool frequency_reanchored;
  /* True while the source is still moving and has not settled for re-lock. */
  bool frequency_change_pending;
  lock_band_t band;
  float dds_frequency_hz;
  float dds_phase_offset_rad;
  float frequency_step_hz;
  float phase_step_rad;
  uint32_t requested_sample_rate_hz;
  float phase_error_rad;
} lock_controller_output_t;

typedef struct {
  uint8_t multiplier;
  lock_band_t band;
  float target_phase_rad;
  float coarse_frequency_hz;
  float tracked_frequency_hz;
  float command_frequency_hz;
  float phase_offset_rad;
  float filtered_phase_error_rad;
  float previous_phase_error_rad;
  float filtered_frequency_error_hz;
  float frequency_trim_hz;
  float frequency_change_candidate_hz;
  float phase_elapsed_s;
  float frequency_change_time_s;
  float missing_frequency_time_s;
  float locked_time_s;
  float unlocked_time_s;
  float hold_settle_time_s;
  float last_frequency_step_hz;
  float last_phase_step_rad;
  bool frequency_initialized;
  bool frequency_change_pending;
  bool filter_initialized;
  bool have_previous_phase_error;
  bool fine_mode;
  bool phase_locked;
  bool frequency_hold_mode;
} lock_controller_t;

void LockController_Init(lock_controller_t *controller);
bool LockController_SetMultiplier(lock_controller_t *controller,
                                  uint8_t multiplier);
void LockController_SetTargetPhaseDeg(lock_controller_t *controller,
                                      float phase_deg);
/* Update a slowly varying calibrated target without resetting loop state. */
void LockController_TrackTargetPhaseDeg(lock_controller_t *controller,
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
