#include "phase_detector.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define PHASE_PI_F                    (3.14159265358979323846f)
#define PHASE_TWO_PI_F                (2.0f * PHASE_PI_F)
#define PHASE_ADC_MIDSCALE_COUNTS     (2048.0f)
#define PHASE_MIN_P2P_COUNTS          (32.0f)
#define PHASE_MIN_FREQUENCY_HZ        (10.0f)
#define PHASE_MAX_NYQUIST_FRACTION    (0.45f)
#define PHASE_SIGNAL_HOLD_SECONDS     (0.12f)
#define PHASE_DEMOD_INTERVAL_SECONDS  (0.00075f)
#define PHASE_ANALYSIS_POINTS_PER_CYCLE (6.0f)
#define PHASE_ANALYSIS_MAX_STRIDE     (8U)
#define PHASE_LOW_BAND_HZ             (40000.0f)
#define PHASE_HIGH_BAND_HZ            (80000.0f)
#define PHASE_LOW_DEMOD_PAIRS         (256U)
#define PHASE_MID_DEMOD_PAIRS         (128U)
#define PHASE_HIGH_DEMOD_PAIRS        (128U)

static float PhaseDetector_Wrap(float radians)
{
  while (radians > PHASE_PI_F) {
    radians -= PHASE_TWO_PI_F;
  }
  while (radians <= -PHASE_PI_F) {
    radians += PHASE_TWO_PI_F;
  }
  return radians;
}

static uint32_t PhaseDetector_SelectAnalysisStride(
    const phase_detector_t *detector)
{
  uint32_t stride = 4U;

  if ((detector->reference_frequency_hz > 0.0f) &&
      (detector->valid_period_count != 0U)) {
    if (detector->reference_frequency_hz >= PHASE_LOW_BAND_HZ) {
      /*
       * Preserve the high-frequency detector cadence from the last
       * board-verified revision: 1/2/4 for <1/1..2/>=2 MSPS.  Using the
       * low-band six-points/cycle rule here reduced 41..83 kHz from 12
       * analysed points per cycle to six and made zero-cross jitter large
       * enough to trigger false source-change reacquisitions.
       */
      if (detector->sample_rate_hz >= 2000000.0f) {
        stride = 4U;
      } else if (detector->sample_rate_hz >= 1000000.0f) {
        stride = 2U;
      } else {
        stride = 1U;
      }
    } else {
      float samples_per_cycle =
          detector->sample_rate_hz /
          detector->reference_frequency_hz;

      stride = (uint32_t)(
          (samples_per_cycle / PHASE_ANALYSIS_POINTS_PER_CYCLE) +
          0.5f);
    }
  }

  if (stride < 1U) {
    stride = 1U;
  } else if (stride > PHASE_ANALYSIS_MAX_STRIDE) {
    stride = PHASE_ANALYSIS_MAX_STRIDE;
  }
  return stride;
}

static uint32_t PhaseDetector_SelectDemodPairs(
    float reference_frequency_hz)
{
  if (reference_frequency_hz < PHASE_LOW_BAND_HZ) {
    return PHASE_LOW_DEMOD_PAIRS;
  }
  if (reference_frequency_hz < PHASE_HIGH_BAND_HZ) {
    return PHASE_MID_DEMOD_PAIRS;
  }
  return PHASE_HIGH_DEMOD_PAIRS;
}

void PhaseDetector_Init(phase_detector_t *detector, float sample_rate_hz)
{
  if (detector == NULL) {
    return;
  }

  memset(detector, 0, sizeof(*detector));
  detector->sample_rate_hz = sample_rate_hz;
  detector->dc_reference_counts = PHASE_ADC_MIDSCALE_COUNTS;
}

void PhaseDetector_ResetFrequency(phase_detector_t *detector)
{
  if (detector == NULL) {
    return;
  }

  detector->reference_frequency_hz = 0.0f;
  detector->valid_period_count = 0U;
  detector->have_crossing = false;
  detector->crossing_armed = false;
  detector->have_previous_sample = false;
  detector->last_crossing_q16 = 0ULL;
  detector->last_good_crossing_sample = detector->sample_index;
  detector->last_phase_sample = detector->sample_index;
  detector->last_phase_rad = 0.0f;
  detector->last_phase_quality = 0.0f;
  detector->have_phase = false;
}

