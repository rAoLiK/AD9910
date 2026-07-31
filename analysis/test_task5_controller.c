#include "task5_controller.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_DDS_HISTORY_CAPACITY (64U)

enum {
  TEST_DDS_TARGET_REACHED = 0x00U,
  TEST_DDS_TOO_LOW = 0x01U,
  TEST_DDS_TOO_HIGH = 0x02U,
  TEST_DDS_NOT_MATCHED = 0x03U,
  TEST_DDS_IMAGE_ERROR = 0x06U
};

typedef struct {
  uint8_t type;
  uint8_t seq;
  uint16_t length;
  uint8_t payload[OPENMV_PROTOCOL_MAX_PAYLOAD];
  uint32_t send_count;
  uint8_t sent_types[16];
  uint8_t last_stop_reason;
  uint16_t last_stop_session;
  uint32_t saw_frequency_hz;
  uint32_t dds_frequency_hz;
  uint32_t dds_frequency_history[TEST_DDS_HISTORY_CAPACITY];
  uint32_t dds_set_count;
  uint32_t seed_frequency_hz;
  uint32_t saw_start_count;
  task5_lock_mode_t lock_mode;
  bool saw_running;
  bool safe;
  bool phase_started;
  bool send_fails;
} fake_port_t;

static bool fake_start_saw(void *context, uint32_t frequency_hz)
{
  fake_port_t *fake = context;
  fake->saw_frequency_hz = frequency_hz;
  fake->saw_running = true;
  fake->saw_start_count++;
  return true;
}

static void fake_stop_saw(void *context)
{
  ((fake_port_t *)context)->saw_running = false;
}

static bool fake_set_dds(void *context, uint32_t frequency_hz)
{
  fake_port_t *fake = context;

  fake->dds_frequency_hz = frequency_hz;
  if (fake->dds_set_count < TEST_DDS_HISTORY_CAPACITY) {
    fake->dds_frequency_history[fake->dds_set_count] = frequency_hz;
  }
  fake->dds_set_count++;
  return true;
}

static bool fake_start_lock(void *context,
                            task5_lock_mode_t mode,
                            uint32_t seed_frequency_hz)
{
  fake_port_t *fake = context;
  fake->lock_mode = mode;
  fake->seed_frequency_hz = seed_frequency_hz;
  fake->phase_started = true;
  return true;
}

static void fake_safe(void *context)
{
  ((fake_port_t *)context)->safe = true;
}

static bool fake_send(void *context,
                      uint8_t type,
                      uint8_t seq,
                      const uint8_t *payload,
                      uint16_t payload_length)
{
  fake_port_t *fake = context;
  fake->type = type;
  fake->seq = seq;
  fake->length = payload_length;
  if (payload_length != 0U) {
    memcpy(fake->payload, payload, payload_length);
  }
  fake->send_count++;
  if (fake->send_count <=
      (sizeof(fake->sent_types) / sizeof(fake->sent_types[0]))) {
    fake->sent_types[fake->send_count - 1U] = type;
  }
  if ((type == OPENMV_MSG_STOP_TASK) &&
      (payload_length == 3U)) {
    fake->last_stop_session = OpenMV_ReadU16LE(payload);
    fake->last_stop_reason = payload[2];
  }
  return !fake->send_fails;
}

static openmv_frame_t ack_for(const fake_port_t *fake)
{
  openmv_frame_t frame = {0};
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_ACK;
  frame.seq = 0x91U;
  frame.length = 3U;
  frame.payload[0] = fake->type;
  frame.payload[1] = fake->seq;
  frame.payload[2] = 0U;
  return frame;
}

static task5_port_t make_port(fake_port_t *fake)
{
  task5_port_t port = {
      .start_saw = fake_start_saw,
      .stop_saw = fake_stop_saw,
      .set_dds_frequency = fake_set_dds,
      .start_phase_lock = fake_start_lock,
      .safe_outputs = fake_safe,
      .send_frame = fake_send,
      .context = fake,
  };

  return port;
}

