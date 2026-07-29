#include "app_core.h"
#include "app_hmi_map.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint32_t safe_count;
  uint32_t direct_count;
  uint32_t lock_count;
  uint32_t amplitude_count;
  uint32_t button_enable_count;
  app_waveform_t waveform;
  uint8_t amplitude_div;
  bool buttons_enabled;
  bool fail_next;
} fake_port_t;

static bool Fake_Result(fake_port_t *port)
{
  bool result = !port->fail_next;
  port->fail_next = false;
  return result;
}

static bool Fake_Safe(void *context)
{
  fake_port_t *port = (fake_port_t *)context;
  port->safe_count++;
  return Fake_Result(port);
}

static bool Fake_Direct(void *context)
{
  fake_port_t *port = (fake_port_t *)context;
  port->direct_count++;
  return Fake_Result(port);
}

static bool Fake_RunLock(void *context,
                         app_waveform_t waveform,
                         uint8_t amplitude_div)
{
  fake_port_t *port = (fake_port_t *)context;
  port->lock_count++;
  port->waveform = waveform;
  port->amplitude_div = amplitude_div;
  return Fake_Result(port);
}

static bool Fake_SetAmplitude(void *context,
                              app_waveform_t waveform,
                              uint8_t amplitude_div)
{
  fake_port_t *port = (fake_port_t *)context;
  port->amplitude_count++;
  port->waveform = waveform;
  port->amplitude_div = amplitude_div;
  return Fake_Result(port);
}

static void Fake_EnableButtons(void *context, bool enable)
{
  fake_port_t *port = (fake_port_t *)context;
  port->button_enable_count++;
  port->buttons_enabled = enable;
}

static void TestHmiMap(void)
{
  app_command_t command = APP_COMMAND_GO_MENU;

  assert(!AppHmi_MapTouch(0U, 2U, APP_HMI_TOUCH_RELEASE, &command));
  assert(AppHmi_MapTouch(0U, 2U, APP_HMI_TOUCH_PRESS, &command));
  assert(command == APP_COMMAND_ENTER_TASK14);
  assert(AppHmi_MapTouch(0U, 3U, APP_HMI_TOUCH_PRESS, &command));
  assert(command == APP_COMMAND_ENTER_TASK5);
  assert(!AppHmi_MapTouch(0U, 4U, APP_HMI_TOUCH_PRESS, &command));
  assert(AppHmi_MapTouch(1U, 10U, APP_HMI_TOUCH_PRESS, &command));
  assert(command == APP_COMMAND_SELECT_TASK1);
  assert(AppHmi_MapTouch(1U, 6U, APP_HMI_TOUCH_PRESS, &command));
  assert(command == APP_COMMAND_SET_AMPLITUDE_2DIV);
  assert(AppHmi_MapTouch(2U, 2U, APP_HMI_TOUCH_PRESS, &command));
  assert(command == APP_COMMAND_GO_MENU);
  assert(!AppHmi_MapTouch(2U, 3U, APP_HMI_TOUCH_PRESS, &command));
}

static void TestAppStateMachine(void)
{
  app_core_t core;
  app_status_t status;
  fake_port_t fake;
  app_port_t port;

  memset(&fake, 0, sizeof(fake));
  memset(&port, 0, sizeof(port));
  port.enter_safe = Fake_Safe;
  port.run_direct = Fake_Direct;
  port.run_lock = Fake_RunLock;
  port.set_lock_amplitude = Fake_SetAmplitude;
  port.enable_task5_buttons = Fake_EnableButtons;
  port.context = &fake;

  assert(AppCore_Init(&core, &port));
  AppCore_GetStatus(&core, &status);
  assert(status.state == APP_STATE_MENU_SAFE);
  assert(fake.safe_count == 1U);
  assert(!fake.buttons_enabled);

  assert(AppCore_HandleCommand(&core, APP_COMMAND_ENTER_TASK1));
  AppCore_GetStatus(&core, &status);
  assert(status.state == APP_STATE_TASK14);
  assert(status.waveform == APP_WAVEFORM_TASK1_LINE);
  assert(status.activity == APP_ACTIVITY_DIRECT);
  assert(fake.direct_count == 1U);

  assert(AppCore_HandleCommand(
      &core, APP_COMMAND_SET_AMPLITUDE_2DIV));
  AppCore_GetStatus(&core, &status);
  assert(status.activity == APP_ACTIVITY_LOCK_SEARCH);
  assert(status.amplitude_div == 2U);
  assert(fake.lock_count == 1U);
  assert(fake.waveform == APP_WAVEFORM_TASK1_LINE);

  assert(AppCore_HandleCommand(&core, APP_COMMAND_SELECT_TASK2));
  assert(fake.waveform == APP_WAVEFORM_TASK2_CIRCLE);
  assert(fake.amplitude_div == 8U);
  assert(AppCore_HandleCommand(
      &core, APP_COMMAND_SET_AMPLITUDE_4DIV));
  assert(fake.amplitude_count == 1U);
  assert(fake.amplitude_div == 4U);

  assert(AppCore_HandleCommand(&core, APP_COMMAND_GO_MENU));
  AppCore_GetStatus(&core, &status);
  assert(status.state == APP_STATE_MENU_SAFE);
  assert(!AppCore_HandleCommand(&core, APP_COMMAND_SELECT_TASK3));
  AppCore_GetStatus(&core, &status);
  assert(status.rejected_command_count == 1U);

  assert(AppCore_HandleCommand(&core, APP_COMMAND_ENTER_TASK14));
  AppCore_GetStatus(&core, &status);
  assert(status.waveform == APP_WAVEFORM_TASK2_CIRCLE);
  assert(status.activity == APP_ACTIVITY_IDLE);
  assert(AppCore_HandleCommand(
      &core, APP_COMMAND_SET_AMPLITUDE_6DIV));
  assert(fake.lock_count == 3U);
  assert(fake.waveform == APP_WAVEFORM_TASK2_CIRCLE);
  assert(fake.amplitude_div == 6U);

  assert(AppCore_HandleCommand(&core, APP_COMMAND_ENTER_TASK5));
  assert(fake.buttons_enabled);
  assert(AppCore_HandleCommand(&core, APP_COMMAND_TASK5_INFINITY));
  AppCore_GetStatus(&core, &status);
  assert(status.state == APP_STATE_TASK5_PLACEHOLDER);
  assert(status.waveform == APP_WAVEFORM_TASK3_INFINITY);

  assert(AppCore_HandleCommand(&core, APP_COMMAND_GO_MENU));
  assert(!fake.buttons_enabled);

  fake.fail_next = true;
  assert(!AppCore_HandleCommand(&core, APP_COMMAND_ENTER_TASK1));
  AppCore_GetStatus(&core, &status);
  assert(status.state == APP_STATE_ERROR_SAFE);
  assert(status.last_error == APP_ERROR_SAFE_OUTPUT);
}

int main(void)
{
  TestHmiMap();
  TestAppStateMachine();
  puts("app-core tests passed");
  return 0;
}
