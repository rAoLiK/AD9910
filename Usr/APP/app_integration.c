#include "app_integration.h"

#include "app_board.h"
#include "app_hmi_map.h"
#include "app_saw.h"
#include "openmv_uart.h"
#include "pll_demo.h"
#include "task5_controller.h"
#include "tjc_ctrl.h"
#include "tjc_usart_hmi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define APP_COMMAND_QUEUE_SIZE                 (16U)
#define APP_COMMAND_QUEUE_MASK                 (APP_COMMAND_QUEUE_SIZE - 1U)
#define APP_BUTTON_DEBOUNCE_MS                 (50UL)
#define APP_BUTTON_LONG_PRESS_MS               (400UL)
#define APP_SCOPE_VOLTS_PER_DIV                (0.5f)
#define APP_DDS_FULL_SCALE_VPP_AFTER_GAIN      (4.432f)
#define APP_FEEDBACK_INVERSION_DEG             (180.0f)
#define APP_TASK1_OUTPUT_PHASE_DEG             (0.0f)
#define APP_TASK2_OUTPUT_PHASE_DEG             (90.0f)
#define APP_TASK3_GENERALIZED_PHASE_DEG        (0.0f)

typedef struct {
  app_core_t core;
  task5_controller_t task5;
  ad9910_t *dds;
  app_command_t command_queue[APP_COMMAND_QUEUE_SIZE];
  uint8_t command_head;
  uint8_t command_tail;
  uint32_t command_queue_overflow_count;
  uint32_t hmi_execute_error_count;
  uint32_t hmi_unknown_message_count;
  app_state_t rendered_state;
  app_waveform_t rendered_task5_waveform;
  uint32_t rendered_task5_revision;
  uint32_t last_button_tick[3];
  bool last_button_tick_valid[3];
  uint32_t button_press_tick;
  uint32_t button_release_tick;
  uint8_t active_button_index;
  bool button_press_pending;
  bool button_release_pending;
  bool render_forced;
  bool initialized;
} app_integration_context_t;

static app_integration_context_t s_app;

static float AppIntegration_WrapDegrees(float degrees)
{
  while (degrees > 180.0f) {
    degrees -= 360.0f;
  }
  while (degrees <= -180.0f) {
    degrees += 360.0f;
  }
  return degrees;
}

static bool AppIntegration_LockParameters(app_waveform_t waveform,
                                          uint8_t *multiplier,
                                          float *feedback_phase_deg)
{
  float output_phase_deg;

  if ((multiplier == NULL) || (feedback_phase_deg == NULL)) {
    return false;
  }

  switch (waveform) {
    case APP_WAVEFORM_TASK1_LINE:
      *multiplier = 1U;
      output_phase_deg = APP_TASK1_OUTPUT_PHASE_DEG;
      break;
    case APP_WAVEFORM_TASK2_CIRCLE:
      *multiplier = 1U;
      output_phase_deg = APP_TASK2_OUTPUT_PHASE_DEG;
      break;
    case APP_WAVEFORM_TASK3_INFINITY:
      *multiplier = 2U;
      output_phase_deg = APP_TASK3_GENERALIZED_PHASE_DEG;
      break;
    case APP_WAVEFORM_NONE:
    default:
      return false;
  }

  *feedback_phase_deg = AppIntegration_WrapDegrees(
      output_phase_deg + APP_FEEDBACK_INVERSION_DEG);
  return true;
}

static float AppIntegration_AmplitudeScale(uint8_t amplitude_div)
{
  float requested_vpp =
      (float)amplitude_div * APP_SCOPE_VOLTS_PER_DIV;
  float scale = requested_vpp / APP_DDS_FULL_SCALE_VPP_AFTER_GAIN;

  if (scale < 0.0f) {
    return 0.0f;
  }
  if (scale > 1.0f) {
    return 1.0f;
  }
  return scale;
}

static bool AppIntegration_EnterSafe(void *context)
{
  (void)context;
  AppSaw_Stop();
  AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
  return PLL_Demo_Stop() == HAL_OK;
}

static bool AppIntegration_RunDirect(void *context)
{
  (void)context;
  if (PLL_Demo_Stop() != HAL_OK) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
    return false;
  }
  AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
  return true;
}