static uint32_t begin_frequency_search(
    task5_controller_t *controller,
    fake_port_t *fake,
    task5_lock_mode_t mode,
    uint32_t saw_frequency_hz,
    uint32_t ratio_x1000)
{
  task5_port_t port = make_port(fake);
  task5_status_t status;
  openmv_frame_t frame = {0};

  assert(Task5_Init(controller, &port));
  Task5_Enter(controller);
  assert(Task5_Start(
      controller, mode, saw_frequency_hz, 10U));

  frame = ack_for(fake);
  Task5_OnFrame(controller, &frame, 20U);
  Task5_GetStatus(controller, &status);
  assert(status.state == TASK5_STATE_WAIT_COARSE_RESULT);

  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_COARSE_RESULT;
  frame.seq = 0x22U;
  frame.length = 10U;
  OpenMV_WriteU16LE(&frame.payload[0], status.session_id);
  frame.payload[2] = 0U;
  OpenMV_WriteU32LE(&frame.payload[3], ratio_x1000);
  frame.payload[7] = 95U;
  frame.payload[8] = 5U;
  frame.payload[9] = 5U;
  Task5_OnFrame(controller, &frame, 30U);

  Task5_GetStatus(controller, &status);
  assert(status.state == TASK5_STATE_DDS_SETTLING);
  assert(!fake->saw_running);
  assert(fake->dds_set_count == 1U);
  assert(status.dds_frequency_hz == fake->dds_frequency_hz);
  return 30U;
}

static void finish_dds_test_with_quality(
    task5_controller_t *controller,
    fake_port_t *fake,
    uint8_t result,
    uint16_t score,
    uint8_t confidence,
    uint8_t result_seq,
    uint32_t *now_ms)
{
  task5_status_t status;
  openmv_frame_t frame = {0};

  *now_ms += 200U;
  Task5_Process(controller, *now_ms);
  assert(fake->type == OPENMV_MSG_DDS_TEST);
  assert(fake->length == 11U);
  assert(OpenMV_ReadU32LE(&fake->payload[4]) ==
         fake->dds_frequency_hz);

  frame = ack_for(fake);
  *now_ms += 1U;
  Task5_OnFrame(controller, &frame, *now_ms);
  Task5_GetStatus(controller, &status);
  assert(status.state == TASK5_STATE_WAIT_DDS_RESULT);

  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_DDS_TEST_RESULT;
  frame.seq = result_seq;
  frame.length = 8U;
  OpenMV_WriteU16LE(&frame.payload[0], status.session_id);
  OpenMV_WriteU16LE(&frame.payload[2], status.test_id);
  frame.payload[4] = result;
  OpenMV_WriteU16LE(&frame.payload[5], score);
  frame.payload[7] = confidence;
  *now_ms += 1U;
  Task5_OnFrame(controller, &frame, *now_ms);
}

static void finish_dds_test(
    task5_controller_t *controller,
    fake_port_t *fake,
    uint8_t result,
    uint8_t result_seq,
    uint32_t *now_ms)
{
  finish_dds_test_with_quality(
      controller, fake, result, 900U, 95U,
      result_seq, now_ms);
}

static void assert_frequency_history(
    const fake_port_t *fake,
    const uint32_t *expected,
    uint32_t expected_count)
{
  uint32_t index;

  assert(fake->dds_set_count == expected_count);
  assert(expected_count <= TEST_DDS_HISTORY_CAPACITY);
  for (index = 0U; index < expected_count; index++) {
    assert(fake->dds_frequency_history[index] == expected[index]);
  }
}

