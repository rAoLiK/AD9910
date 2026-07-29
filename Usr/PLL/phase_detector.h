#ifndef PHASE_DETECTOR_H
#define PHASE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool signal_valid;
  bool frequency_valid;
  bool phase_valid;
  /* True for exactly one call when a new demodulated phase is produced. */
  bool phase_updated;
  float reference_frequency_hz;
  float generalized_phase_rad;
  float reference_amplitude_counts;
  float dds_amplitude_counts;
  float phase_quality;
} phase_measurement_t;

typedef struct {
  float sample_rate_hz;
  float reference_frequency_hz;
  float dc_reference_counts;
  uint64_t sample_index;
  uint64_t last_crossing_q16;
  uint64_t last_good_crossing_sample;
  uint64_t last_signal_sample;
  uint64_t last_phase_sample;
  uint64_t previous_reference_sample;
  uint32_t valid_period_count;
  float last_phase_rad;
  float last_phase_quality;
  uint16_t previous_reference_raw;
  bool have_previous_sample;
  bool crossing_armed;
  bool have_crossing;
  bool have_phase;
} phase_detector_t;

void PhaseDetector_Init(phase_detector_t *detector, float sample_rate_hz);
void PhaseDetector_SetSampleRate(phase_detector_t *detector,
                                 float sample_rate_hz,
                                 bool retain_frequency);
void PhaseDetector_ResetFrequency(phase_detector_t *detector);

/**
 * Process ADC common-data-register words.
 *
 * Each word is ADC1/PC0 in bits 15:0 and ADC2/PC1 in bits 31:16.
 * The returned generalized phase is phi_ADC2 - multiplier * phi_ADC1,
 * using a sine-wave phase convention.
 */
void PhaseDetector_Process(phase_detector_t *detector,
                           const uint32_t *packed_pairs,
                           uint32_t pair_count,
                           uint8_t multiplier,
                           phase_measurement_t *measurement);

#ifdef __cplusplus
}
#endif

#endif /* PHASE_DETECTOR_H */