static bool AppIntegration_RunLock(void *context,
                                   app_waveform_t waveform,
                                   uint8_t amplitude_div)
{
  uint8_t multiplier;
  float feedback_phase_deg;
  float scale;

  (void)context;
  if (!AppIntegration_LockParameters(
          waveform, &multiplier, &feedback_phase_deg)) {
    return false;
  }
  scale = AppIntegration_AmplitudeScale(amplitude_div);

  if ((PLL_Demo_Stop() != HAL_OK) ||
      (PLL_Demo_Configure(multiplier,
                          feedback_phase_deg,
                          scale) != HAL_OK)) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
    return false;
  }

  AppBoard_SetPath(APP_BOARD_PATH_DDS);
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
  if (PLL_Demo_Start() != HAL_OK) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
    (void)PLL_Demo_Stop();
    return false;
  }
  return true;
}

static bool AppIntegration_SetLockAmplitude(void *context,
                                            app_waveform_t waveform,
                                            uint8_t amplitude_div)
{
  uint8_t multiplier;
  float feedback_phase_deg;

  (void)context;
  if (!AppIntegration_LockParameters(
          waveform, &multiplier, &feedback_phase_deg)) {
    return false;
  }
  if (PLL_Demo_Configure(
          multiplier,
          feedback_phase_deg,
          AppIntegration_AmplitudeScale(amplitude_div)) != HAL_OK) {
    return false;
  }
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
  return true;
}

static void AppIntegration_EnableButtons(void *context, bool enable)
{
  (void)context;
  AppBoard_EnableTask5Buttons(enable);
}

static bool AppIntegration_Task5StartSaw(
    void *context, uint32_t frequency_hz)
{
  (void)context;
  (void)PLL_Demo_Stop();
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DAC);
  return AppSaw_Start(frequency_hz) == HAL_OK;
}

static void AppIntegration_Task5StopSaw(void *context)
{
  (void)context;
  AppSaw_Stop();
}

static bool AppIntegration_Task5SetDDS(
    void *context, uint32_t frequency_hz)
{
  app_integration_context_t *app =
      (app_integration_context_t *)context;

  if ((app == NULL) || (app->dds == NULL) ||
      (PLL_Demo_Stop() != HAL_OK)) {
    return false;
  }
  if (AD9910_SetSingleToneHz(
          app->dds, (double)frequency_hz,
          0.0, 1.0, 1U) != AD9910_STATUS_OK) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    return false;
  }
  AppBoard_SetPath(APP_BOARD_PATH_DDS);
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
  return true;
}

static bool AppIntegration_Task5StartPhaseLock(
    void *context,
    task5_lock_mode_t mode,
    uint32_t seed_frequency_hz)
{
  app_integration_context_t *app =
      (app_integration_context_t *)context;
  app_waveform_t waveform;
  uint8_t multiplier;
  float feedback_phase_deg;

  if (app == NULL) {
    return false;
  }
  waveform =
      (mode == TASK5_MODE_LINE_0_DEG)
          ? APP_WAVEFORM_TASK1_LINE
          : (mode == TASK5_MODE_CIRCLE_NEG_90_DEG)
                ? APP_WAVEFORM_TASK2_CIRCLE
                : APP_WAVEFORM_TASK3_INFINITY;
  if (!AppIntegration_LockParameters(
          waveform, &multiplier, &feedback_phase_deg) ||
      (PLL_Demo_Stop() != HAL_OK) ||
      (PLL_Demo_SeedFrequency(
           (float)seed_frequency_hz) != HAL_OK) ||
      (PLL_Demo_Configure(
           multiplier, feedback_phase_deg,
           AppIntegration_AmplitudeScale(8U)) != HAL_OK)) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
    return false;
  }

  AppBoard_SetPath(APP_BOARD_PATH_DDS);
  AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
  if (PLL_Demo_Start() != HAL_OK) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    AppBoard_SetSignalSource(APP_BOARD_SIGNAL_DDS);
    (void)PLL_Demo_Stop();
    return false;
  }
  return true;
}

static void AppIntegration_Task5SafeOutputs(void *context)
{
  (void)AppIntegration_EnterSafe(context);
}