static void test_coarse_origin_rounds_to_100_hz(void)
{
  static const struct {
    uint32_t estimated_hz;
    uint32_t expected_origin_hz;
  } cases[] = {
      {5249U, 5200U},
      {5250U, 5300U},
  };
  uint32_t index;

  for (index = 0U;
       index < (sizeof(cases) / sizeof(cases[0]));
       index++) {
    fake_port_t fake = {0};
    task5_controller_t controller;
    task5_status_t status;

    (void)begin_frequency_search(
        &controller, &fake, TASK5_MODE_LINE_0_DEG,
        1000U, cases[index].estimated_hz);
    Task5_GetStatus(&controller, &status);

    assert(status.estimated_input_frequency_hz ==
           cases[index].estimated_hz);
    assert(status.dds_frequency_hz ==
           cases[index].expected_origin_hz);
    assert(fake.dds_frequency_hz ==
           cases[index].expected_origin_hz);
    assert(fake.dds_frequency_history[0] ==
           cases[index].expected_origin_hz);
  }
}

static void assert_search_sequence_for_result(uint8_t result)
{
  static const uint32_t expected[] = {
      5200U, 5300U, 5100U, 5400U, 5000U};
  fake_port_t fake = {0};
  task5_controller_t controller;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 5249U);
  for (index = 1U;
       index < (sizeof(expected) / sizeof(expected[0]));
       index++) {
    assert(fake.dds_frequency_hz == expected[index - 1U]);
    finish_dds_test(
        &controller, &fake, result,
        (uint8_t)(0x30U + index), &now_ms);
    assert(fake.dds_frequency_hz == expected[index]);
  }

  assert_frequency_history(
      &fake, expected,
      (uint32_t)(sizeof(expected) / sizeof(expected[0])));
}

static void test_search_order_ignores_openmv_direction_hint(void)
{
  assert_search_sequence_for_result(TEST_DDS_NOT_MATCHED);
  assert_search_sequence_for_result(TEST_DDS_TOO_LOW);
  assert_search_sequence_for_result(TEST_DDS_TOO_HIGH);
}

static void assert_frequency_history_is_unique_grid(
    const fake_port_t *fake)
{
  uint32_t index;
  uint32_t previous;

  assert(fake->dds_set_count <= TEST_DDS_HISTORY_CAPACITY);
  for (index = 0U; index < fake->dds_set_count; index++) {
    assert(fake->dds_frequency_history[index] >= 100U);
    assert(fake->dds_frequency_history[index] <= 200000U);
    assert((fake->dds_frequency_history[index] % 100U) == 0U);
    for (previous = 0U; previous < index; previous++) {
      assert(fake->dds_frequency_history[index] !=
             fake->dds_frequency_history[previous]);
    }
  }
}

static void exercise_boundary_search(
    task5_lock_mode_t mode,
    uint32_t ratio_x1000,
    uint32_t expected_origin_hz)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, mode, 1000U, ratio_x1000);
  assert(fake.dds_frequency_hz == expected_origin_hz);
  for (index = 0U; index < 8U; index++) {
    finish_dds_test(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        (uint8_t)(0x50U + index), &now_ms);
  }

  assert(fake.dds_set_count == 9U);
  assert_frequency_history_is_unique_grid(&fake);
}

static void test_boundary_candidates_are_unique_100_hz_grid(void)
{
  exercise_boundary_search(
      TASK5_MODE_LINE_0_DEG, 149U, 100U);
  exercise_boundary_search(
      TASK5_MODE_INFINITY_2X_0_DEG, 100000U, 200000U);
}

static void test_high_search_restarts_flat_quality_curve(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 100000U);
  for (index = 0U; index < 29U; index++) {
    finish_dds_test_with_quality(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        600U, 100U, (uint8_t)(0x80U + index), &now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_DDS_SETTLING);
  assert(status.last_error == TASK5_ERROR_NONE);
  assert(status.search_stage == 0x01U);
  assert(status.search_restart_count == 1U);
  assert(status.search_count == 30U);
  assert(fake.dds_set_count == 30U);
  assert(!fake.saw_running);
}

static void test_search_count_saturates_without_limit_error(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 100000U);
  for (index = 0U; index < 320U; index++) {
    finish_dds_test_with_quality(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        600U, 100U, (uint8_t)index, &now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_DDS_SETTLING);
  assert(status.last_error == TASK5_ERROR_NONE);
  assert(status.search_count == UINT8_MAX);
  assert(status.search_restart_count >= 10U);
  assert(fake.dds_set_count > 255U);
  assert(!fake.saw_running);
}

