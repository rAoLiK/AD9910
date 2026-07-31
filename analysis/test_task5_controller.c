#include "task5_controller.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
  ((fake_port_t *)context)->dds_frequency_hz = frequency_hz;
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
  frame.payload[4] = 0U;
  OpenMV_WriteU16LE(&frame.payload[5], 900U);
  frame.payload[7] = 95U;
  result_seq = frame.seq;
  Task5_OnFrame(&controller, &frame, 250U);
  assert(fake.type == OPENMV_MSG_STOP_TASK);

  frame = ack_for(&fake);
  Task5_OnFrame(&controller, &frame, 260U);
  assert(fake.phase_started);
  assert(fake.seed_frequency_hz == 5200U);
  Task5_NotifyPhaseLock(&controller, true, false);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_LOCKED);

  /* A duplicated result is ACKed but must not restart phase locking. */
  memset(&frame, 0, sizeof(frame));
  frame.version = OPENMV_PROTOCOL_VERSION;
  frame.type = OPENMV_MSG_DDS_TEST_RESULT;
  frame.seq = result_seq;
  frame.length = 8U;
  OpenMV_WriteU16LE(&frame.payload[0], status.session_id);
  OpenMV_WriteU16LE(&frame.payload[2], status.test_id);
  frame.payload[4] = 0U;
  Task5_OnFrame(&controller, &frame, 270U);
  Task5_GetStatus(&controller, &status);
  assert(status.state == TASK5_STATE_LOCKED);
  assert(status.duplicate_result_count == 1U);
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
  test_complete_success_flow();
  test_ack_retry_keeps_sequence();
  test_active_session_can_be_reselected();
  test_ack_timeout_keeps_dac_running();
  test_uart_send_failure_becomes_visible_error();
  test_uart_retry_failure_becomes_visible_error();
  puts("task5 controller tests passed");
  return 0;
}