static bool AppIntegration_Task5SendFrame(
    void *context,
    uint8_t type,
    uint8_t seq,
    const uint8_t *payload,
    uint16_t payload_length)
{
  (void)context;
  return OpenMV_UART_Send(
      type, seq, payload, payload_length);
}

static void AppIntegration_OpenMVFrame(
    const openmv_frame_t *frame,
    void *context)
{
  app_integration_context_t *app =
      (app_integration_context_t *)context;

  if (app == NULL) {
    return;
  }
  Task5_OnFrame(&app->task5, frame, HAL_GetTick());
}

static bool AppIntegration_QueueCommand(app_command_t command)
{
  uint8_t next =
      (uint8_t)((s_app.command_head + 1U) & APP_COMMAND_QUEUE_MASK);

  if (next == s_app.command_tail) {
    s_app.command_queue_overflow_count++;
    return false;
  }
  s_app.command_queue[s_app.command_head] = command;
  s_app.command_head = next;
  return true;
}

static bool AppIntegration_PopCommand(app_command_t *command)
{
  if ((command == NULL) ||
      (s_app.command_tail == s_app.command_head)) {
    return false;
  }
  *command = s_app.command_queue[s_app.command_tail];
  s_app.command_tail =
      (uint8_t)((s_app.command_tail + 1U) & APP_COMMAND_QUEUE_MASK);
  return true;
}

static void AppIntegration_HmiMessage(
    const TJC_ProtocolMessage_t *message)
{
  app_command_t command;

  if (message == NULL) {
    return;
  }

  switch (message->type) {
    case TJC_PROTOCOL_MSG_TOUCH_EVENT:
      if (AppHmi_MapTouch(
              message->payload.touchEvent.pageId,
              message->payload.touchEvent.componentId,
              message->payload.touchEvent.touchEvent,
              &command)) {
        (void)AppIntegration_QueueCommand(command);
      }
      break;
    case TJC_PROTOCOL_MSG_EXECUTE_RESULT:
      if (message->payload.executeResult.isError) {
        s_app.hmi_execute_error_count++;
      }
      break;
    case TJC_PROTOCOL_MSG_PAGE_ID:
      /*
       * This is a confirmation from the screen. Do not answer it with
       * another page command: a screen that reports every page change would
       * otherwise enter a confirmation loop.
       */
      break;
    case TJC_PROTOCOL_MSG_STARTUP:
    case TJC_PROTOCOL_MSG_AUTO_WAKE:
      /* Re-render current application truth; never restart its workflow. */
      s_app.render_forced = true;
      break;
    case TJC_PROTOCOL_MSG_UNKNOWN:
    case TJC_PROTOCOL_MSG_BUFFER_OVERFLOW:
      s_app.hmi_unknown_message_count++;
      break;
    default:
      break;
  }
}

static app_command_t AppIntegration_Task5CommandForButton(
    uint8_t button_index)
{
  return (button_index == 0U)
             ? APP_COMMAND_TASK5_LINE
             : (button_index == 1U)
                   ? APP_COMMAND_TASK5_CIRCLE
                   : APP_COMMAND_TASK5_INFINITY;
}

static task5_lock_mode_t AppIntegration_Task5ModeForButton(
    uint8_t button_index)
{
  return (button_index == 0U)
             ? TASK5_MODE_LINE_0_DEG
             : (button_index == 1U)
                   ? TASK5_MODE_CIRCLE_NEG_90_DEG
                   : TASK5_MODE_INFINITY_2X_0_DEG;
}

static void AppIntegration_StartTask5Selection(
    uint8_t button_index,
    uint32_t saw_frequency_hz,
    uint32_t now)
{
  task5_status_t task5;

  if (!AppCore_HandleCommand(
          &s_app.core,
          AppIntegration_Task5CommandForButton(
              button_index))) {
    AppCore_ReportError(
        &s_app.core, APP_ERROR_TASK5_RUNTIME);
  } else if (!Task5_Start(
                 &s_app.task5,
                 AppIntegration_Task5ModeForButton(button_index),
                 saw_frequency_hz, now)) {
    Task5_GetStatus(&s_app.task5, &task5);
    if (task5.state != TASK5_STATE_ERROR) {
      AppCore_ReportError(
          &s_app.core, APP_ERROR_TASK5_RUNTIME);
    }
  }
  s_app.button_press_pending = false;
  s_app.button_release_pending = false;
}

