#include "task5_controller.h"

#include <stddef.h>
#include <string.h>

#define TASK5_ACK_TIMEOUT_MS             (100UL)
#define TASK5_ACK_MAX_RETRIES            (3U)
#define TASK5_COARSE_TIMEOUT_MS          (5000UL)
#define TASK5_DDS_RESULT_TIMEOUT_MS      (2000UL)
#define TASK5_RESULT_MAX_RETRIES         (3U)
#define TASK5_DDS_SETTLE_MS              (200UL)
#define TASK5_CAPTURE_DELAY_MS           (200U)
#define TASK5_IMAGE_MAX_RETRIES          (2U)
#define TASK5_SEARCH_MAX_TESTS           (40U)
#define TASK5_MAX_INPUT_FREQUENCY_HZ     (100000UL)
#define TASK5_MAX_DDS_FREQUENCY_HZ       (200000UL)
#define TASK5_SAW_FREQUENCY_1KHZ          (1000UL)
#define TASK5_SAW_FREQUENCY_10KHZ         (10000UL)

#define TASK5_ACK_STATUS_OK              (0x00U)
#define TASK5_ACK_STATUS_DUPLICATE       (0x01U)

#define TASK5_COARSE_OK                  (0x00U)
#define TASK5_COARSE_LOW_CONFIDENCE      (0x01U)

#define TASK5_DDS_TARGET_REACHED         (0x00U)
#define TASK5_DDS_TOO_LOW                (0x01U)
#define TASK5_DDS_TOO_HIGH               (0x02U)
#define TASK5_DDS_NOT_MATCHED            (0x03U)
#define TASK5_DDS_UNSTABLE               (0x04U)
#define TASK5_DDS_LOW_CONFIDENCE         (0x05U)
#define TASK5_DDS_IMAGE_ERROR            (0x06U)
#define TASK5_DDS_TIMEOUT                (0x07U)

static bool Task5_DeadlineReached(uint32_t now, uint32_t deadline)
{
  return (int32_t)(now - deadline) >= 0;
}

static void Task5_Touch(task5_controller_t *controller)
{
  controller->status.revision++;
}

static void Task5_SetState(task5_controller_t *controller,
                           task5_state_t state)
{
  if (controller->status.state != state) {
    controller->status.state = state;
    Task5_Touch(controller);
  }
}

static bool Task5_SendRaw(task5_controller_t *controller,
                          uint8_t type,
                          uint8_t seq,
                          const uint8_t *payload,
                          uint16_t payload_length)
{
  if ((controller->port.send_frame == NULL) ||
      !controller->port.send_frame(
          controller->port.context, type, seq,
          payload, payload_length)) {
    controller->status.tx_queue_error_count++;
    Task5_Touch(controller);
    return false;
  }
  return true;
}

static bool Task5_SendUnreliable(task5_controller_t *controller,
                                 uint8_t type,
                                 const uint8_t *payload,
                                 uint16_t payload_length)
{
  uint8_t seq = controller->next_tx_seq;

  if (!Task5_SendRaw(
          controller, type, seq, payload, payload_length)) {
    return false;
  }
  controller->next_tx_seq++;
  return true;
}

static void Task5_SendAck(task5_controller_t *controller,
                          const openmv_frame_t *frame,
                          uint8_t status)
{
  uint8_t payload[3];

  payload[0] = frame->type;
  payload[1] = frame->seq;
  payload[2] = status;
  (void)Task5_SendUnreliable(
      controller, OPENMV_MSG_ACK, payload, sizeof(payload));
}

static void Task5_SendNack(task5_controller_t *controller,
                           const openmv_frame_t *frame,
                           openmv_nack_code_t error)
{
  uint8_t payload[3];

  payload[0] = frame->type;
  payload[1] = frame->seq;
  payload[2] = (uint8_t)error;
  (void)Task5_SendUnreliable(
      controller, OPENMV_MSG_NACK, payload, sizeof(payload));
}

