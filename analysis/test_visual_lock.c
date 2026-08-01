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

static void feed_sample(visual_lock_controller_t *controller,
                        visual_lock_output_t *output,
                        double target_hz,
                        double *source_phase_deg,
                        uint32_t *now_ms)
{
  visual_lock_sample_t sample = {0};
  double command_hz =
      (double)controller->command_frequency_millihz / 1000.0;
  double measured_phase;
  double speed_hz = fabs(target_hz - command_hz);

  *now_ms += 40UL;
  *source_phase_deg +=
      (target_hz - command_hz) * 360.0 * 0.040;
  measured_phase = fold_phase(
      *source_phase_deg -
      ((double)controller->phase_offset_mdeg / 1000.0));
  sample.timestamp_ms = *now_ms;
  sample.phase_mdeg =
      (uint32_t)(measured_phase * 1000.0 + 0.5);
  sample.speed_millihz =
      (uint16_t)(speed_hz * 1000.0 + 0.5);
  sample.quality = 90U;
  sample.flags = VISUAL_LOCK_SAMPLE_PHASE_VALID |
                 VISUAL_LOCK_SAMPLE_FAMILY_VALID;
  VisualLock_Step(controller, &sample, output);
}

static void run_lock_case(double offset_hz,
                          int32_t target_phase_mdeg)
{
  const uint32_t seed_millihz = 10000000UL;
  const double target_hz =
      ((double)seed_millihz / 1000.0) + offset_hz;
  visual_lock_controller_t controller;
  visual_lock_output_t output = {0};
  double source_phase_deg = 73.0;
  uint32_t now_ms = 0UL;
  unsigned int index;

  VisualLock_InitTarget(
      &controller, seed_millihz, target_phase_mdeg, now_ms);
  for (index = 0U; index < 2400U && !output.locked; ++index) {
    feed_sample(&controller, &output, target_hz,
                &source_phase_deg, &now_ms);
    assert(controller.command_frequency_millihz >=
           seed_millihz - 5000UL);
    assert(controller.command_frequency_millihz <=
           seed_millihz + 5000UL);
    assert((controller.command_frequency_millihz % 10UL) == 0UL);
    assert(controller.phase_offset_mdeg == target_phase_mdeg);
  }

  printf("visual df=%+.1fHz cmd=%.2fHz state=%u reversals=%u boundary=%u\n",
         offset_hz,
         (double)controller.command_frequency_millihz / 1000.0,
         (unsigned int)controller.state,
         (unsigned int)controller.direction_reversal_count,
         (unsigned int)controller.boundary_reversal_count);
  assert(output.locked);
  assert(fabs(target_hz -
              ((double)controller.command_frequency_millihz / 1000.0)) <=
         0.0601);

  /* LOCKED is not terminal: keep sampling and hold the same command while
   * the image remains stable. */
  for (index = 0U; index < 40U; ++index) {
    uint32_t held_frequency = controller.command_frequency_millihz;
    feed_sample(&controller, &output, target_hz,
                &source_phase_deg, &now_ms);
    assert(output.locked);
    assert(controller.command_frequency_millihz == held_frequency);
    assert(controller.phase_offset_mdeg == target_phase_mdeg);
  }
}

static void run_permanent_reacquire_case(void)
{
  const uint32_t seed_millihz = 10000000UL;
  visual_lock_controller_t controller;
  visual_lock_output_t output = {0};
  double source_phase_deg = 20.0;
  double target_hz = 10002.3;
  uint32_t now_ms = 0UL;
  unsigned int index;

  VisualLock_InitTarget(&controller, seed_millihz, 270000L, 0UL);
  for (index = 0U; index < 2400U && !output.locked; ++index) {
    feed_sample(&controller, &output, target_hz,
                &source_phase_deg, &now_ms);
  }
  assert(output.locked);

  /* Simulate source drift after lock. The visual loop must reacquire without
   * changing the requested circle phase or exceeding the +/-5 Hz window. */
  target_hz = 10003.1;
  for (index = 0U; index < 2400U; ++index) {
    feed_sample(&controller, &output, target_hz,
                &source_phase_deg, &now_ms);
    assert(controller.command_frequency_millihz >=
           seed_millihz - 5000UL);
    assert(controller.command_frequency_millihz <=
           seed_millihz + 5000UL);
    assert(controller.phase_offset_mdeg == 270000L);
    if (output.locked &&
        fabs(target_hz -
             ((double)controller.command_frequency_millihz / 1000.0)) <=
            0.0601) {
      break;
    }
  }
  assert(index < 2400U);
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

static void run_five_hz_hard_limit_case(void)
{
  const uint32_t seed_millihz = 10000000UL;
  visual_lock_controller_t controller;
  visual_lock_output_t output = {0};
  visual_lock_sample_t sample = {0};
  uint32_t now_ms = 0UL;
  unsigned int index;

  VisualLock_Init(&controller, seed_millihz, now_ms);
  for (index = 0U; index < 1200U; ++index) {
    now_ms += 40UL;
    sample.timestamp_ms = now_ms;
    sample.phase_mdeg = 90000U;
    sample.speed_millihz = 5000U;
    sample.quality = 90U;
    sample.flags = VISUAL_LOCK_SAMPLE_PHASE_VALID |
                   VISUAL_LOCK_SAMPLE_FAMILY_VALID;
    VisualLock_Step(&controller, &sample, &output);
    assert(controller.command_frequency_millihz >=
           seed_millihz - 5000UL);
    assert(controller.command_frequency_millihz <=
           seed_millihz + 5000UL);
  }
  assert(controller.boundary_reversal_count > 0U);
}

int main(void)
{
  run_lock_case(2.3, 0L);
  run_lock_case(-3.7, 270000L);
  run_lock_case(0.0, 0L);
  run_lock_case(5.0, 0L);
  run_lock_case(-5.0, 0L);
  run_permanent_reacquire_case();
  run_five_hz_hard_limit_case();
  run_invalid_sample_case();
  puts("permanent visual-lock tests passed");
  return 0;
}
