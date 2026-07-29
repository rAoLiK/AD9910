#include "lock_controller.h"
#include "phase_detector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_PI             (3.14159265358979323846)
#define TEST_PAIR_COUNT     (256U)

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

int main(void)
{
  int failures = 0;

  failures += run_detector_case(1U, 20.0, 65.0, 45.0);
  failures += run_detector_case(2U, 20.0, 80.0, 40.0);
  failures += run_controller_case();
  if (failures != 0) {
    fprintf(stderr, "phase-lock tests failed: %d\n", failures);
    return EXIT_FAILURE;
  }
  puts("phase-lock tests passed");
  return EXIT_SUCCESS;
}