static void Task5_SendStopBestEffort(
    task5_controller_t *controller,
    uint8_t reason)
{
  uint8_t payload[3];

  if (controller->status.session_id == 0U) {
    return;
  }
  OpenMV_WriteU16LE(payload, controller->status.session_id);
  payload[2] = reason;
  (void)Task5_SendUnreliable(
      controller, OPENMV_MSG_STOP_TASK,
      payload, sizeof(payload));
}

static void Task5_EnterError(task5_controller_t *controller,
                             task5_error_t error)
{
  task5_state_t origin_state;

  if ((controller == NULL) ||
      (controller->status.state == TASK5_STATE_ERROR)) {
    return;
  }

  origin_state = controller->status.state;
  controller->pending.active = false;
  if ((controller->status.saw_frequency_hz != 0UL) &&
      (controller->port.start_saw != NULL)) {
    /*
     * Debug behavior: keep the selected full-scale DAC sawtooth visible
     * after a Task5 failure. This also restores DAC output when the error
     * happened after the controller had already switched to DDS.
     */
    (void)controller->port.start_saw(
        controller->port.context,
        controller->status.saw_frequency_hz);
  }
  controller->status.error_origin_state = origin_state;
  controller->status.last_error = error;
  controller->status.state = TASK5_STATE_ERROR;
  Task5_Touch(controller);
}

static void Task5_BeginReliable(
    task5_controller_t *controller,
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_length,
    task5_state_t wait_state)
{
  controller->pending.active = true;
  controller->pending.sent = false;
  controller->pending.type = type;
  controller->pending.seq = controller->next_tx_seq++;
  controller->pending.retry_count = 0U;
  controller->pending.payload_length = payload_length;
  if (payload_length != 0U) {
    memcpy(controller->pending.payload, payload, payload_length);
  }
  controller->status.ack_retry_count = 0U;
  Task5_SetState(controller, wait_state);
}

static void Task5_ServicePending(task5_controller_t *controller,
                                 uint32_t now_ms)
{
  if (!controller->pending.active) {
    return;
  }

  if (!controller->pending.sent) {
    if ((controller->pending.retry_count != 0U) &&
        !Task5_DeadlineReached(
            now_ms, controller->pending.ack_deadline)) {
      return;
    }
    if (Task5_SendRaw(
            controller,
            controller->pending.type,
            controller->pending.seq,
            controller->pending.payload,
            controller->pending.payload_length)) {
      controller->pending.sent = true;
      controller->pending.retry_count = 0U;
      controller->pending.ack_deadline =
          now_ms + TASK5_ACK_TIMEOUT_MS;
    } else if (controller->pending.retry_count >=
               TASK5_ACK_MAX_RETRIES) {
      Task5_EnterError(
          controller, TASK5_ERROR_UART_SEND);
    } else {
      controller->pending.retry_count++;
      controller->pending.ack_deadline =
          now_ms + TASK5_ACK_TIMEOUT_MS;
    }
    return;
  }

  if (!Task5_DeadlineReached(
          now_ms, controller->pending.ack_deadline)) {
    return;
  }
  if (controller->pending.retry_count >=
      TASK5_ACK_MAX_RETRIES) {
    Task5_EnterError(controller, TASK5_ERROR_ACK_TIMEOUT);
    return;
  }

  if (Task5_SendRaw(
          controller,
          controller->pending.type,
          controller->pending.seq,
          controller->pending.payload,
          controller->pending.payload_length)) {
    controller->pending.retry_count++;
    controller->status.ack_retry_count =
        controller->pending.retry_count;
    controller->pending.ack_deadline =
        now_ms + TASK5_ACK_TIMEOUT_MS;
    Task5_Touch(controller);
  } else if ((uint8_t)(controller->pending.retry_count + 1U) >=
             TASK5_ACK_MAX_RETRIES) {
    Task5_EnterError(
        controller, TASK5_ERROR_UART_SEND);
  } else {
    controller->pending.retry_count++;
    controller->pending.ack_deadline =
        now_ms + TASK5_ACK_TIMEOUT_MS;
  }
}

