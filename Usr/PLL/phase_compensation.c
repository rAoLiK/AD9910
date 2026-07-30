#include "phase_compensation.h"

#include <math.h>
#include <stddef.h>

typedef struct {
  float reference_frequency_hz;
  float correction_1x_deg;
  float correction_2x_deg;
} phase_compensation_point_t;

/*
 * Source: ref/频率偏差.xlsx, measured with CH1 as the phase reference.
 *
 * Theoretical scope phase is -90 degrees for 1x and 0 degrees for 2x.
 * Each correction below is measured - theoretical and is added to the
 * controller's nominal feedback target.
 */
static const phase_compensation_point_t s_phase_compensation[] = {
    {  1000.0f, -0.13f, 0.10f},
    {  5000.0f,  0.87f, 1.00f},
    { 10000.0f,  1.47f, 1.72f},
    { 15000.0f,  1.76f, 1.77f},
    { 20000.0f,  2.10f, 1.80f},
    { 25000.0f,  2.32f, 1.92f},
    { 30000.0f,  2.40f, 1.99f},
    { 35000.0f,  2.59f, 2.12f},
    { 40000.0f,  2.66f, 2.28f},
    { 45000.0f,  2.47f, 2.31f},
    { 50000.0f,  2.98f, 2.54f},
    { 55000.0f,  4.03f, 3.17f},
    { 60000.0f,  4.45f, 3.56f},
    { 65000.0f,  4.16f, 3.36f},
    { 70000.0f,  3.88f, 3.93f},
    { 75000.0f,  3.34f, 3.78f},
    { 80000.0f,  3.44f, 4.10f},
    { 85000.0f,  3.93f, 4.47f},
    { 90000.0f,  4.03f, 4.81f},
    { 95000.0f,  4.15f, 4.88f},
    {100000.0f,  5.24f, 4.95f}
};

float PhaseCompensation_GetDeg(float reference_frequency_hz,
                               uint8_t multiplier)
{
  const size_t point_count =
      sizeof(s_phase_compensation) /
      sizeof(s_phase_compensation[0]);
  size_t index;

  if (!isfinite(reference_frequency_hz) ||
      ((multiplier != 1U) && (multiplier != 2U))) {
    return 0.0f;
  }
  if (reference_frequency_hz <=
      s_phase_compensation[0].reference_frequency_hz) {
    return (multiplier == 1U)
               ? s_phase_compensation[0].correction_1x_deg
               : s_phase_compensation[0].correction_2x_deg;
  }

  for (index = 1U; index < point_count; ++index) {
    const phase_compensation_point_t *lower =
        &s_phase_compensation[index - 1U];
    const phase_compensation_point_t *upper =
        &s_phase_compensation[index];

    if (reference_frequency_hz <= upper->reference_frequency_hz) {
      float lower_correction =
          (multiplier == 1U)
              ? lower->correction_1x_deg
              : lower->correction_2x_deg;
      float upper_correction =
          (multiplier == 1U)
              ? upper->correction_1x_deg
              : upper->correction_2x_deg;
      float fraction =
          (reference_frequency_hz -
           lower->reference_frequency_hz) /
          (upper->reference_frequency_hz -
           lower->reference_frequency_hz);

      return lower_correction +
             fraction * (upper_correction - lower_correction);
    }
  }

  return (multiplier == 1U)
             ? s_phase_compensation[point_count - 1U].correction_1x_deg
             : s_phase_compensation[point_count - 1U].correction_2x_deg;
}
