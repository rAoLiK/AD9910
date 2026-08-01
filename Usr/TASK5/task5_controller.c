#include "task5_controller.h"

#include <stddef.h>
#include <string.h>

#define TASK5_ACK_TIMEOUT_MS             (100UL)
#define TASK5_ACK_MAX_RETRIES            (3U)
#define TASK5_COARSE_REPLAY_MS           (5000UL)
#define TASK5_DDS_RESULT_TIMEOUT_MS      (3000UL)
#define TASK5_RESULT_MAX_RETRIES         (3U)
#define TASK5_DDS_SETTLE_MS              (20UL)
#define TASK5_VISUAL_SAMPLE_TIMEOUT_MS    (1500UL)
#define TASK5_CAPTURE_DELAY_MS           (80U)
#define TASK5_CONFIRM_CAPTURE_DELAY_MS   (160U)
#define TASK5_FINE_SEARCH_STEP_HZ        (100UL)
#define TASK5_HIGH_COARSE_STEP_HZ        (1000UL)
#define TASK5_HIGH_FINE_HALF_WINDOW_HZ   (500UL)
#define TASK5_HIGH_SEARCH_THRESHOLD_HZ   (10000UL)
#define TASK5_HIGH_COARSE_MAX_RADIUS     (8U)
#define TASK5_LOW_MAX_RADIUS             (20U)
#define TASK5_TREND_RISE_MARGIN          (50U)
#define TASK5_TREND_RISE_CONFIRMATIONS   (2U)
#define TASK5_SUSPICION_MIN_QUALITY      (450U)
#define TASK5_CONFIRM_MIN_QUALITY        (350U)
#define TASK5_MIN_DDS_FREQUENCY_HZ       (100UL)
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

#define TASK5_SEARCH_STAGE_COARSE_1KHZ   (0x01U)
#define TASK5_SEARCH_STAGE_FINE_100HZ    (0x02U)
#define TASK5_SEARCH_STAGE_CONFIRM       (0x03U)

#define TASK5_DIRECTION_NEGATIVE         (0U)
#define TASK5_DIRECTION_POSITIVE         (1U)

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

static bool Task5_StartLocalPhaseLock(task5_controller_t *controller)
{
  uint32_t multiplier =
      (controller->status.mode == TASK5_MODE_INFINITY_2X_0_DEG)
          ? 2UL
          : 1UL;
  uint64_t seed_millihz =
      (uint64_t)controller->status.visual_frequency_millihz * multiplier;

  if ((seed_millihz == 0ULL) ||
      (seed_millihz >
       ((uint64_t)TASK5_MAX_DDS_FREQUENCY_HZ * 1000ULL)) ||
      (controller->port.start_phase_lock == NULL) ||
      !controller->port.start_phase_lock(
          controller->port.context,
          controller->status.mode,
          (uint32_t)seed_millihz)) {
    Task5_EnterError(controller, TASK5_ERROR_PHASE_LOCK);
    return false;
  }
  controller->status.dds_frequency_hz =
      (uint32_t)((seed_millihz + 500ULL) / 1000ULL);
  Task5_SetState(controller, TASK5_STATE_PHASE_LOCKING);
  Task5_Touch(controller);
  return true;
}