static bool Task5_ReplayLastCommand(
    task5_controller_t *controller,
    uint32_t now_ms,
    uint32_t result_timeout_ms)
{
  if ((controller->status.result_retry_count >=
       TASK5_RESULT_MAX_RETRIES)) {
    return false;
  }
  if (!Task5_SendRaw(
          controller,
          controller->pending.type,
          controller->pending.seq,
          controller->pending.payload,
          controller->pending.payload_length)) {
    controller->state_deadline = now_ms + 1UL;
    return true;
  }

  controller->status.result_retry_count++;
  controller->state_deadline = now_ms + result_timeout_ms;
  Task5_Touch(controller);
  return true;
}

static uint8_t Task5_SearchStage(
    const task5_controller_t *controller)
{
  uint32_t width;
  uint64_t scaled_width;

  if ((controller->search_lower_hz == 0UL) ||
      (controller->search_upper_hz <=
       controller->search_lower_hz)) {
    return 0x00U;
  }

  width = controller->search_upper_hz -
          controller->search_lower_hz;
  scaled_width = (uint64_t)width * 1000ULL;
  if (scaled_width > (uint64_t)
                         controller->status.dds_frequency_hz *
                         10ULL) {
    return 0x00U;
  }
  if (scaled_width >
      (uint64_t)controller->status.dds_frequency_hz) {
    return 0x01U;
  }
  return 0x02U;
}

static bool Task5_SetDDSAndSettle(
    task5_controller_t *controller,
    uint32_t frequency_hz,
    uint32_t now_ms)
{
  if ((frequency_hz == 0UL) ||
      (frequency_hz > TASK5_MAX_DDS_FREQUENCY_HZ) ||
      (controller->port.set_dds_frequency == NULL) ||
      !controller->port.set_dds_frequency(
          controller->port.context, frequency_hz)) {
    Task5_EnterError(controller, TASK5_ERROR_DDS_OUTPUT);
    return false;
  }

  controller->status.dds_frequency_hz = frequency_hz;
  controller->status.search_stage =
      Task5_SearchStage(controller);
  controller->state_deadline = now_ms + TASK5_DDS_SETTLE_MS;
  Task5_SetState(controller, TASK5_STATE_DDS_SETTLING);
  Task5_Touch(controller);
  return true;
}

static void Task5_SendDDSTest(task5_controller_t *controller,
                              uint32_t now_ms)
{
  uint8_t payload[11];

  if (controller->status.search_count >=
      TASK5_SEARCH_MAX_TESTS) {
    Task5_EnterError(controller, TASK5_ERROR_SEARCH_LIMIT);
    return;
  }

  controller->status.test_id++;
  if (controller->status.test_id == 0U) {
    controller->status.test_id = 1U;
  }
  controller->status.search_count++;
  controller->status.result_retry_count = 0U;

  OpenMV_WriteU16LE(&payload[0],
                    controller->status.session_id);
  OpenMV_WriteU16LE(&payload[2],
                    controller->status.test_id);
  OpenMV_WriteU32LE(&payload[4],
                    controller->status.dds_frequency_hz);
  payload[8] = controller->status.search_stage;
  OpenMV_WriteU16LE(&payload[9],
                    TASK5_CAPTURE_DELAY_MS);
  Task5_BeginReliable(
      controller, OPENMV_MSG_DDS_TEST,
      payload, sizeof(payload), TASK5_STATE_WAIT_DDS_ACK);
  Task5_ServicePending(controller, now_ms);
}

