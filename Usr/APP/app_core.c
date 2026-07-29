#include "app_core.h"

#include <stddef.h>
#include <string.h>

static void AppCore_Touch(app_core_t *core)
{
  core->status.revision++;
}

static bool AppCore_EnterError(app_core_t *core, app_error_t error)
{
  if (core == NULL) {
    return false;
  }

  if (core->port.enable_task5_buttons != NULL) {
    core->port.enable_task5_buttons(core->port.context, false);
  }
  if (core->port.enter_safe != NULL) {
    (void)core->port.enter_safe(core->port.context);
  }

  core->status.state = APP_STATE_ERROR_SAFE;
  core->status.activity = APP_ACTIVITY_ERROR;
  core->status.last_error = error;
  core->status.transition_count++;
  AppCore_Touch(core);
  return false;
}

static bool AppCore_EnterSafeBaseline(app_core_t *core)
{
  if ((core->port.enter_safe == NULL) ||
      !core->port.enter_safe(core->port.context)) {
    return AppCore_EnterError(core, APP_ERROR_SAFE_OUTPUT);
  }
  return true;
}

static void AppCore_DisableTask5Buttons(app_core_t *core)
{
  if (core->port.enable_task5_buttons != NULL) {
    core->port.enable_task5_buttons(core->port.context, false);
  }
}

static uint8_t AppCore_AmplitudeForCommand(app_command_t command)
{
  switch (command) {
    case APP_COMMAND_SET_AMPLITUDE_2DIV:
      return 2U;
    case APP_COMMAND_SET_AMPLITUDE_4DIV:
      return 4U;
    case APP_COMMAND_SET_AMPLITUDE_6DIV:
      return 6U;
    case APP_COMMAND_SET_AMPLITUDE_8DIV:
      return 8U;
    default:
      return 0U;
  }
}

static bool AppCore_RunDirect(app_core_t *core)
{
  AppCore_DisableTask5Buttons(core);
  if (!AppCore_EnterSafeBaseline(core)) {
    return false;
  }
  if ((core->port.run_direct == NULL) ||
      !core->port.run_direct(core->port.context)) {
    return AppCore_EnterError(core, APP_ERROR_DIRECT_OUTPUT);
  }

  core->status.state = APP_STATE_TASK14;
  core->status.waveform = APP_WAVEFORM_TASK1_LINE;
  core->status.activity = APP_ACTIVITY_DIRECT;
  core->status.amplitude_div = 8U;
  core->status.last_error = APP_ERROR_NONE;
  core->last_task14_waveform = APP_WAVEFORM_TASK1_LINE;
  core->status.transition_count++;
  AppCore_Touch(core);
  return true;
}

static bool AppCore_RunLock(app_core_t *core,
                            app_waveform_t waveform,
                            uint8_t amplitude_div)
{
  AppCore_DisableTask5Buttons(core);
  if (!AppCore_EnterSafeBaseline(core)) {
    return false;
  }
  if ((core->port.run_lock == NULL) ||
      !core->port.run_lock(core->port.context,
                           waveform,
                           amplitude_div)) {
    return AppCore_EnterError(core, APP_ERROR_LOCK_START);
  }

  core->status.state = APP_STATE_TASK14;
  core->status.waveform = waveform;
  core->status.activity = APP_ACTIVITY_LOCK_SEARCH;
  core->status.amplitude_div = amplitude_div;
  core->status.last_error = APP_ERROR_NONE;
  core->last_task14_waveform = waveform;
  core->status.transition_count++;
  AppCore_Touch(core);
  return true;
}

static bool AppCore_SetAmplitude(app_core_t *core, uint8_t amplitude_div)
{
  if ((core->status.state != APP_STATE_TASK14) ||
      (core->status.waveform == APP_WAVEFORM_NONE)) {
    core->status.rejected_command_count++;
    AppCore_Touch(core);
    return false;
  }

  /*
   * Task 1 itself is a physical relay bypass. Once an amplitude button is
   * selected, reproduce the same in-phase line with the DDS so ASF can control
   * its height. Task 2/3 keep their current lock and only change ASF.
   */
  if ((core->status.activity == APP_ACTIVITY_DIRECT) ||
      (core->status.activity == APP_ACTIVITY_IDLE)) {
    return AppCore_RunLock(core, core->status.waveform, amplitude_div);
  }

  if ((core->port.set_lock_amplitude == NULL) ||
      !core->port.set_lock_amplitude(core->port.context,
                                     core->status.waveform,
                                     amplitude_div)) {
    return AppCore_EnterError(core, APP_ERROR_LOCK_START);
  }

  core->status.amplitude_div = amplitude_div;
  core->status.transition_count++;
  AppCore_Touch(core);
  return true;
}

bool AppCore_Init(app_core_t *core, const app_port_t *port)
{
  if ((core == NULL) || (port == NULL) ||
      (port->enter_safe == NULL) ||
      (port->run_direct == NULL) ||
      (port->run_lock == NULL) ||
      (port->set_lock_amplitude == NULL)) {
    return false;
  }

  memset(core, 0, sizeof(*core));
  core->port = *port;
  core->status.state = APP_STATE_MENU_SAFE;
  core->status.activity = APP_ACTIVITY_IDLE;
  core->status.waveform = APP_WAVEFORM_NONE;
  core->status.amplitude_div = 8U;
  core->last_task14_waveform = APP_WAVEFORM_NONE;

  AppCore_DisableTask5Buttons(core);
  if (!AppCore_EnterSafeBaseline(core)) {
    return false;
  }
  AppCore_Touch(core);
  return true;
}