static void AppIntegration_HandleButtonPress(
    uint8_t button_index,
    uint32_t now)
{
  task5_status_t task5;

  if ((button_index >= 3U) ||
      (s_app.last_button_tick_valid[button_index] &&
       ((uint32_t)(now -
                   s_app.last_button_tick[button_index]) <
        APP_BUTTON_DEBOUNCE_MS))) {
    return;
  }
  s_app.last_button_tick[button_index] = now;
  s_app.last_button_tick_valid[button_index] = true;

  Task5_GetStatus(&s_app.task5, &task5);
  if (task5.state == TASK5_STATE_INACTIVE) {
    return;
  }

  if (s_app.button_press_pending) {
    return;
  }

  s_app.active_button_index = button_index;
  s_app.button_press_tick = now;
  s_app.button_press_pending = true;
  s_app.button_release_pending = false;
}

static void AppIntegration_ProcessButtons(void)
{
  app_board_button_counts_t counts;
  uint32_t now = HAL_GetTick();

  AppBoard_TakeButtonCounts(&counts);
  while (counts.line != 0U) {
    counts.line--;
    AppIntegration_HandleButtonPress(0U, now);
  }
  while (counts.circle != 0U) {
    counts.circle--;
    AppIntegration_HandleButtonPress(1U, now);
  }
  while (counts.infinity != 0U) {
    counts.infinity--;
    AppIntegration_HandleButtonPress(2U, now);
  }

  if (!s_app.button_press_pending) {
    return;
  }

  if (AppBoard_IsTask5ButtonPressed(
          s_app.active_button_index)) {
    s_app.button_release_pending = false;
    if ((uint32_t)(now - s_app.button_press_tick) >=
        APP_BUTTON_LONG_PRESS_MS) {
      AppIntegration_StartTask5Selection(
          s_app.active_button_index,
          APP_SAW_FREQUENCY_10KHZ, now);
    }
    return;
  }

  if (!s_app.button_release_pending) {
    s_app.button_release_tick = now;
    s_app.button_release_pending = true;
    return;
  }

  if ((uint32_t)(now - s_app.button_release_tick) <
      APP_BUTTON_DEBOUNCE_MS) {
    return;
  }

  AppIntegration_StartTask5Selection(
      s_app.active_button_index,
      ((uint32_t)(s_app.button_release_tick -
                  s_app.button_press_tick) >=
       APP_BUTTON_LONG_PRESS_MS)
          ? APP_SAW_FREQUENCY_10KHZ
          : APP_SAW_FREQUENCY_1KHZ,
      now);
}

static void AppIntegration_UpdateLockActivity(void)
{
  const pll_demo_status_t *pll = PLL_Demo_GetStatus();
  task5_status_t task5;

  if (pll == NULL) {
    return;
  }
  Task5_GetStatus(&s_app.task5, &task5);
  if ((task5.state == TASK5_STATE_PHASE_LOCKING) ||
      (task5.state == TASK5_STATE_LOCKED)) {
    Task5_NotifyPhaseLock(
        &s_app.task5,
        pll->state == PLL_DEMO_LOCKED,
        pll->state == PLL_DEMO_ERROR);
  }
  switch (pll->state) {
    case PLL_DEMO_SEARCHING:
      AppCore_SetLockActivity(&s_app.core, APP_ACTIVITY_LOCK_SEARCH);
      break;
    case PLL_DEMO_ACQUIRING:
      AppCore_SetLockActivity(&s_app.core, APP_ACTIVITY_LOCK_ACQUIRE);
      break;
    case PLL_DEMO_LOCKED:
      AppCore_SetLockActivity(&s_app.core, APP_ACTIVITY_LOCKED);
      break;
    case PLL_DEMO_ERROR:
      AppCore_ReportError(&s_app.core, APP_ERROR_LOCK_RUNTIME);
      break;
    case PLL_DEMO_STOPPED:
    default:
      break;
  }
}