static uint32_t Task5_NextUndirectedFrequency(
    task5_controller_t *controller)
{
  uint32_t radius;
  uint32_t multiplier;
  bool positive;

  controller->undirected_attempt++;
  multiplier =
      (uint32_t)((controller->undirected_attempt + 1U) / 2U);
  positive = (controller->undirected_attempt & 1U) != 0U;
  radius = controller->search_step_hz * multiplier;

  if (positive) {
    if (radius >
        TASK5_MAX_DDS_FREQUENCY_HZ -
            controller->search_origin_hz) {
      return TASK5_MAX_DDS_FREQUENCY_HZ;
    }
    return controller->search_origin_hz + radius;
  }
  return (radius >= controller->search_origin_hz)
             ? 1UL
             : controller->search_origin_hz - radius;
}

static uint32_t Task5_NextDirectedFrequency(
    task5_controller_t *controller,
    uint8_t result)
{
  uint32_t current = controller->status.dds_frequency_hz;
  uint32_t next;

  if (result == TASK5_DDS_TOO_LOW) {
    controller->search_lower_hz = current;
    if (controller->search_upper_hz > current) {
      next = current +
             (controller->search_upper_hz - current) / 2UL;
    } else {
      next = (controller->search_step_hz >
              TASK5_MAX_DDS_FREQUENCY_HZ - current)
                 ? TASK5_MAX_DDS_FREQUENCY_HZ
                 : current + controller->search_step_hz;
      if (controller->search_step_hz <
          TASK5_MAX_DDS_FREQUENCY_HZ / 2UL) {
        controller->search_step_hz *= 2UL;
      }
    }
  } else {
    controller->search_upper_hz = current;
    if ((controller->search_lower_hz != 0UL) &&
        (controller->search_lower_hz < current)) {
      next = controller->search_lower_hz +
             (current - controller->search_lower_hz) / 2UL;
    } else {
      next = (controller->search_step_hz >= current)
                 ? 1UL
                 : current - controller->search_step_hz;
      if (controller->search_step_hz <
          TASK5_MAX_DDS_FREQUENCY_HZ / 2UL) {
        controller->search_step_hz *= 2UL;
      }
    }
  }

  if (next == current) {
    if ((result == TASK5_DDS_TOO_LOW) &&
        (current < TASK5_MAX_DDS_FREQUENCY_HZ)) {
      next++;
    } else if ((result == TASK5_DDS_TOO_HIGH) &&
               (current > 1UL)) {
      next--;
    }
  }
  return next;
}

static bool Task5_IsDuplicateResult(
    const task5_controller_t *controller,
    const openmv_frame_t *frame,
    uint16_t session,
    uint16_t test)
{
  return controller->last_result_valid &&
         (controller->last_result_type == frame->type) &&
         (controller->last_result_seq == frame->seq) &&
         (controller->last_result_session == session) &&
         (controller->last_result_test == test);
}

static void Task5_RememberResult(
    task5_controller_t *controller,
    const openmv_frame_t *frame,
    uint16_t session,
    uint16_t test)
{
  controller->last_result_valid = true;
  controller->last_result_type = frame->type;
  controller->last_result_seq = frame->seq;
  controller->last_result_session = session;
  controller->last_result_test = test;
}

