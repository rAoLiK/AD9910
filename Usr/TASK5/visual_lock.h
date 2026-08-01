#ifndef VISUAL_LOCK_H
#define VISUAL_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Permanent camera-assisted Lissajous frequency lock.
 *
 * The camera supplies an unsigned image-motion speed.  The controller first
 * probes both frequency directions, then adjusts the DDS in 0.01 Hz steps.
 * Every command is hard-limited to +/-5 Hz around the requested-image seed.
 * The requested DDS phase is fixed for the complete session; visual feedback
 * is never allowed to turn a circle or a 2:1 figure into a diagonal line.
 */
typedef enum {
  VISUAL_LOCK_IDLE = 0,
  VISUAL_LOCK_BASELINE,
  VISUAL_LOCK_PROBE_POSITIVE,
  VISUAL_LOCK_PROBE_NEGATIVE,
  VISUAL_LOCK_TRACK_FREQUENCY,
  VISUAL_LOCK_LOCKED,
  VISUAL_LOCK_ERROR
} visual_lock_state_t;

enum {
  VISUAL_LOCK_SAMPLE_PHASE_VALID = 0x01U,
  VISUAL_LOCK_SAMPLE_FAMILY_VALID = 0x02U
};

typedef struct {
  uint32_t timestamp_ms;
  uint32_t phase_mdeg;
  uint16_t speed_millihz;
  uint8_t quality;
  uint8_t flags;
} visual_lock_sample_t;

typedef struct {
  bool command_changed;
  bool locked;
  bool error;
  uint32_t frequency_millihz;
  int32_t phase_offset_mdeg;
} visual_lock_output_t;

typedef struct {
  visual_lock_state_t state;
  uint32_t seed_frequency_millihz;
  uint32_t probe_center_millihz;
  uint32_t command_frequency_millihz;
  uint32_t best_frequency_millihz;
  int32_t phase_offset_mdeg;
  int32_t frequency_direction;
  uint32_t settle_deadline_ms;
  uint32_t accumulated_speed_millihz;
  uint16_t baseline_speed_millihz;
  uint16_t positive_probe_speed_millihz;
  uint16_t best_speed_millihz;
  uint8_t observation_count;
  uint8_t worsening_count;
  uint8_t frequency_stable_count;
  uint8_t unlock_count;
  uint8_t direction_reversal_count;
  uint8_t boundary_reversal_count;
  uint8_t valid_sample_count;
  uint8_t rejected_sample_count;
} visual_lock_controller_t;

void VisualLock_InitTarget(visual_lock_controller_t *controller,
                           uint32_t seed_frequency_millihz,
                           int32_t target_phase_offset_mdeg,
                           uint32_t now_ms);
void VisualLock_Init(visual_lock_controller_t *controller,
                     uint32_t seed_frequency_millihz,
                     uint32_t now_ms);
void VisualLock_Reset(visual_lock_controller_t *controller);
void VisualLock_Step(visual_lock_controller_t *controller,
                     const visual_lock_sample_t *sample,
                     visual_lock_output_t *output);

#ifdef __cplusplus
}
#endif

#endif /* VISUAL_LOCK_H */