static void AppIntegration_UpdateTask5Activity(void)
{
  task5_status_t task5;
  app_activity_t activity;

  Task5_GetStatus(&s_app.task5, &task5);
  switch (task5.state) {
    case TASK5_STATE_WAIT_SELECTION:
      activity = APP_ACTIVITY_TASK5_WAIT_SELECTION;
      break;
    case TASK5_STATE_WAIT_START_ACK:
    case TASK5_STATE_WAIT_COARSE_RESULT:
      activity = APP_ACTIVITY_TASK5_COMMUNICATING;
      break;
    case TASK5_STATE_DDS_SETTLING:
    case TASK5_STATE_WAIT_DDS_ACK:
    case TASK5_STATE_WAIT_DDS_RESULT:
    case TASK5_STATE_WAIT_STOP_ACK:
      activity = APP_ACTIVITY_TASK5_SEARCHING;
      break;
    case TASK5_STATE_PHASE_LOCKING:
      activity = APP_ACTIVITY_TASK5_LOCKING;
      break;
    case TASK5_STATE_LOCKED:
      activity = APP_ACTIVITY_TASK5_LOCKED;
      break;
    case TASK5_STATE_ERROR:
      activity = APP_ACTIVITY_ERROR;
      break;
    case TASK5_STATE_INACTIVE:
    default:
      return;
  }
  AppCore_SetTask5Activity(&s_app.core, activity);
}

static const char *AppIntegration_Task5ModeText(
    task5_lock_mode_t mode)
{
  return (mode == TASK5_MODE_LINE_0_DEG)
             ? "line"
             : (mode == TASK5_MODE_CIRCLE_NEG_90_DEG)
                   ? "circle"
                   : "infinity";
}

static void AppIntegration_Task5ErrorText(
    const task5_status_t *task5,
    char *reason,
    size_t reason_size)
{
  if ((task5 == NULL) || (reason == NULL) ||
      (reason_size == 0U)) {
    return;
  }

  switch (task5->last_error) {
    case TASK5_ERROR_BAD_ARGUMENT:
      (void)snprintf(reason, reason_size, "bad argument");
      break;
    case TASK5_ERROR_SAW_START:
      (void)snprintf(reason, reason_size, "DAC start failed");
      break;
    case TASK5_ERROR_UART_SEND:
      (void)snprintf(reason, reason_size, "UART5 TX failed");
      break;
    case TASK5_ERROR_ACK_TIMEOUT:
      (void)snprintf(
          reason, reason_size, "%s ACK timeout",
          (task5->error_origin_state ==
           TASK5_STATE_WAIT_START_ACK)
              ? "START"
              : (task5->error_origin_state ==
                 TASK5_STATE_WAIT_DDS_ACK)
                    ? "DDS"
                    : (task5->error_origin_state ==
                       TASK5_STATE_WAIT_STOP_ACK)
                          ? "STOP"
                          : "OpenMV");
      break;
    case TASK5_ERROR_RESULT_TIMEOUT:
      (void)snprintf(
          reason, reason_size, "%s result timeout",
          (task5->error_origin_state ==
           TASK5_STATE_WAIT_COARSE_RESULT)
              ? "COARSE"
              : (task5->error_origin_state ==
                 TASK5_STATE_WAIT_DDS_RESULT)
                    ? "DDS"
                    : "OpenMV");
      break;
    case TASK5_ERROR_NACK:
      (void)snprintf(
          reason, reason_size, "OpenMV NACK 0x%02X",
          (unsigned int)task5->last_nack_code);
      break;
    case TASK5_ERROR_BAD_RESULT:
      (void)snprintf(
          reason, reason_size, "OpenMV bad result");
      break;
    case TASK5_ERROR_DDS_OUTPUT:
      (void)snprintf(reason, reason_size, "DDS output failed");
      break;
    case TASK5_ERROR_SEARCH_LIMIT:
      (void)snprintf(reason, reason_size, "DDS search limit");
      break;
    case TASK5_ERROR_PHASE_LOCK:
      (void)snprintf(reason, reason_size, "phase lock failed");
      break;
    case TASK5_ERROR_OPENMV_REPORTED:
      (void)snprintf(
          reason, reason_size, "OpenMV reported error");
      break;
    case TASK5_ERROR_NONE:
    default:
      (void)snprintf(
          reason, reason_size, "unknown error %u",
          (unsigned int)task5->last_error);
      break;
  }
}