static void Task5_HandleAck(task5_controller_t *controller,
                            const openmv_frame_t *frame,
                            uint32_t now_ms)
{
  uint8_t ack_type;
  uint8_t ack_seq;

  if (frame->length != 3U) {
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  ack_type = frame->payload[0];
  ack_seq = frame->payload[1];
  if (frame->payload[2] > 0x02U) {
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  if (!controller->pending.active ||
      (ack_type != controller->pending.type) ||
      (ack_seq != controller->pending.seq)) {
    return;
  }

  controller->pending.active = false;
  controller->status.result_retry_count = 0U;
  switch (controller->status.state) {
    case TASK5_STATE_WAIT_START_ACK:
      controller->state_deadline =
          now_ms + TASK5_COARSE_TIMEOUT_MS;
      Task5_SetState(
          controller, TASK5_STATE_WAIT_COARSE_RESULT);
      break;
    case TASK5_STATE_WAIT_DDS_ACK:
      controller->state_deadline =
          now_ms + TASK5_DDS_RESULT_TIMEOUT_MS;
      Task5_SetState(
          controller, TASK5_STATE_WAIT_DDS_RESULT);
      break;
    case TASK5_STATE_WAIT_STOP_ACK:
      if (controller->port.start_phase_lock == NULL ||
          !controller->port.start_phase_lock(
              controller->port.context,
              controller->status.mode,
              controller->status.dds_frequency_hz)) {
        Task5_EnterError(
            controller, TASK5_ERROR_PHASE_LOCK);
        return;
      }
      Task5_SetState(
          controller, TASK5_STATE_PHASE_LOCKING);
      break;
    default:
      break;
  }
}

static void Task5_HandleNack(task5_controller_t *controller,
                             const openmv_frame_t *frame,
                             uint32_t now_ms)
{
  if (frame->length != 3U) {
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  if (!controller->pending.active ||
      (frame->payload[0] != controller->pending.type) ||
      (frame->payload[1] != controller->pending.seq)) {
    return;
  }

  controller->status.last_nack_code = frame->payload[2];
  Task5_Touch(controller);
  if (frame->payload[2] == OPENMV_NACK_BUSY) {
    controller->pending.ack_deadline = now_ms + 200UL;
    return;
  }
  Task5_EnterError(controller, TASK5_ERROR_NACK);
}

static void Task5_HandleCoarseResult(
    task5_controller_t *controller,
    const openmv_frame_t *frame,
    uint32_t now_ms)
{
  uint16_t session;
  uint8_t result;
  uint32_t ratio_x1000;
  uint64_t estimated_input;
  uint64_t estimated;
  uint8_t multiplier;

  if (frame->length != 10U) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_BAD_LENGTH);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  session = OpenMV_ReadU16LE(&frame->payload[0]);
  if (session != controller->status.session_id) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_BAD_SESSION);
    controller->status.stale_result_count++;
    Task5_Touch(controller);
    return;
  }
  if (Task5_IsDuplicateResult(
          controller, frame, session, 0U)) {
    Task5_SendAck(
        controller, frame, TASK5_ACK_STATUS_DUPLICATE);
    controller->status.duplicate_result_count++;
    Task5_Touch(controller);
    return;
  }
  if (controller->status.state !=
      TASK5_STATE_WAIT_COARSE_RESULT) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_BAD_STATE);
    return;
  }

  result = frame->payload[2];
  ratio_x1000 = OpenMV_ReadU32LE(&frame->payload[3]);
  if (frame->payload[7] > 100U) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_OUT_OF_RANGE);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  if ((result > TASK5_COARSE_LOW_CONFIDENCE) ||
      (ratio_x1000 == 0UL)) {
    Task5_SendAck(controller, frame, TASK5_ACK_STATUS_OK);
    Task5_RememberResult(
        controller, frame, session, 0U);
    Task5_EnterError(controller, TASK5_ERROR_BAD_RESULT);
    return;
  }

  multiplier =
      (controller->status.mode ==
       TASK5_MODE_INFINITY_2X_0_DEG)
          ? 2U
          : 1U;
  estimated_input =
      ((uint64_t)controller->status.saw_frequency_hz *
       ratio_x1000 + 500ULL) /
      1000ULL;
  estimated = estimated_input * multiplier;
  if ((estimated_input == 0ULL) ||
      (estimated_input > TASK5_MAX_INPUT_FREQUENCY_HZ) ||
      (estimated > TASK5_MAX_DDS_FREQUENCY_HZ)) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_OUT_OF_RANGE);
    Task5_EnterError(controller, TASK5_ERROR_BAD_RESULT);
    return;
  }

  Task5_SendAck(controller, frame, TASK5_ACK_STATUS_OK);
  Task5_RememberResult(controller, frame, session, 0U);
  controller->status.last_openmv_result = result;
  controller->status.estimated_input_frequency_hz =
      (uint32_t)estimated_input;
  controller->search_origin_hz = (uint32_t)estimated;
  controller->search_step_hz =
      controller->search_origin_hz / 20UL;
  if (controller->search_step_hz == 0UL) {
    controller->search_step_hz = 1UL;
  }
  controller->search_lower_hz = 0UL;
  controller->search_upper_hz = 0UL;
  controller->undirected_attempt = 0U;
  controller->image_retry_count = 0U;
  if (controller->port.stop_saw != NULL) {
    controller->port.stop_saw(controller->port.context);
  }
  (void)Task5_SetDDSAndSettle(
      controller, controller->search_origin_hz, now_ms);
}