static void test_low_search_holds_local_quality_peak(void)
{
  static const uint32_t frequencies[] = {
      5200U, 5300U, 5100U, 5400U, 5000U, 5500U};
  static const uint16_t qualities[] = {
      500U, 850U, 300U, 600U, 100U, 300U};
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 5249U);
  for (index = 0U;
       index < (sizeof(frequencies) / sizeof(frequencies[0]));
       index++) {
    assert(fake.dds_frequency_hz == frequencies[index]);
    finish_dds_test_with_quality(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        qualities[index], 100U,
        (uint8_t)(0x60U + index), &now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.search_stage == 0x03U);
  assert(fake.dds_frequency_hz == 5300U);
  assert(controller.confirmation_peak_valid);
  assert(controller.fine_best_frequency_hz == 5300U);

  finish_dds_test_with_quality(
      &controller, &fake, TEST_DDS_NOT_MATCHED,
      700U, 100U, 0x70U, &now_ms);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_WAIT_STOP_ACK);
  assert(fake.type == OPENMV_MSG_STOP_TASK);
  assert(fake.dds_frequency_hz == 5300U);
  assert(!fake.saw_running);
}

static void test_fast_settle_and_capture_delay(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;

  (void)begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 5249U);
  assert(fake.type != OPENMV_MSG_DDS_TEST);
  Task5_Process(&controller, 49U);
  assert(fake.type != OPENMV_MSG_DDS_TEST);
  Task5_Process(&controller, 50U);
  assert(fake.type == OPENMV_MSG_DDS_TEST);
  assert(OpenMV_ReadU16LE(&fake.payload[9]) == 80U);
}

static void test_high_search_threshold_is_inclusive(void)
{
  fake_port_t below_fake = {0};
  fake_port_t high_fake = {0};
  task5_controller_t below_controller;
  task5_controller_t high_controller;
  task5_status_t status;

  (void)begin_frequency_search(
      &below_controller, &below_fake,
      TASK5_MODE_LINE_0_DEG, 1000U, 9999U);
  Task5_GetStatus(&below_controller, &status);
  assert(status.search_stage == 0x02U);

  (void)begin_frequency_search(
      &high_controller, &high_fake,
      TASK5_MODE_LINE_0_DEG, 1000U, 10000U);
  Task5_GetStatus(&high_controller, &status);
  assert(status.search_stage == 0x01U);
  assert(high_fake.dds_frequency_hz == 10000U);
}

static void test_high_search_uses_trend_pair_midpoint(void)
{
  static const uint32_t coarse_frequencies[] = {
      50000U, 51000U, 49000U, 52000U,
      48000U, 53000U, 54000U, 55000U};
  static const uint16_t qualities[] = {
      300U, 400U, 200U, 700U,
      100U, 850U, 650U, 400U};
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 50000U);
  for (index = 0U;
       index <
       (sizeof(coarse_frequencies) /
        sizeof(coarse_frequencies[0]));
       index++) {
    assert(fake.dds_frequency_hz == coarse_frequencies[index]);
    finish_dds_test_with_quality(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        qualities[index], 100U,
        (uint8_t)(0xA0U + index), &now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_DDS_SETTLING);
  assert(status.search_stage == 0x02U);
  assert(controller.search_lower_hz == 52000U);
  assert(controller.search_upper_hz == 53000U);
  assert(fake.dds_frequency_hz == 52500U);
  assert(status.best_match_quality == 850U);
  assert(status.best_match_frequency_hz == 53000U);
}