void PhaseDetector_SetSampleRate(phase_detector_t *detector,
                                 float sample_rate_hz,
                                 bool retain_frequency)
{
  float retained = 0.0f;
  uint32_t retained_periods = 0U;

  if ((detector == NULL) || (sample_rate_hz <= 0.0f)) {
    return;
  }

  if (retain_frequency) {
    retained = detector->reference_frequency_hz;
    retained_periods = detector->valid_period_count;
  }

  detector->sample_rate_hz = sample_rate_hz;
  detector->sample_index = 0ULL;
  detector->last_signal_sample = 0ULL;
  PhaseDetector_ResetFrequency(detector);
  detector->reference_frequency_hz = retained;
  detector->valid_period_count = retained_periods;
}

static void PhaseDetector_EstimateFrequency(phase_detector_t *detector,
                                            const uint32_t *packed_pairs,
                                            uint32_t pair_count,
                                            uint32_t analysis_stride,
                                            float hysteresis_counts)
{
  uint64_t interval_sum_q16 = 0ULL;
  uint32_t interval_count = 0U;
  uint32_t i;

  for (i = 0U; i < pair_count; i += analysis_stride) {
    uint16_t raw = (uint16_t)(packed_pairs[i] & 0x0FFFU);
    float centered = (float)raw - detector->dc_reference_counts;
    uint64_t current_sample =
        detector->sample_index + (uint64_t)i;

    if (centered <= -hysteresis_counts) {
      detector->crossing_armed = true;
    }

    if (detector->have_previous_sample && detector->crossing_armed) {
      float previous =
          (float)detector->previous_reference_raw -
          detector->dc_reference_counts;
      if ((previous < 0.0f) && (centered >= 0.0f)) {
        float denominator = centered - previous;
        float fraction = 0.0f;
        uint64_t crossing_q16;

        if (denominator > 0.0f) {
          fraction = -previous / denominator;
        }
        if (fraction < 0.0f) {
          fraction = 0.0f;
        } else if (fraction > 1.0f) {
          fraction = 1.0f;
        }

        crossing_q16 =
            (detector->previous_reference_sample << 16U) +
            (uint64_t)(
                fraction *
                (float)(current_sample -
                        detector->previous_reference_sample) *
                65536.0f);

        if (detector->have_crossing &&
            (crossing_q16 > detector->last_crossing_q16)) {
          uint64_t delta_q16 =
              crossing_q16 - detector->last_crossing_q16;
          float frequency =
              (detector->sample_rate_hz * 65536.0f) /
              (float)delta_q16;

          if ((frequency >= PHASE_MIN_FREQUENCY_HZ) &&
              (frequency <=
               (detector->sample_rate_hz *
                PHASE_MAX_NYQUIST_FRACTION))) {
            interval_sum_q16 += delta_q16;
            interval_count++;
            detector->last_good_crossing_sample =
                crossing_q16 >> 16U;
          }
        }

        detector->last_crossing_q16 = crossing_q16;
        detector->have_crossing = true;
        detector->crossing_armed = false;
      }
    }

    detector->previous_reference_raw = raw;
    detector->previous_reference_sample = current_sample;
    detector->have_previous_sample = true;
  }

  if ((interval_count != 0U) && (interval_sum_q16 != 0ULL)) {
    float instant =
        (detector->sample_rate_hz * 65536.0f *
         (float)interval_count) /
        (float)interval_sum_q16;

    if ((detector->valid_period_count == 0U) ||
        ((interval_count >= 2U) &&
         (fabsf(instant - detector->reference_frequency_hz) >
          (0.15f * detector->reference_frequency_hz)))) {
      detector->reference_frequency_hz = instant;
      detector->have_phase = false;
    } else {
      /* A restrained IIR prevents FTW jitter from ADC quantization/noise. */
      detector->reference_frequency_hz +=
          0.18f * (instant - detector->reference_frequency_hz);
    }

    if (detector->valid_period_count <=
        (UINT32_MAX - interval_count)) {
      detector->valid_period_count += interval_count;
    }
  }
}