static void Task5_HandleDDSTestResult(
    task5_controller_t *controller,
    const openmv_frame_t *frame,
    uint32_t now_ms)
{
  uint16_t session;
  uint16_t test;
  uint8_t result;
  uint32_t next_frequency;
  uint8_t stop_payload[3];

  if (frame->length != 8U) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_BAD_LENGTH);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  session = OpenMV_ReadU16LE(&frame->payload[0]);
  test = OpenMV_ReadU16LE(&frame->payload[2]);
  if (session != controller->status.session_id) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_BAD_SESSION);
    controller->status.stale_result_count++;
    Task5_Touch(controller);
    return;
  }
  if (Task5_IsDuplicateResult(
          controller, frame, session, test)) {
    Task5_SendAck(
        controller, frame, TASK5_ACK_STATUS_DUPLICATE);
    controller->status.duplicate_result_count++;
    Task5_Touch(controller);
    return;
  }
  if ((controller->status.state !=
       TASK5_STATE_WAIT_DDS_RESULT) ||
      (test != controller->status.test_id)) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_BAD_STATE);
    controller->status.stale_result_count++;
    Task5_Touch(controller);
    return;
  }

  result = frame->payload[4];
  if ((result > TASK5_DDS_TIMEOUT) ||
      (OpenMV_ReadU16LE(&frame->payload[5]) > 1000U) ||
      (frame->payload[7] > 100U)) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_OUT_OF_RANGE);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }

  Task5_SendAck(controller, frame, TASK5_ACK_STATUS_OK);
  Task5_RememberResult(controller, frame, session, test);
  controller->status.last_openmv_result = result;
  controller->status.result_retry_count = 0U;
  Task5_Touch(controller);

  if (result == TASK5_DDS_TARGET_REACHED) {
    OpenMV_WriteU16LE(
        &stop_payload[0], controller->status.session_id);
    stop_payload[2] = 0x00U;
    Task5_BeginReliable(
        controller, OPENMV_MSG_STOP_TASK,
        stop_payload, sizeof(stop_payload),
        TASK5_STATE_WAIT_STOP_ACK);
    Task5_ServicePending(controller, now_ms);
    return;
  }

  if ((result == TASK5_DDS_TOO_LOW) ||
      (result == TASK5_DDS_TOO_HIGH)) {
    controller->image_retry_count = 0U;
    next_frequency = Task5_NextDirectedFrequency(
        controller, result);
    (void)Task5_SetDDSAndSettle(
        controller, next_frequency, now_ms);
    return;
  }

  if (result == TASK5_DDS_NOT_MATCHED) {
    controller->image_retry_count = 0U;
    next_frequency =
        Task5_NextUndirectedFrequency(controller);
    (void)Task5_SetDDSAndSettle(
        controller, next_frequency, now_ms);
    return;
  }

  if (controller->image_retry_count <
      TASK5_IMAGE_MAX_RETRIES) {
    controller->image_retry_count++;
    controller->state_deadline =
        now_ms + TASK5_DDS_SETTLE_MS;
    Task5_SetState(controller, TASK5_STATE_DDS_SETTLING);
    return;
  }
  Task5_EnterError(controller, TASK5_ERROR_BAD_RESULT);
}

