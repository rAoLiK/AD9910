#ifndef TASK5_CONTROLLER_H
#define TASK5_CONTROLLER_H

#include "openmv_protocol.h"
#include "visual_lock.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TASK5_MODE_LINE_0_DEG = 0x00,
  TASK5_MODE_CIRCLE_NEG_90_DEG = 0x01,
  TASK5_MODE_INFINITY_2X_0_DEG = 0x02
} task5_lock_mode_t;

typedef enum {
  TASK5_STATE_INACTIVE = 0,
  TASK5_STATE_WAIT_SELECTION,
  TASK5_STATE_WAIT_START_ACK,
  TASK5_STATE_WAIT_COARSE_RESULT,
  TASK5_STATE_DDS_SETTLING,
  TASK5_STATE_WAIT_DDS_ACK,
  TASK5_STATE_WAIT_DDS_RESULT,
  TASK5_STATE_WAIT_VISUAL_LOCK_ACK,
  TASK5_STATE_VISUAL_LOCKING,
  TASK5_STATE_WAIT_STOP_ACK,
  TASK5_STATE_FREQUENCY_HOLD,
  TASK5_STATE_PHASE_LOCKING,
  TASK5_STATE_LOCKED,
  TASK5_STATE_ERROR
} task5_state_t;

typedef enum {
  TASK5_ERROR_NONE = 0,
  TASK5_ERROR_BAD_ARGUMENT,
  TASK5_ERROR_SAW_START,
  TASK5_ERROR_UART_SEND,
  TASK5_ERROR_ACK_TIMEOUT,
  TASK5_ERROR_RESULT_TIMEOUT,
  TASK5_ERROR_NACK,
  TASK5_ERROR_BAD_RESULT,
  TASK5_ERROR_DDS_OUTPUT,
  TASK5_ERROR_SEARCH_LIMIT,
  TASK5_ERROR_PHASE_LOCK,
  TASK5_ERROR_OPENMV_REPORTED,
  TASK5_ERROR_IMAGE_RECOGNITION
} task5_error_t;

typedef struct {
  bool (*start_saw)(void *context, uint32_t frequency_hz);
  void (*stop_saw)(void *context);
  bool (*set_dds_frequency)(void *context, uint32_t frequency_hz);
  bool (*set_dds_tone)(void *context,
                       uint32_t frequency_millihz,
                       int32_t phase_offset_mdeg);
  void (*safe_outputs)(void *context);
  bool (*send_frame)(void *context,
                     uint8_t type,
                     uint8_t seq,
                     const uint8_t *payload,
                     uint16_t payload_length);
  void *context;
} task5_port_t;

typedef struct {
  task5_state_t state;
  task5_state_t error_origin_state;
  task5_error_t last_error;
  task5_lock_mode_t mode;
  uint16_t session_id;
  uint16_t test_id;
  uint32_t saw_frequency_hz;
  uint32_t estimated_input_frequency_hz;
  uint32_t absolute_frequency_hz;
  uint32_t dds_frequency_hz;
  uint32_t visual_frequency_millihz;
  int32_t visual_phase_offset_mdeg;
  uint32_t visual_phase_mdeg;
  uint16_t visual_speed_millihz;
  uint8_t visual_quality;
  int8_t visual_frequency_direction;
  uint8_t visual_direction_reversals;
  uint8_t visual_boundary_reversals;
  uint8_t search_stage;
  uint8_t last_openmv_result;
  uint8_t last_nack_code;
  uint8_t search_count;
  uint8_t ack_retry_count;
  uint8_t result_retry_count;
  uint16_t last_match_score;
  uint8_t last_match_confidence;
  uint16_t best_match_quality;
  uint32_t best_match_frequency_hz;
  uint32_t image_recovery_count;
  uint32_t search_restart_count;
  uint32_t duplicate_result_count;
  uint32_t stale_result_count;
  uint32_t invalid_frame_count;
  uint32_t tx_queue_error_count;
  bool absolute_frequency_locked;
  bool lock_hold_acknowledged;
  uint32_t revision;
} task5_status_t;

typedef struct {
  bool active;
  bool sent;
  uint8_t type;
  uint8_t seq;
  uint8_t retry_count;
  uint16_t payload_length;
  uint8_t payload[OPENMV_PROTOCOL_MAX_PAYLOAD];
  uint32_t ack_deadline;
} task5_pending_tx_t;

typedef struct {
  bool seeded;
  bool complete;
  uint8_t sample_count;
  uint8_t rise_count;
  uint16_t previous_distance;
  uint16_t best_distance;
  uint32_t previous_frequency_hz;
  uint32_t best_frequency_hz;
  uint32_t best_pair_cost;
  uint32_t best_pair_lower_hz;
  uint32_t best_pair_upper_hz;
} task5_search_trend_t;

typedef struct {
  task5_port_t port;
  task5_status_t status;
  task5_pending_tx_t pending;
  uint8_t next_tx_seq;
  uint16_t next_session_id;
  uint32_t state_deadline;
  uint32_t search_origin_hz;
  uint32_t search_step_hz;
  uint32_t search_lower_hz;
  uint32_t search_upper_hz;
  visual_lock_controller_t visual_lock;
  uint16_t last_visual_sample_id;
  bool visual_sample_valid;
  uint8_t undirected_attempt;
  uint8_t image_retry_count;
  bool high_frequency_search;
  task5_search_trend_t coarse_trend[2];
  uint16_t fine_best_distance;
  uint16_t fine_worst_distance;
  uint32_t fine_best_frequency_hz;
  bool confirmation_peak_valid;
  bool last_result_valid;
  uint8_t last_result_type;
  uint8_t last_result_seq;
  uint16_t last_result_session;
  uint16_t last_result_test;
} task5_controller_t;

bool Task5_Init(task5_controller_t *controller,
                const task5_port_t *port);
void Task5_Enter(task5_controller_t *controller);
bool Task5_Start(task5_controller_t *controller,
                 task5_lock_mode_t mode,
                 uint32_t saw_frequency_hz,
                 uint32_t now_ms);
void Task5_Exit(task5_controller_t *controller,
                uint8_t stop_reason);
void Task5_Process(task5_controller_t *controller,
                   uint32_t now_ms);
void Task5_OnFrame(task5_controller_t *controller,
                   const openmv_frame_t *frame,
                   uint32_t now_ms);
void Task5_NotifyPhaseLock(task5_controller_t *controller,
                           bool locked,
                           bool error);
void Task5_NotifyDDSError(task5_controller_t *controller);
void Task5_GetStatus(const task5_controller_t *controller,
                     task5_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* TASK5_CONTROLLER_H */