static void test_missed_initial_target_returns_to_valley_minimum(void)
{
  static const uint32_t coarse_frequencies[] = {
      50000U, 51000U, 49000U, 52000U, 48000U};
  static const uint16_t coarse_qualities[] = {
      800U, 500U, 500U, 200U, 200U};
  static const uint32_t fine_frequencies[] = {
      49500U, 49600U, 49400U, 49700U, 49300U, 49800U,
      49200U, 49900U, 49100U, 50000U, 49000U};
  static const uint16_t fine_qualities[] = {
      300U, 350U, 250U, 400U, 200U, 500U,
      150U, 600U, 100U, 700U, 80U};
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  openmv_frame_t frame;
  uint32_t now_ms;
  uint32_t index;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 50000U);
  for (index = 0U;
       index <
       (sizeof(coarse_frequencies) /
        sizeof(coarse_frequencies[0]));
       index++) {
    assert(fake.dds_frequency_hz == coarse_frequencies[index]);
    finish_dds_test_with_quality(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        coarse_qualities[index], 100U,
        (uint8_t)(0xB0U + index), &now_ms);
  }

  assert(controller.search_lower_hz == 49000U);
  assert(controller.search_upper_hz == 50000U);
  for (index = 0U;
       index <
       (sizeof(fine_frequencies) /
        sizeof(fine_frequencies[0]));
       index++) {
    assert(fake.dds_frequency_hz == fine_frequencies[index]);
    finish_dds_test_with_quality(
        &controller, &fake, TEST_DDS_NOT_MATCHED,
        fine_qualities[index], 100U,
        (uint8_t)(0xC0U + index), &now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.search_stage == 0x03U);
  assert(fake.dds_frequency_hz == 50000U);
  assert(controller.fine_best_frequency_hz == 50000U);

  finish_dds_test_with_quality(
      &controller, &fake, TEST_DDS_NOT_MATCHED,
      650U, 100U, 0xD0U, &now_ms);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_WAIT_STOP_ACK);
  assert(fake.type == OPENMV_MSG_STOP_TASK);
  assert(fake.dds_frequency_hz == 50000U);

  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, now_ms + 1U);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_FREQUENCY_HOLD);
  assert(status.dds_frequency_hz == 50000U);
}

static void test_image_error_retries_forever_without_output_change(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  uint32_t now_ms;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 5249U);
  finish_dds_test(
      &controller, &fake, TEST_DDS_IMAGE_ERROR,
      0x71U, &now_ms);
  finish_dds_test(
      &controller, &fake, TEST_DDS_IMAGE_ERROR,
      0x72U, &now_ms);

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_DDS_SETTLING);
  assert(status.search_count == 1U);
  assert(fake.dds_frequency_hz == 5200U);
  assert(fake.dds_set_count == 1U);

  finish_dds_test(
      &controller, &fake, TEST_DDS_IMAGE_ERROR,
      0x73U, &now_ms);

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_DDS_SETTLING);
  assert(status.last_error == TASK5_ERROR_NONE);
  assert(status.last_openmv_result == TEST_DDS_IMAGE_ERROR);
  assert(status.image_recovery_count == 3U);
  assert(status.search_count == 1U);
  assert(fake.dds_frequency_hz == 5200U);
  assert(fake.dds_set_count == 1U);
  assert(!fake.saw_running);
}

