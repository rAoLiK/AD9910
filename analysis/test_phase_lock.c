#include "lock_controller.h"
#include "phase_detector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_PI             (3.14159265358979323846)
#define TEST_PAIR_COUNT     (256U)
#define TEST_HIGH_PAIR_COUNT (2048U)

static uint32_t make_pair(double reference, double dds)
{
  long reference_code = lround(2048.0 + 900.0 * reference);
  long dds_code = lround(2048.0 + 850.0 * dds);

  if (reference_code < 0L) {
    reference_code = 0L;
  } else if (reference_code > 4095L) {
    reference_code = 4095L;
  }
  if (dds_code < 0L) {
    dds_code = 0L;
  } else if (dds_code > 4095L) {
    dds_code = 4095L;
  }

  return (uint32_t)reference_code |
         ((uint32_t)dds_code << 16U);
}

static int run_detector_case(uint8_t multiplier,
                             double reference_phase_deg,
                             double dds_phase_deg,
                             double expected_generalized_deg)
{
  phase_detector_t detector;
  phase_measurement_t measurement = {0};
  uint32_t pairs[TEST_PAIR_COUNT];
  const double sample_rate = 240000.0;
  const double frequency = 10000.0;
  uint64_t sample_index = 0U;
  unsigned int block;

  PhaseDetector_Init(&detector, (float)sample_rate);
  for (block = 0U; block < 12U; ++block) {
    uint32_t i;
    for (i = 0U; i < TEST_PAIR_COUNT; ++i, ++sample_index) {
      double reference_phase =
          2.0 * TEST_PI * frequency * (double)sample_index /
              sample_rate +
          reference_phase_deg * TEST_PI / 180.0;
      double dds_phase =
          2.0 * TEST_PI * frequency * (double)multiplier *
              (double)sample_index / sample_rate +
          dds_phase_deg * TEST_PI / 180.0;
      pairs[i] = make_pair(sin(reference_phase), sin(dds_phase));
    }
    PhaseDetector_Process(&detector, pairs, TEST_PAIR_COUNT,
                          multiplier, &measurement);
  }

  {
    double phase_deg =
        (double)measurement.generalized_phase_rad * 180.0 / TEST_PI;
    double frequency_error =
        fabs((double)measurement.reference_frequency_hz - frequency);
    double phase_error = fabs(phase_deg - expected_generalized_deg);

    if (phase_error > 180.0) {
      phase_error = fabs(phase_error - 360.0);
    }
    printf("%ux: f=%.3f Hz phase=%.3f deg quality=%.3f\n",
           (unsigned int)multiplier,
           (double)measurement.reference_frequency_hz,
           phase_deg, (double)measurement.phase_quality);
    if (!measurement.frequency_valid || !measurement.phase_valid ||
        (frequency_error > 2.0) || (phase_error > 1.0)) {
      return 1;
    }
  }
  return 0;
}

static int run_controller_case(void)
{
  lock_controller_t controller;
  lock_controller_output_t output = {0};
  phase_measurement_t measurement = {
      .signal_valid = true,
      .frequency_valid = true,
      .phase_valid = true,
      .phase_updated = true,
      .reference_frequency_hz = 10000.0f,
      .generalized_phase_rad = 30.0f * (float)TEST_PI / 180.0f,
      .phase_quality = 0.95f
  };
  unsigned int i;

  LockController_Init(&controller);
  if (!LockController_SetMultiplier(&controller, 2U)) {
    return 1;
  }
  LockController_SetTargetPhaseDeg(&controller, 30.0f);
  for (i = 0U; i < 70U; ++i) {
    LockController_Step(&controller, &measurement, 0.001f, &output);
  }

  printf("controller: dds=%.3f Hz fs=%lu locked=%u\n",
         (double)output.dds_frequency_hz,
         (unsigned long)output.requested_sample_rate_hz,
         output.phase_locked ? 1U : 0U);
  return (!output.command_valid || !output.phase_locked ||
          (fabs((double)output.dds_frequency_hz - 20000.0) > 0.5) ||
          (output.requested_sample_rate_hz != 480000U))
             ? 1
             : 0;
}

