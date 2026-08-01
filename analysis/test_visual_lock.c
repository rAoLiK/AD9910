#include "visual_lock.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static double fold_phase(double degrees)
{
  while (degrees >= 360.0) {
    degrees -= 360.0;
  }
  while (degrees < 0.0) {
    degrees += 360.0;
  }
  return (degrees <= 180.0) ? degrees : (360.0 - degrees);
}

static void run_lock_case(double offset_hz)
{
  const uint32_t seed_millihz = 10000000UL;
  const double target_hz = ((double)seed_millihz / 1000.0) + offset_hz;
  visual_lock_controller_t controller;
  visual_lock_output_t output = {0};
  visual_lock_sample_t sample = {0};
  double source_phase_deg = 73.0;
  uint32_t now_ms = 0UL;
  unsigned int index;

  VisualLock_Init(&controller, seed_millihz, now_ms);
  for (index = 0U; index < 1600U && !output.locked; ++index) {
    double command_hz =
        (double)controller.command_frequency_millihz / 1000.0;
    double measured_phase;
    double speed_hz = fabs(target_hz - command_hz);

    now_ms += 40UL;
    source_phase_deg += (target_hz - command_hz) * 360.0 * 0.040;
    measured_phase = fold_phase(
        source_phase_deg - ((double)controller.phase_offset_mdeg / 1000.0));
    sample.timestamp_ms = now_ms;
    sample.phase_mdeg = (uint32_t)(measured_phase * 1000.0 + 0.5);
    sample.speed_millihz = (uint16_t)(speed_hz * 1000.0 + 0.5);
    sample.quality = 90U;
    sample.flags = VISUAL_LOCK_SAMPLE_PHASE_VALID |
                   VISUAL_LOCK_SAMPLE_FAMILY_VALID;
    VisualLock_Step(&controller, &sample, &output);
    assert(controller.command_frequency_millihz >= seed_millihz - 5000UL);
    assert(controller.command_frequency_millihz <= seed_millihz + 5000UL);
    assert((controller.command_frequency_millihz % 100UL) == 0UL);
  }

  printf("visual df=%+.1fHz cmd=%.1fHz state=%u reversals=%u boundary=%u\n",
         offset_hz,
         (double)controller.command_frequency_millihz / 1000.0,
         (unsigned int)controller.state,
         (unsigned int)controller.direction_reversal_count,
         (unsigned int)controller.boundary_reversal_count);
  assert(output.locked);
  assert(fabs(target_hz -
              ((double)controller.command_frequency_millihz / 1000.0)) <=
         0.1001);
}

static void run_invalid_sample_case(void)
{
  visual_lock_controller_t controller;
  visual_lock_output_t output;
  visual_lock_sample_t sample = {
      .timestamp_ms = 1000UL,
      .phase_mdeg = 0U,
      .speed_millihz = 100U,
      .quality = 10U,
      .flags = 0U
  };

  VisualLock_Init(&controller, 1000000UL, 0UL);
  VisualLock_Step(&controller, &sample, &output);
  assert(controller.state == VISUAL_LOCK_BASELINE);
  assert(controller.rejected_sample_count == 1U);
  assert(!output.command_changed);
}

static void run_five_hz_boundary_restore_case(void)
{
  const uint32_t seed_millihz = 10000000UL;
  visual_lock_controller_t controller;
  visual_lock_output_t output = {0};
  visual_lock_sample_t sample = {0};
  uint32_t now_ms = 0UL;
  unsigned int index;

  VisualLock_Init(&controller, seed_millihz, now_ms);
  for (index = 0U; index < 400U &&
                   controller.boundary_reversal_count == 0U; ++index) {
    now_ms += 40UL;
    sample.timestamp_ms = now_ms;
    sample.phase_mdeg = 90000U;
    sample.speed_millihz =
        (controller.state == VISUAL_LOCK_BASELINE) ? 5000U : 4000U;
    sample.quality = 90U;
    sample.flags = VISUAL_LOCK_SAMPLE_PHASE_VALID |
                   VISUAL_LOCK_SAMPLE_FAMILY_VALID;
    VisualLock_Step(&controller, &sample, &output);
    assert(controller.command_frequency_millihz >= seed_millihz - 5000UL);
    assert(controller.command_frequency_millihz <= seed_millihz + 5000UL);
  }

  assert(controller.boundary_reversal_count == 1U);
  assert(controller.command_frequency_millihz == seed_millihz);
  assert(controller.frequency_direction == -1L);
  assert(output.command_changed);
}

int main(void)
{
  run_lock_case(2.3);
  run_lock_case(-3.7);
  run_lock_case(0.0);
  run_five_hz_boundary_restore_case();
  run_invalid_sample_case();
  puts("visual-lock tests passed");
  return 0;
}