static void test_complete_success_flow(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  task5_port_t port = {
      .start_saw = fake_start_saw,
      .stop_saw = fake_stop_saw,
      .set_dds_frequency = fake_set_dds,
      .start_phase_lock = fake_start_lock,
      .safe_outputs = fake_safe,
      .send_frame = fake_send,
      .context = &fake,
  };
  openmv_frame_t frame;
  uint8_t result_seq;

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_LINE_0_DEG, 1000U, 10U));
  assert(fake.saw_running);
  assert(fake.type == OPENMV_MSG_START_TASK);
  assert(fake.length == 7U);
  assert(OpenMV_ReadU32LE(&fake.payload[3]) == 1000U);

  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, 20U);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_WAIT_COARSE_RESULT);

  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_COARSE_RESULT;
  frame.seq = 0x22U;
  frame.length = 10U;
  OpenMV_WriteU16LE(&frame.payload[0], status.session_id);
  frame.payload[2] = 0U;
  OpenMV_WriteU32LE(&frame.payload[3], 5200U);
  frame.payload[7] = 95U;
  frame.payload[8] = 5U;
  frame.payload[9] = 5U;
  Task5_OnFrame(&controller, &frame, 30U);
  assert(!fake.saw_running);
  assert(fake.dds_frequency_hz == 5200U);

  Task5_Process(&controller, 230U);
  assert(fake.type == OPENMV_MSG_DDS_TEST);
  assert(OpenMV_ReadU32LE(&fake.payload[4]) == 5200U);
  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, 240U);

  Task5_GetStatus(&controller, &status);
  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_DDS_TEST_RESULT;
  frame.seq = 0x23U;
  frame.length = 8U;
  OpenMV_WriteU16LE(&frame.payload[0], status.session_id);
  OpenMV_WriteU16LE(&frame.payload[2], status.test_id);
  frame.payload[4] = TEST_DDS_TARGET_REACHED;
  OpenMV_WriteU16LE(&frame.payload[5], 900U);
  frame.payload[7] = 95U;
  result_seq = frame.seq;
  Task5_OnFrame(&controller, &frame, 250U);
  assert(fake.type == OPENMV_MSG_STOP_TASK);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_WAIT_STOP_ACK);

  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, 260U);
  assert(!fake.phase_started);
  assert(fake.dds_frequency_hz == 5200U);
  assert(fake.dds_set_count == 1U);
  Task5_NotifyPhaseLock(&controller, true, false);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_FREQUENCY_HOLD);
  assert(status.dds_frequency_hz == 5200U);
  Task5_Process(&controller, 1260U);
  assert(fake.dds_frequency_hz == 5200U);
  assert(fake.dds_set_count == 1U);

  /* A duplicated result is ACKed but must not leave frequency hold. */
  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_DDS_TEST_RESULT;
  frame.seq = result_seq;
  frame.length = 8U;
  OpenMV_WriteU16LE(&frame.payload[0], status.session_id);
  OpenMV_WriteU16LE(&frame.payload[2], status.test_id);
  frame.payload[4] = TEST_DDS_TARGET_REACHED;
  Task5_OnFrame(&controller, &frame, 270U);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_FREQUENCY_HOLD);
  assert(status.duplicate_result_count == 1U);
  assert(!fake.phase_started);
  assert(fake.dds_frequency_hz == 5200U);
  assert(fake.dds_set_count == 1U);
}

static void test_lost_stop_ack_still_holds_frequency(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  uint32_t now_ms;
  uint32_t retry;

  now_ms = begin_frequency_search(
      &controller, &fake, TASK5_MODE_LINE_0_DEG,
      1000U, 5249U);
  finish_dds_test(
      &controller, &fake, TEST_DDS_TARGET_REACHED,
      0x47U, &now_ms);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_WAIT_STOP_ACK);
  assert(fake.dds_frequency_hz == 5200U);

  /* Drop every STOP ACK. Reliable cleanup exhausts its retries, but the
   * already established TARGET result must remain a successful hold. */
  for (retry = 0U; retry < 4U; retry++) {
    now_ms += 100U;
    Task5_Process(&controller, now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_FREQUENCY_HOLD);
  assert(status.last_error == TASK5_ERROR_NONE);
  assert(!fake.phase_started);
  assert(!fake.saw_running);
  assert(fake.dds_frequency_hz == 5200U);
  assert(fake.dds_set_count == 1U);
}

static void test_ack_retry_keeps_sequence(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};
  uint8_t first_seq;

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_CIRCLE_NEG_90_DEG,
      10000U, 0U));
  first_seq = fake.seq;
  Task5_Process(&controller, 100U);
  assert(fake.send_count == 2U);
  assert(fake.seq == first_seq);
}

static void test_coarse_wait_has_no_hard_timeout(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};
  openmv_frame_t frame;
  uint32_t now_ms;

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_LINE_0_DEG, 10000U, 0U));
  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, 20U);

  for (now_ms = 5020U; now_ms <= 30020U; now_ms += 5000U) {
    Task5_Process(&controller, now_ms);
  }

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_WAIT_COARSE_RESULT);
  assert(status.last_error == TASK5_ERROR_NONE);
  assert(status.result_retry_count == 6U);
  assert(fake.send_count == 7U);
  assert(fake.type == OPENMV_MSG_START_TASK);
  assert(fake.saw_running);
  assert(fake.saw_frequency_hz == 10000U);
}