static bool PhaseDetector_Demodulate(const uint32_t *packed_pairs,
                                     uint32_t pair_count,
                                     float sample_rate_hz,
                                     float reference_frequency_hz,
                                     uint8_t multiplier,
                                     float mean_reference,
                                     float mean_dds,
                                     float reference_amplitude,
                                     float dds_amplitude,
                                     float *phase_rad,
                                     float *quality)
{
  float reference_step;
  float dds_step;
  float reference_cos_step;
  float reference_sin_step;
  float dds_cos_step;
  float dds_sin_step;
  float reference_cos = 1.0f;
  float reference_sin = 0.0f;
  float dds_cos = 1.0f;
  float dds_sin = 0.0f;
  float reference_re = 0.0f;
  float reference_im = 0.0f;
  float dds_re = 0.0f;
  float dds_im = 0.0f;
  float weight_sum = 0.0f;
  uint32_t i;

  if ((pair_count < 8U) || (phase_rad == NULL) || (quality == NULL) ||
      (reference_frequency_hz <= 0.0f) ||
      (((float)multiplier * reference_frequency_hz) >=
       (sample_rate_hz * PHASE_MAX_NYQUIST_FRACTION))) {
    return false;
  }

  /* Require enough time aperture to reject DC and the image component. */
  if ((reference_frequency_hz * (float)pair_count / sample_rate_hz) < 1.5f) {
    return false;
  }

  reference_step =
      PHASE_TWO_PI_F * reference_frequency_hz / sample_rate_hz;
  dds_step = reference_step * (float)multiplier;
  reference_cos_step = cosf(reference_step);
  reference_sin_step = sinf(reference_step);
  dds_cos_step = cosf(dds_step);
  dds_sin_step = sinf(dds_step);

  for (i = 0U; i < pair_count; ++i) {
    float weight =
        (i < (pair_count / 2U)) ? (float)(i + 1U)
                               : (float)(pair_count - i);
    float reference =
        (float)(packed_pairs[i] & 0x0FFFU) - mean_reference;
    float dds =
        (float)((packed_pairs[i] >> 16U) & 0x0FFFU) - mean_dds;
    float next_cos;

    reference_re += weight * reference * reference_cos;
    reference_im -= weight * reference * reference_sin;
    dds_re += weight * dds * dds_cos;
    dds_im -= weight * dds * dds_sin;
    weight_sum += weight;

    next_cos = reference_cos * reference_cos_step -
               reference_sin * reference_sin_step;
    reference_sin = reference_sin * reference_cos_step +
                    reference_cos * reference_sin_step;
    reference_cos = next_cos;

    next_cos = dds_cos * dds_cos_step - dds_sin * dds_sin_step;
    dds_sin = dds_sin * dds_cos_step + dds_cos * dds_sin_step;
    dds_cos = next_cos;
  }

  if (weight_sum > 0.0f) {
    float reference_magnitude =
        2.0f * hypotf(reference_re, reference_im) / weight_sum;
    float dds_magnitude =
        2.0f * hypotf(dds_re, dds_im) / weight_sum;
    float reference_phase =
        atan2f(reference_im, reference_re) + (PHASE_PI_F / 2.0f);
    float dds_phase =
        atan2f(dds_im, dds_re) + (PHASE_PI_F / 2.0f);
    float reference_ratio =
        reference_magnitude / (reference_amplitude + 1.0f);
    float dds_ratio = dds_magnitude / (dds_amplitude + 1.0f);

    *quality =
        (reference_ratio < dds_ratio) ? reference_ratio : dds_ratio;
    *phase_rad = PhaseDetector_Wrap(
        dds_phase - ((float)multiplier * reference_phase));

    return (reference_magnitude >= (PHASE_MIN_P2P_COUNTS / 2.0f)) &&
           (dds_magnitude >= (PHASE_MIN_P2P_COUNTS / 2.0f)) &&
           (*quality >= 0.20f);
  }

  return false;
}