static bool Task5_FinishStopWithoutAck(
    task5_controller_t *controller)
{
  if ((controller->status.state != TASK5_STATE_WAIT_STOP_ACK) ||
      (controller->pending.type != OPENMV_MSG_STOP_TASK)) {
    return false;
  }

  /* Visual lock already established success. A lost STOP ACK must not block
   * the local high-rate ADC loop or replace the DDS tone with diagnostics. */
  controller->pending.active = false;
  controller->pending.sent = false;
  return Task5_StartLocalPhaseLock(controller);
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
      if (!Task5_FinishStopWithoutAck(controller)) {
        Task5_EnterError(
            controller, TASK5_ERROR_UART_SEND);
      }
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
    if (!Task5_FinishStopWithoutAck(controller)) {
      Task5_EnterError(controller, TASK5_ERROR_ACK_TIMEOUT);
    }
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
    if (!Task5_FinishStopWithoutAck(controller)) {
      Task5_EnterError(
          controller, TASK5_ERROR_UART_SEND);
    }
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

static void Task5_KeepWaitingForCoarseResult(
    task5_controller_t *controller,
    uint32_t now_ms)
{
  /*
   * Coarse recognition can legitimately take longer with the 1820-sample
   * model. Replay the original START periodically so OpenMV can resend a
   * result frame that was lost, but do not convert recognition time into an
   * application error. The live Task5 buttons remain the explicit cancel /
   * reselect path.
   */
  if (Task5_SendRaw(
          controller,
          controller->pending.type,
          controller->pending.seq,
          controller->pending.payload,
          controller->pending.payload_length)) {
    if (controller->status.result_retry_count < UINT8_MAX) {
      controller->status.result_retry_count++;
    }
    Task5_Touch(controller);
  }
  controller->state_deadline = now_ms + TASK5_COARSE_REPLAY_MS;
}

static uint32_t Task5_RoundToGrid(uint32_t frequency_hz,
                                  uint32_t step_hz)
{
  uint32_t rounded;

  if (frequency_hz >=
      (TASK5_MAX_DDS_FREQUENCY_HZ -
       (step_hz / 2UL))) {
    return TASK5_MAX_DDS_FREQUENCY_HZ;
  }
  rounded =
      ((frequency_hz + (step_hz / 2UL)) / step_hz) *
      step_hz;
  if (rounded < TASK5_MIN_DDS_FREQUENCY_HZ) {
    rounded = TASK5_MIN_DDS_FREQUENCY_HZ;
  }
  return rounded;
}

static bool Task5_SetDDSAndSettle(
    task5_controller_t *controller,
    uint32_t frequency_hz,
    uint32_t now_ms)
{
  bool new_candidate =
      (controller->status.search_count == 0U) ||
      (frequency_hz != controller->status.dds_frequency_hz);

  if ((frequency_hz == 0UL) ||
      (frequency_hz > TASK5_MAX_DDS_FREQUENCY_HZ) ||
      (controller->port.set_dds_frequency == NULL) ||
      !controller->port.set_dds_frequency(
          controller->port.context, frequency_hz)) {
    Task5_EnterError(controller, TASK5_ERROR_DDS_OUTPUT);
    return false;
  }

  if (new_candidate &&
      (controller->status.search_count < UINT8_MAX)) {
    controller->status.search_count++;
  }
  controller->status.dds_frequency_hz = frequency_hz;
  controller->state_deadline = now_ms + TASK5_DDS_SETTLE_MS;
  Task5_SetState(controller, TASK5_STATE_DDS_SETTLING);
  Task5_Touch(controller);
  return true;
}

static void Task5_SendDDSTest(task5_controller_t *controller,
                              uint32_t now_ms)
{
  uint8_t payload[11];

  controller->status.test_id++;
  if (controller->status.test_id == 0U) {
    controller->status.test_id = 1U;
  }
  controller->status.result_retry_count = 0U;

  OpenMV_WriteU16LE(&payload[0],
                    controller->status.session_id);
  OpenMV_WriteU16LE(&payload[2],
                    controller->status.test_id);
  OpenMV_WriteU32LE(&payload[4],
                    controller->status.dds_frequency_hz);
  payload[8] = controller->status.search_stage;
  OpenMV_WriteU16LE(&payload[9],
                    (controller->status.search_stage ==
                     TASK5_SEARCH_STAGE_CONFIRM)
                        ? TASK5_CONFIRM_CAPTURE_DELAY_MS
                        : TASK5_CAPTURE_DELAY_MS);
  Task5_BeginReliable(
      controller, OPENMV_MSG_DDS_TEST,
      payload, sizeof(payload), TASK5_STATE_WAIT_DDS_ACK);
  Task5_ServicePending(controller, now_ms);
}

static uint16_t Task5_ResultQuality(uint16_t score,
                                    uint8_t confidence)
{
  uint16_t confidence_score = (uint16_t)confidence * 10U;

  /* A broad swept ellipse can retain a high geometry score while receiving
   * few family votes.  The weaker of score and confidence is therefore the
   * useful trend quantity; distance = 1000 - quality is minimized at the
   * most ellipse-like candidate. */
  return (score < confidence_score) ? score : confidence_score;
}

static void Task5_UpdateBestQuality(
    task5_controller_t *controller,
    uint32_t frequency_hz,
    uint16_t distance)
{
  uint16_t quality = (uint16_t)(1000U - distance);

  if ((controller->status.best_match_frequency_hz == 0UL) ||
      (quality > controller->status.best_match_quality)) {
    controller->status.best_match_frequency_hz = frequency_hz;
    controller->status.best_match_quality = quality;
  }
}

static void Task5_SeedTrend(task5_search_trend_t *trend,
                            uint32_t frequency_hz,
                            uint16_t distance)
{
  memset(trend, 0, sizeof(*trend));
  trend->seeded = true;
  trend->sample_count = 1U;
  trend->previous_frequency_hz = frequency_hz;
  trend->previous_distance = distance;
  trend->best_frequency_hz = frequency_hz;
  trend->best_distance = distance;
  trend->best_pair_cost = UINT32_MAX;
}

static void Task5_AddTrendSample(task5_search_trend_t *trend,
                                 uint32_t frequency_hz,
                                 uint16_t distance)
{
  uint32_t pair_cost;
  uint32_t pair_lower;
  uint32_t pair_upper;

  if (!trend->seeded) {
    Task5_SeedTrend(trend, frequency_hz, distance);
    return;
  }

  pair_cost =
      (uint32_t)trend->previous_distance + (uint32_t)distance;
  pair_lower =
      (trend->previous_frequency_hz < frequency_hz)
          ? trend->previous_frequency_hz
          : frequency_hz;
  pair_upper =
      (trend->previous_frequency_hz > frequency_hz)
          ? trend->previous_frequency_hz
          : frequency_hz;
  if (pair_cost < trend->best_pair_cost) {
    /* Only adjacent points from the same monotonic direction form a valid
     * 1 kHz suspicion interval. */
    trend->best_pair_cost = pair_cost;
    trend->best_pair_lower_hz = pair_lower;
    trend->best_pair_upper_hz = pair_upper;
  }

  if (distance < trend->best_distance) {
    trend->best_distance = distance;
    trend->best_frequency_hz = frequency_hz;
    trend->rise_count = 0U;
  } else if (distance >
             (uint16_t)(trend->best_distance +
                        TASK5_TREND_RISE_MARGIN)) {
    if (trend->rise_count < UINT8_MAX) {
      trend->rise_count++;
    }
  } else {
    trend->rise_count = 0U;
  }

  if (trend->sample_count < UINT8_MAX) {
    trend->sample_count++;
  }
  trend->previous_frequency_hz = frequency_hz;
  trend->previous_distance = distance;
  if ((trend->sample_count >= 3U) &&
      (trend->rise_count >=
       TASK5_TREND_RISE_CONFIRMATIONS)) {
    /* Two clear rises after the directional minimum bracket the valley
     * without forcing the other direction to waste the same number of
     * samples. */
    trend->complete = true;
  }
}

static void Task5_RecordDirectionalSample(
    task5_controller_t *controller,
    uint16_t distance)
{
  uint32_t frequency_hz = controller->status.dds_frequency_hz;
  uint8_t direction;

  Task5_UpdateBestQuality(controller, frequency_hz, distance);
  if (frequency_hz == controller->search_origin_hz) {
    Task5_SeedTrend(
        &controller->coarse_trend[TASK5_DIRECTION_NEGATIVE],
        frequency_hz, distance);
    Task5_SeedTrend(
        &controller->coarse_trend[TASK5_DIRECTION_POSITIVE],
        frequency_hz, distance);
    return;
  }

  direction =
      (frequency_hz < controller->search_origin_hz)
          ? TASK5_DIRECTION_NEGATIVE
          : TASK5_DIRECTION_POSITIVE;
  Task5_AddTrendSample(
      &controller->coarse_trend[direction],
      frequency_hz, distance);
}

static void Task5_ResetSearchWindow(
    task5_controller_t *controller,
    uint32_t origin_hz,
    uint32_t step_hz,
    uint8_t search_stage)
{
  controller->search_origin_hz =
      Task5_RoundToGrid(origin_hz, step_hz);
  controller->search_step_hz = step_hz;
  controller->search_lower_hz = 0UL;
  controller->search_upper_hz = 0UL;
  controller->undirected_attempt = 0U;
  controller->image_retry_count = 0U;
  memset(
      controller->coarse_trend, 0,
      sizeof(controller->coarse_trend));
  controller->fine_best_distance = UINT16_MAX;
  controller->fine_worst_distance = 0U;
  controller->fine_best_frequency_hz = 0UL;
  controller->confirmation_peak_valid = false;
  controller->status.best_match_quality = 0U;
  controller->status.best_match_frequency_hz = 0UL;
  controller->status.search_stage = search_stage;
}

static const task5_search_trend_t *
Task5_SelectBestDirectionalTrend(
    const task5_controller_t *controller)
{
  const task5_search_trend_t *negative =
      &controller->coarse_trend[TASK5_DIRECTION_NEGATIVE];
  const task5_search_trend_t *positive =
      &controller->coarse_trend[TASK5_DIRECTION_POSITIVE];

  if (!negative->seeded) {
    return positive->seeded ? positive : NULL;
  }
  if (!positive->seeded) {
    return negative;
  }
  return (positive->best_distance < negative->best_distance)
             ? positive
             : negative;
}

static uint32_t Task5_NextHighCoarseFrequency(
    task5_controller_t *controller)
{
  uint32_t radius;
  uint32_t multiplier;
  uint8_t direction;
  task5_search_trend_t *trend;

  while (controller->undirected_attempt <
         (uint8_t)(2U * TASK5_HIGH_COARSE_MAX_RADIUS)) {
    controller->undirected_attempt++;
    multiplier =
        (uint32_t)((controller->undirected_attempt + 1U) / 2U);
    direction =
        ((controller->undirected_attempt & 1U) != 0U)
            ? TASK5_DIRECTION_POSITIVE
            : TASK5_DIRECTION_NEGATIVE;
    trend = &controller->coarse_trend[direction];
    if (trend->complete) {
      continue;
    }

    radius = TASK5_HIGH_COARSE_STEP_HZ * multiplier;
    if (direction == TASK5_DIRECTION_POSITIVE) {
      if (radius <=
          TASK5_MAX_DDS_FREQUENCY_HZ -
              controller->search_origin_hz) {
        return controller->search_origin_hz + radius;
      }
    } else if (radius <=
               controller->search_origin_hz -
                   TASK5_MIN_DDS_FREQUENCY_HZ) {
      return controller->search_origin_hz - radius;
    }
    trend->complete = true;
  }
  return 0UL;
}

static uint32_t Task5_NextLowFrequency(
    task5_controller_t *controller)
{
  uint32_t radius;
  uint32_t multiplier;
  uint8_t direction;
  task5_search_trend_t *trend;

  while (controller->undirected_attempt <
         (uint8_t)(2U * TASK5_LOW_MAX_RADIUS)) {
    controller->undirected_attempt++;
    multiplier =
        (uint32_t)((controller->undirected_attempt + 1U) / 2U);
    direction =
        ((controller->undirected_attempt & 1U) != 0U)
            ? TASK5_DIRECTION_POSITIVE
            : TASK5_DIRECTION_NEGATIVE;
    trend = &controller->coarse_trend[direction];
    if (trend->complete) {
      continue;
    }

    radius = TASK5_FINE_SEARCH_STEP_HZ * multiplier;
    if (direction == TASK5_DIRECTION_POSITIVE) {
      if (radius <=
          TASK5_MAX_DDS_FREQUENCY_HZ -
              controller->search_origin_hz) {
        return controller->search_origin_hz + radius;
      }
    } else if (radius <=
               controller->search_origin_hz -
                   TASK5_MIN_DDS_FREQUENCY_HZ) {
      return controller->search_origin_hz - radius;
    }
    trend->complete = true;
  }
  return 0UL;
}

static bool Task5_RestartHighSearch(
    task5_controller_t *controller,
    uint32_t preferred_frequency_hz,
    uint32_t now_ms)
{
  uint32_t origin_hz = preferred_frequency_hz;

  if (origin_hz == 0UL) {
    origin_hz = controller->status.best_match_frequency_hz;
  }
  if (origin_hz == 0UL) {
    origin_hz = controller->status.dds_frequency_hz;
  }
  controller->status.search_restart_count++;
  Task5_ResetSearchWindow(
      controller, origin_hz,
      TASK5_HIGH_COARSE_STEP_HZ,
      TASK5_SEARCH_STAGE_COARSE_1KHZ);
  return Task5_SetDDSAndSettle(
      controller, controller->search_origin_hz, now_ms);
}

static bool Task5_RestartLowSearch(
    task5_controller_t *controller,
    uint32_t preferred_frequency_hz,
    uint32_t now_ms)
{
  uint32_t origin_hz = preferred_frequency_hz;

  if (origin_hz == 0UL) {
    origin_hz = controller->status.best_match_frequency_hz;
  }
  if (origin_hz == 0UL) {
    origin_hz = controller->status.dds_frequency_hz;
  }
  controller->status.search_restart_count++;
  Task5_ResetSearchWindow(
      controller, origin_hz,
      TASK5_FINE_SEARCH_STEP_HZ,
      TASK5_SEARCH_STAGE_FINE_100HZ);
  return Task5_SetDDSAndSettle(
      controller, controller->search_origin_hz, now_ms);
}

static bool Task5_BeginLowConfirmation(
    task5_controller_t *controller,
    uint32_t now_ms)
{
  const task5_search_trend_t *selected =
      Task5_SelectBestDirectionalTrend(controller);

  if (selected == NULL) {
    return Task5_RestartLowSearch(
        controller, controller->status.dds_frequency_hz,
        now_ms);
  }
  controller->fine_best_frequency_hz =
      selected->best_frequency_hz;
  controller->fine_best_distance = selected->best_distance;
  controller->confirmation_peak_valid = selected->complete;
  controller->status.search_stage = TASK5_SEARCH_STAGE_CONFIRM;
  return Task5_SetDDSAndSettle(
      controller, controller->fine_best_frequency_hz, now_ms);
}

static bool Task5_BeginFineSearch(
    task5_controller_t *controller,
    uint32_t now_ms)
{
  uint32_t center;

  if (controller->status.best_match_frequency_hz == 0UL) {
    return Task5_RestartHighSearch(
        controller, controller->status.dds_frequency_hz,
        now_ms);
  }

  /* A narrow, visually obvious coarse peak must not lose to a broader but
   * mediocre adjacent pair.  The nearest 1 kHz coarse point is at most
   * 500 Hz from the true target, so center the complete 100 Hz scan on the
   * single best coarse sample. */
  center = Task5_RoundToGrid(
      controller->status.best_match_frequency_hz,
      TASK5_FINE_SEARCH_STEP_HZ);
  controller->search_origin_hz = center;
  controller->search_lower_hz =
      (center > (TASK5_MIN_DDS_FREQUENCY_HZ +
                 TASK5_HIGH_FINE_HALF_WINDOW_HZ))
          ? center - TASK5_HIGH_FINE_HALF_WINDOW_HZ
          : TASK5_MIN_DDS_FREQUENCY_HZ;
  controller->search_upper_hz =
      (center <= (TASK5_MAX_DDS_FREQUENCY_HZ -
                  TASK5_HIGH_FINE_HALF_WINDOW_HZ))
          ? center + TASK5_HIGH_FINE_HALF_WINDOW_HZ
          : TASK5_MAX_DDS_FREQUENCY_HZ;
  controller->search_step_hz = TASK5_FINE_SEARCH_STEP_HZ;
  controller->undirected_attempt = 0U;
  controller->fine_best_distance = UINT16_MAX;
  controller->fine_worst_distance = 0U;
  controller->fine_best_frequency_hz = 0UL;
  controller->confirmation_peak_valid = false;
  controller->status.search_stage =
      TASK5_SEARCH_STAGE_FINE_100HZ;
  return Task5_SetDDSAndSettle(
      controller, controller->search_origin_hz, now_ms);
}

static void Task5_RecordFineSample(
    task5_controller_t *controller,
    uint16_t distance)
{
  uint32_t frequency_hz = controller->status.dds_frequency_hz;

  Task5_UpdateBestQuality(controller, frequency_hz, distance);
  if ((controller->fine_best_frequency_hz == 0UL) ||
      (distance < controller->fine_best_distance)) {
    controller->fine_best_frequency_hz = frequency_hz;
    controller->fine_best_distance = distance;
  }
  if (distance > controller->fine_worst_distance) {
    controller->fine_worst_distance = distance;
  }
}

static uint32_t Task5_NextFineFrequency(
    task5_controller_t *controller)
{
  uint32_t radius;
  uint32_t multiplier;
  bool positive;
  uint32_t maximum_radius =
      controller->search_upper_hz -
      controller->search_origin_hz;
  uint32_t negative_radius =
      controller->search_origin_hz -
      controller->search_lower_hz;

  if (negative_radius > maximum_radius) {
    maximum_radius = negative_radius;
  }
  while (controller->undirected_attempt < UINT8_MAX) {
    controller->undirected_attempt++;
    multiplier =
        (uint32_t)((controller->undirected_attempt + 1U) / 2U);
    radius = controller->search_step_hz * multiplier;
    if (radius > maximum_radius) {
      return 0UL;
    }
    positive = (controller->undirected_attempt & 1U) != 0U;
    if (positive) {
      if (radius <=
          controller->search_upper_hz -
              controller->search_origin_hz) {
        return controller->search_origin_hz + radius;
      }
    } else if (radius <=
               controller->search_origin_hz -
                   controller->search_lower_hz) {
      return controller->search_origin_hz - radius;
    }
  }
  return 0UL;
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

static void Task5_RestartCoarseRecognition(
    task5_controller_t *controller,
    uint32_t now_ms)
{
  task5_lock_mode_t mode = controller->status.mode;
  uint32_t saw_frequency_hz =
      controller->status.saw_frequency_hz;

  /*
   * Compatibility with an older OpenMV image that may still return
   * NO_TRACE/TIMEOUT: acknowledge that result, close its Session, and start
   * a fresh one instead of entering TASK5_ERROR_BAD_RESULT.
   */
  if (!Task5_Start(
          controller, mode, saw_frequency_hz, now_ms) &&
      (controller->status.state != TASK5_STATE_ERROR)) {
    Task5_EnterError(controller, TASK5_ERROR_BAD_RESULT);
  }
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
          now_ms + TASK5_COARSE_REPLAY_MS;
      Task5_SetState(
          controller, TASK5_STATE_WAIT_COARSE_RESULT);
      break;
    case TASK5_STATE_WAIT_DDS_ACK:
      controller->state_deadline =
          now_ms + TASK5_DDS_RESULT_TIMEOUT_MS;
      Task5_SetState(
          controller, TASK5_STATE_WAIT_DDS_RESULT);
      break;
    case TASK5_STATE_WAIT_VISUAL_LOCK_ACK:
      controller->state_deadline =
          now_ms + TASK5_VISUAL_SAMPLE_TIMEOUT_MS;
      Task5_SetState(controller, TASK5_STATE_VISUAL_LOCKING);
      break;
    case TASK5_STATE_WAIT_STOP_ACK:
      (void)Task5_StartLocalPhaseLock(controller);
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
  if (!Task5_FinishStopWithoutAck(controller)) {
    Task5_EnterError(controller, TASK5_ERROR_NACK);
  }
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
    Task5_RestartCoarseRecognition(controller, now_ms);
    return;
  }

  estimated_input =
      ((uint64_t)controller->status.saw_frequency_hz *
       ratio_x1000 + 500ULL) /
      1000ULL;
  /* Always acquire the camera-assisted 1:1 line first. The requested 1x/2x
   * FTW and target POW are applied only when handing off to the local loop. */
  estimated = estimated_input;
  if ((estimated_input == 0ULL) ||
      (estimated_input > TASK5_MAX_INPUT_FREQUENCY_HZ) ||
      (estimated > TASK5_MAX_DDS_FREQUENCY_HZ)) {
    Task5_SendNack(
        controller, frame, OPENMV_NACK_OUT_OF_RANGE);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    Task5_RestartCoarseRecognition(controller, now_ms);
    return;
  }

  Task5_SendAck(controller, frame, TASK5_ACK_STATUS_OK);
  Task5_RememberResult(controller, frame, session, 0U);
  controller->status.last_openmv_result = result;
  controller->status.estimated_input_frequency_hz =
      (uint32_t)estimated_input;
  controller->high_frequency_search =
      estimated >= TASK5_HIGH_SEARCH_THRESHOLD_HZ;
  controller->status.last_match_score = 0U;
  controller->status.last_match_confidence = 0U;
  Task5_ResetSearchWindow(
      controller, (uint32_t)estimated,
      controller->high_frequency_search
          ? TASK5_HIGH_COARSE_STEP_HZ
          : TASK5_FINE_SEARCH_STEP_HZ,
      controller->high_frequency_search
          ? TASK5_SEARCH_STAGE_COARSE_1KHZ
          : TASK5_SEARCH_STAGE_FINE_100HZ);
  if (controller->port.stop_saw != NULL) {
    controller->port.stop_saw(controller->port.context);
  }
  (void)Task5_SetDDSAndSettle(
      controller, controller->search_origin_hz, now_ms);
}

static void Task5_BeginVisualLock(
    task5_controller_t *controller,
    uint32_t now_ms)
{
  uint8_t payload[7];
  uint32_t seed_millihz = controller->status.dds_frequency_hz * 1000UL;

  /* Visual samples carry the OpenMV monotonic clock, which has a different
   * epoch from HAL_GetTick(). Seed the visual controller in that clock domain;
   * every later settle deadline is based on a received sample timestamp. */
  VisualLock_Init(&controller->visual_lock, seed_millihz, 0UL);
  controller->visual_sample_valid = false;
  controller->status.visual_frequency_millihz = seed_millihz;
  controller->status.visual_phase_offset_mdeg = 0L;
  controller->status.visual_phase_mdeg = 0UL;
  controller->status.visual_speed_millihz = 0U;
  controller->status.visual_quality = 0U;
  controller->status.visual_frequency_direction = 0;
  OpenMV_WriteU16LE(&payload[0], controller->status.session_id);
  OpenMV_WriteU32LE(&payload[2], seed_millihz);
  payload[6] = 0x00U; /* visual target: 1x, positive-slope line */
  Task5_BeginReliable(
      controller, OPENMV_MSG_VISUAL_LOCK_START,
      payload, sizeof(payload),
      TASK5_STATE_WAIT_VISUAL_LOCK_ACK);
  Task5_ServicePending(controller, now_ms);
}

static void Task5_StopVisualLock(
    task5_controller_t *controller,
    uint32_t now_ms)
{
  uint8_t payload[3];

  OpenMV_WriteU16LE(&payload[0], controller->status.session_id);
  payload[2] = 0x00U;
  Task5_BeginReliable(
      controller, OPENMV_MSG_STOP_TASK,
      payload, sizeof(payload), TASK5_STATE_WAIT_STOP_ACK);
  Task5_ServicePending(controller, now_ms);
}

static void Task5_HandleDDSTestResult(
    task5_controller_t *controller,
    const openmv_frame_t *frame,
    uint32_t now_ms)
{
  uint16_t session;
  uint16_t test;
  uint8_t result;
  uint16_t match_score;
  uint8_t confidence;
  uint16_t quality;
  uint16_t distance;
  uint32_t next_frequency;

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
  match_score = OpenMV_ReadU16LE(&frame->payload[5]);
  confidence = frame->payload[7];
  if ((result > TASK5_DDS_TIMEOUT) ||
      (match_score > 1000U) ||
      (confidence > 100U)) {
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
  controller->status.last_match_score = match_score;
  controller->status.last_match_confidence = confidence;
  Task5_Touch(controller);

  if ((result == TASK5_DDS_TARGET_REACHED) &&
      !controller->high_frequency_search) {
    Task5_BeginVisualLock(controller, now_ms);
    return;
  }

  if (result <= TASK5_DDS_LOW_CONFIDENCE) {
    controller->image_retry_count = 0U;
    /* High-frequency figures jitter too quickly for frame-to-frame stability
     * confidence to be a reliable ranking signal.  Rank that segmented scan
     * by ellipse-family geometry alone; retain the conservative combined
     * quality for the low-frequency path. */
    quality = controller->high_frequency_search
                  ? match_score
                  : Task5_ResultQuality(match_score, confidence);
    distance = (uint16_t)(1000U - quality);

    if (controller->high_frequency_search) {
      if (controller->status.search_stage ==
          TASK5_SEARCH_STAGE_COARSE_1KHZ) {
        Task5_RecordDirectionalSample(controller, distance);
        next_frequency =
            Task5_NextHighCoarseFrequency(controller);
        if (next_frequency != 0UL) {
          (void)Task5_SetDDSAndSettle(
              controller, next_frequency, now_ms);
        } else {
          (void)Task5_BeginFineSearch(controller, now_ms);
        }
        return;
      }

      if (controller->status.search_stage ==
          TASK5_SEARCH_STAGE_FINE_100HZ) {
        Task5_RecordFineSample(controller, distance);
        next_frequency = Task5_NextFineFrequency(controller);
        if (next_frequency != 0UL) {
          (void)Task5_SetDDSAndSettle(
              controller, next_frequency, now_ms);
        } else {
          /* High-frequency Lissajous figures can jitter even when the DDS
           * frequency is correct.  The complete 100 Hz scan already gives
           * the required evidence: select its most ellipse-like sample and
           * hold it without demanding another consecutive camera result. */
          if (controller->fine_best_frequency_hz == 0UL) {
            (void)Task5_RestartHighSearch(
                controller,
                controller->status.dds_frequency_hz,
                now_ms);
          } else if (Task5_SetDDSAndSettle(
                         controller,
                         controller->fine_best_frequency_hz,
                         now_ms)) {
            Task5_BeginVisualLock(controller, now_ms);
          }
        }
        return;
      }
    }

    if (controller->status.search_stage ==
        TASK5_SEARCH_STAGE_FINE_100HZ) {
      Task5_RecordDirectionalSample(controller, distance);
      next_frequency = Task5_NextLowFrequency(controller);
      if (next_frequency != 0UL) {
        (void)Task5_SetDDSAndSettle(
            controller, next_frequency, now_ms);
      } else {
        (void)Task5_BeginLowConfirmation(
            controller, now_ms);
      }
      return;
    }

    if (controller->status.search_stage ==
        TASK5_SEARCH_STAGE_CONFIRM) {
      Task5_UpdateBestQuality(
          controller,
          controller->status.dds_frequency_hz,
          distance);
      if (controller->confirmation_peak_valid &&
          (controller->fine_best_distance <=
           (uint16_t)(1000U -
                      TASK5_SUSPICION_MIN_QUALITY)) &&
          (quality >= TASK5_CONFIRM_MIN_QUALITY)) {
        Task5_BeginVisualLock(controller, now_ms);
      } else {
        (void)Task5_RestartLowSearch(
            controller,
            controller->fine_best_frequency_hz,
            now_ms);
      }
      return;
    }

    (void)Task5_RestartLowSearch(
        controller, controller->status.dds_frequency_hz,
        now_ms);
    return;
  }

  /* IMAGE_ERROR/TIMEOUT are acquisition failures, not evidence that the
   * candidate frequency is wrong.  Keep the DDS output untouched and let
   * OpenMV rebuild its ROI on every non-blocking retry. */
  if (controller->image_retry_count < UINT8_MAX) {
    controller->image_retry_count++;
  }
  controller->status.image_recovery_count++;
  controller->state_deadline =
      now_ms + TASK5_DDS_SETTLE_MS;
  Task5_SetState(controller, TASK5_STATE_DDS_SETTLING);
  Task5_Touch(controller);
}

static void Task5_HandleVisualLockSample(
    task5_controller_t *controller,
    const openmv_frame_t *frame,
    uint32_t now_ms)
{
  visual_lock_sample_t sample;
  visual_lock_output_t output;
  uint16_t session;
  uint16_t sample_id;

  if (frame->length != 16U) {
    Task5_SendNack(controller, frame, OPENMV_NACK_BAD_LENGTH);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }
  session = OpenMV_ReadU16LE(&frame->payload[0]);
  sample_id = OpenMV_ReadU16LE(&frame->payload[2]);
  if (session != controller->status.session_id) {
    Task5_SendNack(controller, frame, OPENMV_NACK_BAD_SESSION);
    controller->status.stale_result_count++;
    Task5_Touch(controller);
    return;
  }
  if (controller->status.state != TASK5_STATE_VISUAL_LOCKING) {
    Task5_SendNack(controller, frame, OPENMV_NACK_BAD_STATE);
    return;
  }
  if (controller->visual_sample_valid &&
      (sample_id == controller->last_visual_sample_id)) {
    controller->status.duplicate_result_count++;
    Task5_Touch(controller);
    return;
  }

  sample.timestamp_ms = OpenMV_ReadU32LE(&frame->payload[4]);
  sample.phase_mdeg = OpenMV_ReadU32LE(&frame->payload[8]);
  sample.speed_millihz = OpenMV_ReadU16LE(&frame->payload[12]);
  sample.quality = frame->payload[14];
  sample.flags = frame->payload[15];
  if ((sample.phase_mdeg > 180000UL) ||
      (sample.speed_millihz > 10000U) ||
      (sample.quality > 100U) ||
      ((sample.flags & (uint8_t)~(
          VISUAL_LOCK_SAMPLE_PHASE_VALID |
          VISUAL_LOCK_SAMPLE_FAMILY_VALID)) != 0U)) {
    Task5_SendNack(controller, frame, OPENMV_NACK_OUT_OF_RANGE);
    controller->status.invalid_frame_count++;
    Task5_Touch(controller);
    return;
  }

  controller->visual_sample_valid = true;
  controller->last_visual_sample_id = sample_id;
  controller->state_deadline = now_ms + TASK5_VISUAL_SAMPLE_TIMEOUT_MS;
  controller->status.visual_phase_mdeg = sample.phase_mdeg;
  controller->status.visual_speed_millihz = sample.speed_millihz;
  controller->status.visual_quality = sample.quality;
  VisualLock_Step(&controller->visual_lock, &sample, &output);
  controller->status.visual_frequency_millihz = output.frequency_millihz;
  controller->status.visual_phase_offset_mdeg = output.phase_offset_mdeg;
  controller->status.visual_frequency_direction =
      (int8_t)controller->visual_lock.frequency_direction;
  controller->status.visual_direction_reversals =
      controller->visual_lock.direction_reversal_count;
  controller->status.visual_boundary_reversals =
      controller->visual_lock.boundary_reversal_count;
  Task5_Touch(controller);

  if (output.error) {
    Task5_EnterError(controller, TASK5_ERROR_SEARCH_LIMIT);
    return;
  }
  if (output.command_changed &&
      ((controller->port.set_dds_tone == NULL) ||
       !controller->port.set_dds_tone(
           controller->port.context,
           output.frequency_millihz,
           output.phase_offset_mdeg))) {
    Task5_EnterError(controller, TASK5_ERROR_DDS_OUTPUT);
    return;
  }
  if (output.locked) {
    Task5_StopVisualLock(controller, now_ms);
  }
}

bool Task5_Init(task5_controller_t *controller,
                const task5_port_t *port)
{
  if ((controller == NULL) || (port == NULL) ||
      (port->start_saw == NULL) ||
      (port->stop_saw == NULL) ||
      (port->set_dds_frequency == NULL) ||
      (port->set_dds_tone == NULL) ||
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
              now_ms, controller->state_deadline)) {
        Task5_KeepWaitingForCoarseResult(controller, now_ms);
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
    case TASK5_STATE_VISUAL_LOCKING:
      if (Task5_DeadlineReached(now_ms, controller->state_deadline)) {
        Task5_EnterError(
            controller, TASK5_ERROR_IMAGE_RECOGNITION);
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
    case OPENMV_MSG_VISUAL_LOCK_SAMPLE:
      Task5_HandleVisualLockSample(controller, frame, now_ms);
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

void Task5_NotifyDDSError(task5_controller_t *controller)
{
  if ((controller == NULL) ||
      (controller->status.state == TASK5_STATE_INACTIVE) ||
      (controller->status.state == TASK5_STATE_WAIT_SELECTION)) {
    return;
  }
  Task5_EnterError(controller, TASK5_ERROR_DDS_OUTPUT);
}

void Task5_GetStatus(const task5_controller_t *controller,
                     task5_status_t *status)
{
  if ((controller == NULL) || (status == NULL)) {
    return;
  }
  *status = controller->status;
}