static void test_bad_coarse_result_restarts_instead_of_error(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t before;
  task5_status_t after;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};
  openmv_frame_t frame;

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_CIRCLE_NEG_90_DEG,
      10000U, 0U));
  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, 20U);
  Task5_GetStatus(&controller, &before);

  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_COARSE_RESULT;
  frame.seq = 0x31U;
  frame.length = 10U;
  OpenMV_WriteU16LE(&frame.payload[0], before.session_id);
  frame.payload[2] = 2U;
  OpenMV_WriteU32LE(&frame.payload[3], 0U);
  Task5_OnFrame(&controller, &frame, 30U);

  Task5_GetStatus(&controller, &after);
  assert(after.state == TASK5_STATE_WAIT_START_ACK);
  assert(after.last_error == TASK5_ERROR_NONE);
  assert(after.session_id != before.session_id);
  assert(after.mode == TASK5_MODE_CIRCLE_NEG_90_DEG);
  assert(after.saw_frequency_hz == 10000U);
  assert(fake.send_count == 4U);
  assert(fake.sent_types[1] == OPENMV_MSG_ACK);
  assert(fake.sent_types[2] == OPENMV_MSG_STOP_TASK);
  assert(fake.sent_types[3] == OPENMV_MSG_START_TASK);
  assert(fake.last_stop_session == before.session_id);
  assert(fake.last_stop_reason == 0x04U);
  assert(fake.saw_running);
  assert(fake.saw_start_count == 2U);
}

static void test_ack_timeout_keeps_dac_running(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_LINE_0_DEG, 1000U, 0U));
  Task5_Process(&controller, 100U);
  Task5_Process(&controller, 200U);
  Task5_Process(&controller, 300U);
  Task5_Process(&controller, 400U);

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_ERROR);
  assert(status.error_origin_state ==
         TASK5_STATE_WAIT_START_ACK);
  assert(status.last_error == TASK5_ERROR_ACK_TIMEOUT);
  assert(fake.saw_running);
  assert(fake.saw_frequency_hz == 1000U);
  assert(fake.saw_start_count == 2U);
  assert(!fake.safe);
}

static void test_active_session_can_be_reselected(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t before;
  task5_status_t after;
  openmv_frame_t frame = {0};
  uint8_t old_start_seq;
  uint16_t old_session_id;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_LINE_0_DEG, 1000U, 10U));
  old_start_seq = fake.seq;
  Task5_GetStatus(&controller, &before);
  old_session_id = before.session_id;

  assert(Task5_Start(
      &controller, TASK5_MODE_CIRCLE_NEG_90_DEG,
      10000U, 20U));
  Task5_GetStatus(&controller, &after);

  assert(fake.send_count == 3U);
  assert(fake.sent_types[0] == OPENMV_MSG_START_TASK);
  assert(fake.sent_types[1] == OPENMV_MSG_STOP_TASK);
  assert(fake.sent_types[2] == OPENMV_MSG_START_TASK);
  assert(fake.last_stop_session == old_session_id);
  assert(fake.last_stop_reason == 0x04U);
  assert(after.session_id != before.session_id);
  assert(after.mode == TASK5_MODE_CIRCLE_NEG_90_DEG);
  assert(after.saw_frequency_hz == 10000U);
  assert(after.state == TASK5_STATE_WAIT_START_ACK);
  assert(fake.safe);
  assert(fake.saw_running);
  assert(fake.saw_frequency_hz == 10000U);
  assert(fake.saw_start_count == 2U);

  /* Invalid re-selection must leave the active session untouched. */
  before = after;
  assert(!Task5_Start(
      &controller, TASK5_MODE_LINE_0_DEG, 500U, 30U));
  Task5_GetStatus(&controller, &after);
  assert(after.session_id == before.session_id);
  assert(after.state == before.state);
  assert(fake.send_count == 3U);

  /* Late traffic from the cancelled session cannot advance the new one. */
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_ACK;
  frame.seq = 0x90U;
  frame.length = 3U;
  frame.payload[0] = OPENMV_MSG_START_TASK;
  frame.payload[1] = old_start_seq;
  frame.payload[2] = 0U;
  Task5_OnFrame(&controller, &frame, 40U);
  Task5_GetStatus(&controller, &after);
  assert(after.state == TASK5_STATE_WAIT_START_ACK);
  assert(fake.send_count == 3U);

  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_COARSE_RESULT;
  frame.seq = 0x91U;
  frame.length = 10U;
  OpenMV_WriteU16LE(&frame.payload[0], old_session_id);
  frame.payload[2] = 0U;
  OpenMV_WriteU32LE(&frame.payload[3], 5000U);
  frame.payload[7] = 90U;
  Task5_OnFrame(&controller, &frame, 50U);
  Task5_GetStatus(&controller, &after);
  assert(after.state == TASK5_STATE_WAIT_START_ACK);
  assert(after.stale_result_count == 1U);
  assert(fake.type == OPENMV_MSG_NACK);
}