bool Task5_Init(task5_controller_t *controller,
                const task5_port_t *port)
{
  if ((controller == NULL) || (port == NULL) ||
      (port->start_saw == NULL) ||
      (port->stop_saw == NULL) ||
      (port->set_dds_frequency == NULL) ||
      (port->start_phase_lock == NULL) ||
      (port->safe_outputs == NULL) ||
      (port->send_frame == NULL)) {
    return false;
  }

  memset(controller, 0, sizeof(*controller));
  controller->port = *port;
  controller->next_session_id = 1U;
  controller->status.state = TASK5_STATE_INACTIVE;
  return true;
}

void Task5_Enter(task5_controller_t *controller)
{
  uint16_t next_session_id;
  uint8_t next_tx_seq;
  task5_port_t port;

  if (controller == NULL) {
    return;
  }
  next_session_id = controller->next_session_id;
  next_tx_seq = controller->next_tx_seq;
  port = controller->port;
  memset(controller, 0, sizeof(*controller));
  controller->port = port;
  controller->next_session_id =
      (next_session_id == 0U) ? 1U : next_session_id;
  controller->next_tx_seq = next_tx_seq;
  controller->status.state = TASK5_STATE_WAIT_SELECTION;
  Task5_Touch(controller);
}

bool Task5_Start(task5_controller_t *controller,
                 task5_lock_mode_t mode,
                 uint32_t saw_frequency_hz,
                 uint32_t now_ms)
{
  uint8_t payload[7];

  if ((controller == NULL) ||
      (controller->status.state == TASK5_STATE_INACTIVE) ||
      (mode > TASK5_MODE_INFINITY_2X_0_DEG) ||
      ((saw_frequency_hz != TASK5_SAW_FREQUENCY_1KHZ) &&
       (saw_frequency_hz != TASK5_SAW_FREQUENCY_10KHZ))) {
    return false;
  }

  /*
   * Task5 mode keys stay live for the complete Task5 lifetime.  Cancel the
   * previous OpenMV session before starting the newly selected mode.  STOP
   * and START are queued in this order, while the new session id prevents a
   * late response from the old session from advancing the new workflow.
   */
  if (controller->status.state != TASK5_STATE_WAIT_SELECTION) {
    Task5_Exit(controller, 0x04U);
    Task5_Enter(controller);
  }

  controller->status.mode = mode;
  controller->status.saw_frequency_hz = saw_frequency_hz;
  controller->status.session_id =
      controller->next_session_id++;
  if (controller->next_session_id == 0U) {
    controller->next_session_id = 1U;
  }
  controller->status.last_error = TASK5_ERROR_NONE;
  controller->last_result_valid = false;

  if (!controller->port.start_saw(
          controller->port.context, saw_frequency_hz)) {
    Task5_EnterError(controller, TASK5_ERROR_SAW_START);
    return false;
  }

  OpenMV_WriteU16LE(
      &payload[0], controller->status.session_id);
  payload[2] = (uint8_t)mode;
  OpenMV_WriteU32LE(&payload[3], saw_frequency_hz);
  Task5_BeginReliable(
      controller, OPENMV_MSG_START_TASK,
      payload, sizeof(payload), TASK5_STATE_WAIT_START_ACK);
  Task5_ServicePending(controller, now_ms);
  return controller->status.state != TASK5_STATE_ERROR;
}

void Task5_Exit(task5_controller_t *controller,
                uint8_t stop_reason)
{
  if (controller == NULL) {
    return;
  }

  if ((controller->status.state != TASK5_STATE_INACTIVE) &&
      (controller->status.state !=
       TASK5_STATE_WAIT_SELECTION)) {
    Task5_SendStopBestEffort(controller, stop_reason);
  }
  controller->pending.active = false;
  controller->port.stop_saw(controller->port.context);
  controller->port.safe_outputs(controller->port.context);
  controller->status.state = TASK5_STATE_INACTIVE;
  Task5_Touch(controller);
}

