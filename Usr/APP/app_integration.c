#include "app_integration.h"

#include "app_board.h"
#include "app_hmi_map.h"
#include "pll_demo.h"
#include "tjc_ctrl.h"
#include "tjc_usart_hmi.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define APP_COMMAND_QUEUE_SIZE                 (16U)
#define APP_COMMAND_QUEUE_MASK                 (APP_COMMAND_QUEUE_SIZE - 1U)
#define APP_BUTTON_DEBOUNCE_MS                 (50UL)
#define APP_SCOPE_VOLTS_PER_DIV                (0.5f)
#define APP_DDS_FULL_SCALE_VPP_AFTER_GAIN      (4.2f)
#define APP_FEEDBACK_INVERSION_DEG             (180.0f)
#define APP_TASK1_OUTPUT_PHASE_DEG             (0.0f)
#define APP_TASK2_OUTPUT_PHASE_DEG             (90.0f)
#define APP_TASK3_GENERALIZED_PHASE_DEG        (0.0f)

typedef struct {
  app_core_t core;
  app_command_t command_queue[APP_COMMAND_QUEUE_SIZE];
  uint8_t command_head;
  uint8_t command_tail;
  uint32_t command_queue_overflow_count;
  uint32_t hmi_execute_error_count;
  uint32_t hmi_unknown_message_count;
  app_state_t rendered_state;
  app_waveform_t rendered_task5_waveform;
  uint32_t last_button_tick[3];
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
  AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
  return PLL_Demo_Stop() == HAL_OK;
}

static bool AppIntegration_RunDirect(void *context)
{
  (void)context;
  if (PLL_Demo_Stop() != HAL_OK) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
    return false;
  }
  AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
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
    return false;
  }

  AppBoard_SetPath(APP_BOARD_PATH_DDS);
  if (PLL_Demo_Start() != HAL_OK) {
    AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
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
  return PLL_Demo_Configure(
             multiplier,
             feedback_phase_deg,
             AppIntegration_AmplitudeScale(amplitude_div)) == HAL_OK;
}

static void AppIntegration_EnableButtons(void *context, bool enable)
{
  (void)context;
  AppBoard_EnableTask5Buttons(enable);
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

static void AppIntegration_ProcessButtons(void)
{
  uint32_t events = AppBoard_TakeButtonEvents();
  uint32_t now = HAL_GetTick();

  if (((events & APP_BUTTON_EVENT_LINE) != 0U) &&
      ((uint32_t)(now - s_app.last_button_tick[0]) >=
       APP_BUTTON_DEBOUNCE_MS)) {
    s_app.last_button_tick[0] = now;
    (void)AppIntegration_QueueCommand(APP_COMMAND_TASK5_LINE);
  }
  if (((events & APP_BUTTON_EVENT_CIRCLE) != 0U) &&
      ((uint32_t)(now - s_app.last_button_tick[1]) >=
       APP_BUTTON_DEBOUNCE_MS)) {
    s_app.last_button_tick[1] = now;
    (void)AppIntegration_QueueCommand(APP_COMMAND_TASK5_CIRCLE);
  }
  if (((events & APP_BUTTON_EVENT_INFINITY) != 0U) &&
      ((uint32_t)(now - s_app.last_button_tick[2]) >=
       APP_BUTTON_DEBOUNCE_MS)) {
    s_app.last_button_tick[2] = now;
    (void)AppIntegration_QueueCommand(APP_COMMAND_TASK5_INFINITY);
  }
}

static void AppIntegration_UpdateLockActivity(void)
{
  const pll_demo_status_t *pll = PLL_Demo_GetStatus();

  if (pll == NULL) {
    return;
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

static const char *AppIntegration_Task5Text(app_waveform_t waveform)
{
  switch (waveform) {
    case APP_WAVEFORM_TASK1_LINE:
      return "Mode: diagonal (camera logic reserved)";
    case APP_WAVEFORM_TASK2_CIRCLE:
      return "Mode: circle (camera logic reserved)";
    case APP_WAVEFORM_TASK3_INFINITY:
      return "Mode: infinity (camera logic reserved)";
    case APP_WAVEFORM_NONE:
    default:
      return "PA0:diagonal PB9:circle PB8:infinity";
  }
}

static void AppIntegration_Render(void)
{
  app_status_t status;
  uint16_t page_id;
  HAL_StatusTypeDef result;

  AppCore_GetStatus(&s_app.core, &status);
  if (!s_app.render_forced &&
      (status.state == s_app.rendered_state) &&
      ((status.state != APP_STATE_TASK5_PLACEHOLDER) ||
       (status.waveform == s_app.rendered_task5_waveform))) {
    return;
  }

  page_id = (status.state == APP_STATE_TASK14)
                ? 1U
                : (status.state == APP_STATE_TASK5_PLACEHOLDER)
                      ? 2U
                      : 0U;
  result = TJC_PageSetById(page_id);
  if ((result == HAL_OK) &&
      (status.state == APP_STATE_TASK5_PLACEHOLDER)) {
    result = TJC_TextSetText(
        "t2", AppIntegration_Task5Text(status.waveform));
  }

  if (result == HAL_OK) {
    s_app.rendered_state = status.state;
    s_app.rendered_task5_waveform = status.waveform;
    s_app.render_forced = false;
  }
}

HAL_StatusTypeDef AppIntegration_Init(void)
{
  app_port_t port = {
      .enter_safe = AppIntegration_EnterSafe,
      .run_direct = AppIntegration_RunDirect,
      .run_lock = AppIntegration_RunLock,
      .set_lock_amplitude = AppIntegration_SetLockAmplitude,
      .enable_task5_buttons = AppIntegration_EnableButtons,
      .context = NULL,
  };

  memset(&s_app, 0, sizeof(s_app));
  s_app.render_forced = true;
  TJC_RegisterMessageHandler(AppIntegration_HmiMessage);
  if (TJC_Init() != HAL_OK) {
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
  AppIntegration_ProcessButtons();
  while ((processed < APP_COMMAND_QUEUE_SIZE) &&
         AppIntegration_PopCommand(&command)) {
    (void)AppCore_HandleCommand(&s_app.core, command);
    processed++;
  }

  PLL_Demo_Process();
  AppIntegration_UpdateLockActivity();
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
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_RxCpltCallback(huart);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_TxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_ErrorCallback(huart);
}