static void test_uart_send_failure_becomes_visible_error(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};

  fake.send_fails = true;
  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_CIRCLE_NEG_90_DEG,
      10000U, 0U));
  Task5_Process(&controller, 100U);
  Task5_Process(&controller, 200U);
  Task5_Process(&controller, 300U);

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_ERROR);
  assert(status.error_origin_state ==
         TASK5_STATE_WAIT_START_ACK);
  assert(status.last_error == TASK5_ERROR_UART_SEND);
  assert(fake.send_count == 4U);
  assert(fake.saw_running);
  assert(fake.saw_frequency_hz == 10000U);
  assert(!fake.safe);
}

static void test_uart_retry_failure_becomes_visible_error(void)
{
  fake_port_t fake = {0};
  task5_controller_t controller;
  task5_status_t status;
  task5_port_t port = {
      fake_start_saw, fake_stop_saw, fake_set_dds,
      fake_start_lock, fake_safe, fake_send, &fake};

  assert(Task5_Init(&controller, &port));
  Task5_Enter(&controller);
  assert(Task5_Start(
      &controller, TASK5_MODE_INFINITY_2X_0_DEG,
      1000U, 0U));
  fake.send_fails = true;
  Task5_Process(&controller, 100U);
  Task5_Process(&controller, 200U);
  Task5_Process(&controller, 300U);

  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_ERROR);
  assert(status.error_origin_state ==
         TASK5_STATE_WAIT_START_ACK);
  assert(status.last_error == TASK5_ERROR_UART_SEND);
  assert(fake.send_count == 4U);
  assert(fake.saw_running);
  assert(fake.saw_frequency_hz == 1000U);
  assert(!fake.safe);
}

int main(void)
{
  test_coarse_origin_rounds_to_100_hz();
  test_search_order_ignores_openmv_direction_hint();
  test_boundary_candidates_are_unique_100_hz_grid();
  test_high_search_restarts_flat_quality_curve();
  test_search_count_saturates_without_limit_error();
  test_low_search_holds_local_quality_peak();
  test_fast_settle_and_capture_delay();
  test_high_search_threshold_is_inclusive();
  test_high_search_uses_trend_pair_midpoint();
  test_missed_initial_target_returns_to_valley_minimum();
  test_image_error_retries_forever_without_output_change();
  test_complete_success_flow();
  test_lost_stop_ack_still_holds_frequency();
  test_ack_retry_keeps_sequence();
  test_coarse_wait_has_no_hard_timeout();
  test_bad_coarse_result_restarts_instead_of_error();
  test_active_session_can_be_reselected();
  test_ack_timeout_keeps_dac_running();
  test_uart_send_failure_becomes_visible_error();
  test_uart_retry_failure_becomes_visible_error();
  puts("task5 controller tests passed");
  return 0;
}