static int run_100khz_detector_case(void)
{
  phase_detector_t detector;
  phase_measurement_t measurement = {0};
  uint32_t pairs[TEST_HIGH_PAIR_COUNT];
  const double sample_rate = 2400000.0;
  const double frequency = 100000.0;
  uint64_t sample_index = 0U;
  unsigned int block;

  PhaseDetector_Init(&detector, (float)sample_rate);
  for (block = 0U; block < 8U; ++block) {
    uint32_t i;
    for (i = 0U; i < TEST_HIGH_PAIR_COUNT; ++i, ++sample_index) {
      double reference_phase =
          2.0 * TEST_PI * frequency * (double)sample_index /
              sample_rate +
          10.0 * TEST_PI / 180.0;
      double dds_phase =
          2.0 * TEST_PI * frequency * (double)sample_index /
              sample_rate +
          55.0 * TEST_PI / 180.0;
      pairs[i] = make_pair(sin(reference_phase), sin(dds_phase));
    }
    PhaseDetector_Process(&detector, pairs, TEST_HIGH_PAIR_COUNT,
                          1U, &measurement);
  }

  printf("100k detector: f=%.3f Hz phase=%.3f deg quality=%.3f\n",
         (double)measurement.reference_frequency_hz,
         (double)measurement.generalized_phase_rad *
             180.0 / TEST_PI,
         (double)measurement.phase_quality);
  return (!measurement.frequency_valid || !measurement.phase_valid ||
          (fabs((double)measurement.reference_frequency_hz -
                frequency) > 10.0) ||
          (fabs((double)measurement.generalized_phase_rad *
                    180.0 / TEST_PI -
                45.0) > 1.0))
             ? 1
             : 0;
}

static int run_band_detector_case(double frequency)
{
  phase_detector_t detector;
  phase_measurement_t measurement = {0};
  uint32_t pairs[TEST_HIGH_PAIR_COUNT];
  double sample_rate = 24.0 * frequency;
  uint64_t sample_index = 0U;
  unsigned int block;

  if (sample_rate < 10000.0) {
    sample_rate = 10000.0;
  } else if (sample_rate > 2400000.0) {
    sample_rate = 2400000.0;
  }

  PhaseDetector_Init(&detector, (float)sample_rate);
  for (block = 0U; block < 8U; ++block) {
    uint32_t i;
    for (i = 0U; i < TEST_HIGH_PAIR_COUNT; ++i, ++sample_index) {
      double reference_phase =
          2.0 * TEST_PI * frequency * (double)sample_index /
              sample_rate +
          10.0 * TEST_PI / 180.0;
      double dds_phase =
          2.0 * TEST_PI * frequency * (double)sample_index /
              sample_rate +
          55.0 * TEST_PI / 180.0;
      pairs[i] = make_pair(sin(reference_phase), sin(dds_phase));
    }
    PhaseDetector_Process(&detector, pairs, TEST_HIGH_PAIR_COUNT,
                          1U, &measurement);
  }

  printf("band detector: f=%7.0f measured=%10.3f phase=%7.3f "
         "stride=%u win=%u\n",
         frequency, (double)measurement.reference_frequency_hz,
         (double)measurement.generalized_phase_rad *
             180.0 / TEST_PI,
         (unsigned int)measurement.analysis_stride,
         (unsigned int)measurement.phase_pair_count);
  return (!measurement.frequency_valid || !measurement.phase_valid ||
          (fabs((double)measurement.reference_frequency_hz -
                frequency) > 10.0) ||
          (fabs((double)measurement.generalized_phase_rad *
                    180.0 / TEST_PI -
                45.0) > 1.0) ||
          (measurement.analysis_stride < 3U))
             ? 1
             : 0;
}

static double wrap_radians(double radians)
{
  while (radians > TEST_PI) {
    radians -= 2.0 * TEST_PI;
  }
  while (radians <= -TEST_PI) {
    radians += 2.0 * TEST_PI;
  }
  return radians;
}

static int run_100khz_gradient_case(void)
{
  lock_controller_t controller;
  lock_controller_output_t output = {0};
  phase_measurement_t measurement = {
      .signal_valid = true,
      .frequency_valid = true,
      .phase_valid = true,
      .phase_updated = true,
      .reference_frequency_hz = 100000.0f,
      .phase_quality = 0.95f
  };
  const double dt = 0.001;
  double input_frequency = 100000.0;
  double phase = 120.0 * TEST_PI / 180.0;
  double previous_phase_offset = 0.0;
  double largest_fine_step = 0.0;
  bool saw_fine_mode = false;
  unsigned int i;

  LockController_Init(&controller);
  for (i = 0U; i < 2600U; ++i) {
    if (i == 1300U) {
      /* Exercise the clean reacquisition path after an input-frequency hop. */
      input_frequency = 112345.0;
      measurement.reference_frequency_hz = (float)input_frequency;
    }

    measurement.generalized_phase_rad = (float)phase;
    LockController_Step(&controller, &measurement, (float)dt, &output);
    phase = wrap_radians(
        phase +
        2.0 * TEST_PI *
            ((double)output.dds_frequency_hz - input_frequency) * dt +
        wrap_radians(
            (double)output.dds_phase_offset_rad -
            previous_phase_offset));
    previous_phase_offset = (double)output.dds_phase_offset_rad;

    if (output.fine_mode) {
      double step = fabs((double)output.frequency_step_hz);
      saw_fine_mode = true;
      if (step > largest_fine_step) {
        largest_fine_step = step;
      }
    }
  }

  printf("100k/hop: dds=%.4f Hz input=%.4f Hz phase=%.3f deg "
         "fine_step<=%.4f Hz locked=%u\n",
         (double)output.dds_frequency_hz, input_frequency,
         phase * 180.0 / TEST_PI, largest_fine_step,
         output.phase_locked ? 1U : 0U);
  return (!output.command_valid || !output.phase_locked ||
          !saw_fine_mode || (largest_fine_step > 0.0501) ||
          (fabs((double)output.dds_frequency_hz -
                input_frequency) > 0.10) ||
          (fabs(phase * 180.0 / TEST_PI) > 3.0))
             ? 1
             : 0;
}