bool AppCore_HandleCommand(app_core_t *core, app_command_t command)
{
  uint8_t amplitude_div;

  if (core == NULL) {
    return false;
  }

  amplitude_div = AppCore_AmplitudeForCommand(command);
  if (amplitude_div != 0U) {
    return AppCore_SetAmplitude(core, amplitude_div);
  }

  switch (command) {
    case APP_COMMAND_GO_MENU:
      AppCore_DisableTask5Buttons(core);
      if (!AppCore_EnterSafeBaseline(core)) {
        return false;
      }
      core->status.state = APP_STATE_MENU_SAFE;
      core->status.waveform = APP_WAVEFORM_NONE;
      core->status.activity = APP_ACTIVITY_IDLE;
      core->status.amplitude_div = 8U;
      core->status.last_error = APP_ERROR_NONE;
      core->status.transition_count++;
      AppCore_Touch(core);
      return true;

    case APP_COMMAND_ENTER_TASK1:
    case APP_COMMAND_SELECT_TASK1:
      if ((command == APP_COMMAND_SELECT_TASK1) &&
          (core->status.state != APP_STATE_TASK14)) {
        break;
      }
      return AppCore_RunDirect(core);

    case APP_COMMAND_ENTER_TASK14:
      AppCore_DisableTask5Buttons(core);
      if (!AppCore_EnterSafeBaseline(core)) {
        return false;
      }
      core->status.state = APP_STATE_TASK14;
      core->status.waveform = core->last_task14_waveform;
      core->status.activity = APP_ACTIVITY_IDLE;
      core->status.amplitude_div = 8U;
      core->status.last_error = APP_ERROR_NONE;
      core->status.transition_count++;
      AppCore_Touch(core);
      return true;

    case APP_COMMAND_ENTER_TASK5:
      if (!AppCore_EnterSafeBaseline(core)) {
        return false;
      }
      if (core->port.enable_task5_buttons != NULL) {
        core->port.enable_task5_buttons(core->port.context, true);
      }
      core->status.state = APP_STATE_TASK5_PLACEHOLDER;
      core->status.waveform = APP_WAVEFORM_NONE;
      core->status.activity = APP_ACTIVITY_TASK5_WAITING;
      core->status.last_error = APP_ERROR_NONE;
      core->status.transition_count++;
      AppCore_Touch(core);
      return true;

    case APP_COMMAND_SELECT_TASK2:
      if (core->status.state == APP_STATE_TASK14) {
        return AppCore_RunLock(core, APP_WAVEFORM_TASK2_CIRCLE, 8U);
      }
      break;

    case APP_COMMAND_SELECT_TASK3:
      if (core->status.state == APP_STATE_TASK14) {
        return AppCore_RunLock(core, APP_WAVEFORM_TASK3_INFINITY, 8U);
      }
      break;

    case APP_COMMAND_TASK5_LINE:
    case APP_COMMAND_TASK5_CIRCLE:
    case APP_COMMAND_TASK5_INFINITY:
      if (core->status.state == APP_STATE_TASK5_PLACEHOLDER) {
        core->status.waveform =
            (command == APP_COMMAND_TASK5_LINE)
                ? APP_WAVEFORM_TASK1_LINE
                : (command == APP_COMMAND_TASK5_CIRCLE)
                      ? APP_WAVEFORM_TASK2_CIRCLE
                      : APP_WAVEFORM_TASK3_INFINITY;
        core->status.activity = APP_ACTIVITY_TASK5_WAITING;
        core->status.transition_count++;
        AppCore_Touch(core);
        return true;
      }
      break;

    case APP_COMMAND_SET_AMPLITUDE_2DIV:
    case APP_COMMAND_SET_AMPLITUDE_4DIV:
    case APP_COMMAND_SET_AMPLITUDE_6DIV:
    case APP_COMMAND_SET_AMPLITUDE_8DIV:
    default:
      break;
  }

  core->status.rejected_command_count++;
  AppCore_Touch(core);
  return false;
}

void AppCore_SetLockActivity(app_core_t *core, app_activity_t activity)
{
  if ((core == NULL) ||
      (core->status.state != APP_STATE_TASK14) ||
      ((activity != APP_ACTIVITY_LOCK_SEARCH) &&
       (activity != APP_ACTIVITY_LOCK_ACQUIRE) &&
       (activity != APP_ACTIVITY_LOCKED))) {
    return;
  }

  if (core->status.activity != activity) {
    core->status.activity = activity;
    AppCore_Touch(core);
  }
}

void AppCore_ReportError(app_core_t *core, app_error_t error)
{
  if ((core == NULL) || (error == APP_ERROR_NONE)) {
    return;
  }
  (void)AppCore_EnterError(core, error);
}

void AppCore_GetStatus(const app_core_t *core, app_status_t *status)
{
  if ((core == NULL) || (status == NULL)) {
    return;
  }
  *status = core->status;
}