static void AppIntegration_Task5Text(
    const task5_status_t *task5,
    char *text,
    size_t text_size)
{
  app_saw_status_t saw;
  openmv_uart_diagnostics_t uart;
  char reason[40];

  if ((task5 == NULL) || (text == NULL) ||
      (text_size == 0U)) {
    return;
  }

  switch (task5->state) {
    case TASK5_STATE_WAIT_SELECTION:
      (void)snprintf(
          text, text_size,
          "PA0: diagonal\rPB9: circle\rPB8: infinity");
      break;
    case TASK5_STATE_WAIT_START_ACK:
      (void)snprintf(
          text, text_size, "%s: wait camera ACK",
          AppIntegration_Task5ModeText(task5->mode));
      break;
    case TASK5_STATE_WAIT_COARSE_RESULT:
      (void)snprintf(
          text, text_size, "%s: coarse recognize",
          AppIntegration_Task5ModeText(task5->mode));
      break;
    case TASK5_STATE_DDS_SETTLING:
    case TASK5_STATE_WAIT_DDS_ACK:
    case TASK5_STATE_WAIT_DDS_RESULT:
      (void)snprintf(
          text, text_size, "DDS %luHz test %u stage %u",
          (unsigned long)task5->dds_frequency_hz,
          (unsigned int)task5->test_id,
          (unsigned int)task5->search_stage);
      break;
    case TASK5_STATE_WAIT_STOP_ACK:
      (void)snprintf(
          text, text_size, "frequency found: %luHz",
          (unsigned long)task5->dds_frequency_hz);
      break;
    case TASK5_STATE_PHASE_LOCKING:
      (void)snprintf(
          text, text_size, "phase locking at %luHz",
          (unsigned long)task5->dds_frequency_hz);
      break;
    case TASK5_STATE_LOCKED:
      (void)snprintf(
          text, text_size, "LOCKED %s %luHz",
          AppIntegration_Task5ModeText(task5->mode),
          (unsigned long)task5->dds_frequency_hz);
      break;
    case TASK5_STATE_ERROR:
      AppSaw_GetStatus(&saw);
      OpenMV_UART_GetDiagnostics(&uart);
      AppIntegration_Task5ErrorText(
          task5, reason, sizeof(reason));
      (void)snprintf(
          text, text_size,
          "ERR: %s\rOUTPUT %s\rRX%lu CRC%lu U%lu",
          reason,
          saw.running ? "ACTIVE" : "OFF",
          (unsigned long)uart.parser.valid_frame_count,
          (unsigned long)uart.parser.crc_error_count,
          (unsigned long)uart.uart_error_count);
      break;
    case TASK5_STATE_INACTIVE:
    default:
      text[0] = '\0';
      break;
  }
}

static void AppIntegration_Render(void)
{
  app_status_t status;
  task5_status_t task5;
  char task5_text[96];
  uint16_t page_id;
  HAL_StatusTypeDef result = HAL_OK;
  bool state_changed;
  bool task5_changed;

  AppCore_GetStatus(&s_app.core, &status);
  Task5_GetStatus(&s_app.task5, &task5);
  state_changed = status.state != s_app.rendered_state;
  task5_changed =
      (status.state == APP_STATE_TASK5) &&
      ((status.waveform != s_app.rendered_task5_waveform) ||
       (task5.revision != s_app.rendered_task5_revision));
  if (!s_app.render_forced && !state_changed &&
      !task5_changed) {
    return;
  }

  page_id = (status.state == APP_STATE_TASK14)
                ? 1U
                : (status.state == APP_STATE_TASK5)
                      ? 2U
                      : 0U;
  if (s_app.render_forced || state_changed) {
    result = TJC_PageSetById(page_id);
  }
  if ((result == HAL_OK) &&
      (status.state == APP_STATE_TASK5)) {
    AppIntegration_Task5Text(
        &task5, task5_text, sizeof(task5_text));
    result = TJC_TextSetText("t2", task5_text);
  }

  if (result == HAL_OK) {
    s_app.rendered_state = status.state;
    s_app.rendered_task5_waveform = status.waveform;
    s_app.rendered_task5_revision = task5.revision;
    s_app.render_forced = false;
  }
}