void Task5_Process(task5_controller_t *controller,
                   uint32_t now_ms)
{
  if (controller == NULL) {
    return;
  }

  Task5_ServicePending(controller, now_ms);
  if (controller->status.state == TASK5_STATE_ERROR) {
    return;
  }

  switch (controller->status.state) {
    case TASK5_STATE_WAIT_COARSE_RESULT:
      if (Task5_DeadlineReached(
              now_ms, controller->state_deadline) &&
          !Task5_ReplayLastCommand(
              controller, now_ms,
              TASK5_COARSE_TIMEOUT_MS)) {
        Task5_EnterError(
            controller, TASK5_ERROR_RESULT_TIMEOUT);
      }
      break;
    case TASK5_STATE_DDS_SETTLING:
      if (Task5_DeadlineReached(
              now_ms, controller->state_deadline)) {
        Task5_SendDDSTest(controller, now_ms);
      }
      break;
    case TASK5_STATE_WAIT_DDS_RESULT:
      if (Task5_DeadlineReached(
              now_ms, controller->state_deadline) &&
          !Task5_ReplayLastCommand(
              controller, now_ms,
              TASK5_DDS_RESULT_TIMEOUT_MS)) {
        Task5_EnterError(
            controller, TASK5_ERROR_RESULT_TIMEOUT);
      }
      break;
    default:
      break;
  }
}

void Task5_OnFrame(task5_controller_t *controller,
                   const openmv_frame_t *frame,
                   uint32_t now_ms)
{
  if ((controller == NULL) || (frame == NULL)) {
    return;
  }

  if (frame->version != OPENMV_PROTOCOL_VERSION) {
    Task5_SendNack(
        controller, frame,
        OPENMV_NACK_UNSUPPORTED_VERSION);
    return;
  }

  switch (frame->type) {
    case OPENMV_MSG_ACK:
      Task5_HandleAck(controller, frame, now_ms);
      break;
    case OPENMV_MSG_NACK:
      Task5_HandleNack(controller, frame, now_ms);
      break;
    case OPENMV_MSG_COARSE_RESULT:
      Task5_HandleCoarseResult(controller, frame, now_ms);
      break;
    case OPENMV_MSG_DDS_TEST_RESULT:
      Task5_HandleDDSTestResult(
          controller, frame, now_ms);
      break;
    case OPENMV_MSG_OPENMV_ERROR:
      Task5_SendAck(controller, frame, TASK5_ACK_STATUS_OK);
      Task5_EnterError(
          controller, TASK5_ERROR_OPENMV_REPORTED);
      break;
    case OPENMV_MSG_HEARTBEAT:
      Task5_SendAck(controller, frame, TASK5_ACK_STATUS_OK);
      break;
    default:
      Task5_SendNack(
          controller, frame, OPENMV_NACK_BAD_STATE);
      break;
  }
}

void Task5_NotifyPhaseLock(task5_controller_t *controller,
                           bool locked,
                           bool error)
{
  if ((controller == NULL) ||
      (controller->status.state !=
       TASK5_STATE_PHASE_LOCKING &&
       controller->status.state != TASK5_STATE_LOCKED)) {
    return;
  }
  if (error) {
    Task5_EnterError(controller, TASK5_ERROR_PHASE_LOCK);
  } else if (locked) {
    Task5_SetState(controller, TASK5_STATE_LOCKED);
  } else if (controller->status.state == TASK5_STATE_LOCKED) {
    Task5_SetState(controller, TASK5_STATE_PHASE_LOCKING);
  }
}

void Task5_GetStatus(const task5_controller_t *controller,
                     task5_status_t *status)
{
  if ((controller == NULL) || (status == NULL)) {
    return;
  }
  *status = controller->status;
}