void PhaseDetector_Process(phase_detector_t *detector,
                           const uint32_t *packed_pairs,
                           uint32_t pair_count,
                           uint8_t multiplier,
                           phase_measurement_t *measurement)
{
  uint32_t sum_reference = 0U;
  uint32_t sum_dds = 0U;
  uint16_t min_reference = 4095U;
  uint16_t max_reference = 0U;
  uint16_t min_dds = 4095U;
  uint16_t max_dds = 0U;
  float mean_reference;
  float mean_dds;
  float p2p_reference;
  float p2p_dds;
  float hysteresis;
  uint64_t stale_samples;
  uint32_t analysis_stride;
  uint32_t analysis_count = 0U;
  uint32_t i;

  if (measurement != NULL) {
    memset(measurement, 0, sizeof(*measurement));
  }
  if ((detector == NULL) || (packed_pairs == NULL) ||
      (pair_count == 0U) || (measurement == NULL) ||
      ((multiplier != 1U) && (multiplier != 2U)) ||
      (detector->sample_rate_hz <= 0.0f)) {
    return;
  }

  /*
   * Select the decimation from samples-per-cycle instead of absolute Fs.
   * With dynamic Fs this keeps CPU load continuous across 1 MHz (about
   * 41.7 kHz input) and retains roughly six points per reference cycle.
   */
  analysis_stride =
      PhaseDetector_SelectAnalysisStride(detector);
  measurement->analysis_stride = (uint8_t)analysis_stride;
  for (i = 0U; i < pair_count; i += analysis_stride) {
    uint16_t reference = (uint16_t)(packed_pairs[i] & 0x0FFFU);
    uint16_t dds = (uint16_t)((packed_pairs[i] >> 16U) & 0x0FFFU);

    analysis_count++;
    sum_reference += reference;
    sum_dds += dds;
    if (reference < min_reference) {
      min_reference = reference;
    }
    if (reference > max_reference) {
      max_reference = reference;
    }
    if (dds < min_dds) {
      min_dds = dds;
    }
    if (dds > max_dds) {
      max_dds = dds;
    }
  }

  mean_reference = (float)sum_reference / (float)analysis_count;
  mean_dds = (float)sum_dds / (float)analysis_count;
  p2p_reference = (float)(max_reference - min_reference);
  p2p_dds = (float)(max_dds - min_dds);
  measurement->reference_amplitude_counts = p2p_reference * 0.5f;
  measurement->dds_amplitude_counts = p2p_dds * 0.5f;

  if ((p2p_reference >= PHASE_MIN_P2P_COUNTS) &&
      (p2p_dds >= PHASE_MIN_P2P_COUNTS)) {
    detector->last_signal_sample =
        detector->sample_index + (uint64_t)pair_count;
  }

  hysteresis = p2p_reference / 16.0f;
  if (hysteresis < 8.0f) {
    hysteresis = 8.0f;
  } else if (hysteresis > 128.0f) {
    hysteresis = 128.0f;
  }

  PhaseDetector_EstimateFrequency(detector, packed_pairs, pair_count,
                                  analysis_stride, hysteresis);

  detector->sample_index += (uint64_t)pair_count;
  stale_samples =
      (uint64_t)(detector->sample_rate_hz * PHASE_SIGNAL_HOLD_SECONDS);
  measurement->signal_valid =
      (detector->sample_index - detector->last_signal_sample) <=
      stale_samples;
  measurement->frequency_valid =
      (detector->valid_period_count >= 2U) &&
      ((detector->sample_index -
        detector->last_good_crossing_sample) <= stale_samples);
  measurement->reference_frequency_hz =
      detector->reference_frequency_hz;

  if (measurement->frequency_valid) {
    float cycles =
        detector->reference_frequency_hz * (float)pair_count /
        detector->sample_rate_hz;
    uint64_t demod_interval =
        (uint64_t)(detector->sample_rate_hz *
                   PHASE_DEMOD_INTERVAL_SECONDS);

    /* Once complete cycles are present, trim small analogue offset errors. */
    if (cycles >= 1.0f) {
      detector->dc_reference_counts +=
          0.02f * (mean_reference - detector->dc_reference_counts);
    }

    if (demod_interval < (uint64_t)pair_count) {
      demod_interval = (uint64_t)pair_count;
    }
    if (!detector->have_phase ||
        ((detector->sample_index - detector->last_phase_sample) >=
         demod_interval)) {
      float phase_rad;
      float quality;
      uint32_t demod_count =
          PhaseDetector_SelectDemodPairs(
              detector->reference_frequency_hz);
      uint32_t minimum_count =
          (uint32_t)(
              (1.75f * detector->sample_rate_hz) /
              detector->reference_frequency_hz) + 1U;

      if (demod_count < minimum_count) {
        demod_count = minimum_count;
      }
      if (demod_count > pair_count) {
        demod_count = pair_count;
      }
      measurement->phase_pair_count = (uint16_t)demod_count;

      detector->have_phase =
          PhaseDetector_Demodulate(
              packed_pairs, demod_count, detector->sample_rate_hz,
              detector->reference_frequency_hz, multiplier,
              mean_reference, mean_dds,
              measurement->reference_amplitude_counts,
              measurement->dds_amplitude_counts,
              &phase_rad, &quality);
      detector->last_phase_sample = detector->sample_index;
      if (detector->have_phase) {
        detector->last_phase_rad = phase_rad;
        detector->last_phase_quality = quality;
        measurement->phase_updated = true;
      }
    }

    measurement->phase_valid =
        detector->have_phase && measurement->signal_valid;
    if (measurement->phase_valid) {
      measurement->generalized_phase_rad =
          detector->last_phase_rad;
      measurement->phase_quality =
          detector->last_phase_quality;
    }
  }
}