static void AppIntegration_ExecuteCommand(app_command_t command)
{
  app_status_t status;

  AppCore_GetStatus(&s_app.core, &status);
  if ((status.state == APP_STATE_TASK5) &&
      ((command == APP_COMMAND_GO_MENU) ||
       (command == APP_COMMAND_ENTER_TASK1) ||
       (command == APP_COMMAND_ENTER_TASK14))) {
    Task5_Exit(&s_app.task5, 0x01U);
    s_app.button_press_pending = false;
    s_app.button_release_pending = false;
  }

  if (command == APP_COMMAND_ENTER_TASK5) {
    if (AppCore_HandleCommand(&s_app.core, command)) {
      Task5_Enter(&s_app.task5);
      s_app.button_press_pending = false;
      s_app.button_release_pending = false;
    }
    return;
  }
  (void)AppCore_HandleCommand(&s_app.core, command);
}

HAL_StatusTypeDef AppIntegration_Init(ad9910_t *dds)
{
  app_port_t port = {
      .enter_safe = AppIntegration_EnterSafe,
      .run_direct = AppIntegration_RunDirect,
      .run_lock = AppIntegration_RunLock,
      .set_lock_amplitude = AppIntegration_SetLockAmplitude,
      .enable_task5_buttons = AppIntegration_EnableButtons,
      .context = NULL,
  };
  task5_port_t task5_port = {
      .start_saw = AppIntegration_Task5StartSaw,
      .stop_saw = AppIntegration_Task5StopSaw,
      .set_dds_frequency = AppIntegration_Task5SetDDS,
      .start_phase_lock = AppIntegration_Task5StartPhaseLock,
      .safe_outputs = AppIntegration_Task5SafeOutputs,
      .send_frame = AppIntegration_Task5SendFrame,
      .context = &s_app,
  };

  memset(&s_app, 0, sizeof(s_app));
  if (dds == NULL) {
    return HAL_ERROR;
  }
  s_app.dds = dds;
  s_app.render_forced = true;
  TJC_RegisterMessageHandler(AppIntegration_HmiMessage);
  if (TJC_Init() != HAL_OK) {
    return HAL_ERROR;
  }
  if ((AppSaw_Init() != HAL_OK) ||
      !Task5_Init(&s_app.task5, &task5_port) ||
      (OpenMV_UART_Init(
           AppIntegration_OpenMVFrame, &s_app) != HAL_OK)) {
    return HAL_ERROR;
  }
  if (!AppCore_Init(&s_app.core, &port)) {
    return HAL_ERROR;
  }

  s_app.initialized = true;
  AppIntegration_Render();
  return HAL_OK;
}

void AppIntegration_Process(void)
{
  app_command_t command;
  uint32_t processed = 0U;

  if (!s_app.initialized) {
    return;
  }

  TJC_Service();
  OpenMV_UART_Service();
  while ((processed < APP_COMMAND_QUEUE_SIZE) &&
         AppIntegration_PopCommand(&command)) {
    AppIntegration_ExecuteCommand(command);
    processed++;
  }
  AppIntegration_ProcessButtons();

  Task5_Process(&s_app.task5, HAL_GetTick());
  AppSaw_Process();
  PLL_Demo_Process();
  AppIntegration_UpdateLockActivity();
  AppIntegration_UpdateTask5Activity();
  AppIntegration_Render();
}

void AppIntegration_GetStatus(app_integration_status_t *status)
{
  if (status == NULL) {
    return;
  }
  AppCore_GetStatus(&s_app.core, &status->app);
  status->command_queue_overflow_count =
      s_app.command_queue_overflow_count;
  status->hmi_execute_error_count =
      s_app.hmi_execute_error_count;
  status->hmi_unknown_message_count =
      s_app.hmi_unknown_message_count;
  Task5_GetStatus(&s_app.task5, &status->task5);
  AppSaw_GetStatus(&status->saw);
  OpenMV_UART_GetDiagnostics(&status->openmv_uart);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  (void)OpenMV_UART_RxCpltCallback(huart);
  TJC_UART_RxCpltCallback(huart);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  (void)OpenMV_UART_TxCpltCallback(huart);
  TJC_UART_TxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  (void)OpenMV_UART_ErrorCallback(huart);
  TJC_UART_ErrorCallback(huart);
}