static int run_band_gradient_case(double initial_frequency,
                                  uint8_t multiplier)
{
  lock_controller_t controller;
  lock_controller_output_t output = {0};
  phase_measurement_t measurement = {
      .signal_valid = true,
      .frequency_valid = true,
      .phase_valid = true,
      .phase_updated = true,
      .phase_quality = 0.95f
  };
  double input_frequency = initial_frequency;
  double sample_rate =
      24.0 * input_frequency * (double)multiplier;
  double dt;
  double phase = 120.0 * TEST_PI / 180.0;
  double previous_phase_offset = 0.0;
  double largest_fine_step = 0.0;
  double elapsed = 0.0;
  bool hopped = false;
  bool saw_fine_mode = false;

  if (sample_rate > 2400000.0) {
    sample_rate = 2400000.0;
  }
  dt = (double)TEST_HIGH_PAIR_COUNT / sample_rate;
  measurement.reference_frequency_hz = (float)input_frequency;

  LockController_Init(&controller);
  if (!LockController_SetMultiplier(&controller, multiplier)) {
    return 1;
  }
  while (elapsed < 3.0) {
    if (!hopped && (elapsed >= 1.4)) {
      input_frequency = initial_frequency * 1.10;
      measurement.reference_frequency_hz = (float)input_frequency;
      hopped = true;
    }

    measurement.generalized_phase_rad = (float)phase;
    LockController_Step(&controller, &measurement, (float)dt, &output);
    phase = wrap_radians(
        phase +
        2.0 * TEST_PI *
            ((double)output.dds_frequency_hz -
             ((double)multiplier * input_frequency)) * dt +
        wrap_radians(
            (double)output.dds_phase_offset_rad -
            previous_phase_offset));
    previous_phase_offset = (double)output.dds_phase_offset_rad;
    elapsed += dt;

    if (output.fine_mode) {
      double step = fabs((double)output.frequency_step_hz);
      saw_fine_mode = true;
      if (step > largest_fine_step) {
        largest_fine_step = step;
      }
    }
  }

  printf("band loop %ux: f=%7.0f->%7.0f dt=%6.2fms phase=%7.3f "
         "step<=%.4f band=%u locked=%u\n",
         (unsigned int)multiplier,
         initial_frequency, input_frequency, dt * 1000.0,
         phase * 180.0 / TEST_PI, largest_fine_step,
         (unsigned int)output.band,
         output.phase_locked ? 1U : 0U);
  return (!output.command_valid || !output.phase_locked ||
          !saw_fine_mode || (largest_fine_step > 0.0501) ||
          (fabs((double)output.dds_frequency_hz -
                ((double)multiplier * input_frequency)) > 0.10) ||
          (fabs(phase * 180.0 / TEST_PI) > 3.0))
             ? 1
             : 0;
}

int main(void)
{
  int failures = 0;

  failures += run_detector_case(1U, 20.0, 65.0, 45.0);
  failures += run_detector_case(2U, 20.0, 80.0, 40.0);
  failures += run_100khz_detector_case();
  failures += run_band_detector_case(10000.0);
  failures += run_band_detector_case(20000.0);
  failures += run_band_detector_case(39000.0);
  failures += run_band_detector_case(40000.0);
  failures += run_band_detector_case(41000.0);
  failures += run_band_detector_case(60000.0);
  failures += run_controller_case();
  failures += run_100khz_gradient_case();
  failures += run_band_gradient_case(10000.0, 1U);
  failures += run_band_gradient_case(10000.0, 2U);
  failures += run_band_gradient_case(20000.0, 1U);
  failures += run_band_gradient_case(39000.0, 1U);
  failures += run_band_gradient_case(41000.0, 1U);
  failures += run_band_gradient_case(60000.0, 1U);
  if (failures != 0) {
    fprintf(stderr, "phase-lock tests failed: %d\n", failures);
    return EXIT_FAILURE;
  }
  puts("phase-lock tests passed");
  return EXIT_SUCCESS;
}
