#ifndef APP_CORE_H
#define APP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  APP_STATE_MENU_SAFE = 0,
  APP_STATE_TASK14,
  APP_STATE_TASK5,
  APP_STATE_ERROR_SAFE
} app_state_t;

typedef enum {
  APP_WAVEFORM_NONE = 0,
  APP_WAVEFORM_TASK1_LINE,
  APP_WAVEFORM_TASK2_CIRCLE,
  APP_WAVEFORM_TASK3_INFINITY
} app_waveform_t;

typedef enum {
  APP_ACTIVITY_IDLE = 0,
  APP_ACTIVITY_DIRECT,
  APP_ACTIVITY_LOCK_SEARCH,
  APP_ACTIVITY_LOCK_ACQUIRE,
  APP_ACTIVITY_LOCKED,
  APP_ACTIVITY_TASK5_WAIT_SELECTION,
  APP_ACTIVITY_TASK5_WAITING = APP_ACTIVITY_TASK5_WAIT_SELECTION,
  APP_ACTIVITY_TASK5_COMMUNICATING,
  APP_ACTIVITY_TASK5_SEARCHING,
  APP_ACTIVITY_TASK5_LOCKING,
  APP_ACTIVITY_TASK5_LOCKED,
  APP_ACTIVITY_ERROR
} app_activity_t;

typedef enum {
  APP_COMMAND_GO_MENU = 0,
  APP_COMMAND_ENTER_TASK1,
  APP_COMMAND_ENTER_TASK14,
  APP_COMMAND_ENTER_TASK5,
  APP_COMMAND_SELECT_TASK1,
  APP_COMMAND_SELECT_TASK2,
  APP_COMMAND_SELECT_TASK3,
  APP_COMMAND_SET_AMPLITUDE_2DIV,
  APP_COMMAND_SET_AMPLITUDE_4DIV,
  APP_COMMAND_SET_AMPLITUDE_6DIV,
  APP_COMMAND_SET_AMPLITUDE_8DIV,
  APP_COMMAND_TASK5_LINE,
  APP_COMMAND_TASK5_CIRCLE,
  APP_COMMAND_TASK5_INFINITY
} app_command_t;

typedef enum {
  APP_ERROR_NONE = 0,
  APP_ERROR_SAFE_OUTPUT,
  APP_ERROR_DIRECT_OUTPUT,
  APP_ERROR_LOCK_START,
  APP_ERROR_LOCK_RUNTIME,
  APP_ERROR_TASK5_RUNTIME
} app_error_t;

typedef struct {
  bool (*enter_safe)(void *context);
  bool (*run_direct)(void *context);
  bool (*run_lock)(void *context,
                   app_waveform_t waveform,
                   uint8_t amplitude_div);
  bool (*set_lock_amplitude)(void *context,
                             app_waveform_t waveform,
                             uint8_t amplitude_div);
  void (*enable_task5_buttons)(void *context, bool enable);
  void *context;
} app_port_t;

typedef struct {
  app_state_t state;
  app_waveform_t waveform;
  app_activity_t activity;
  app_error_t last_error;
  uint8_t amplitude_div;
  uint32_t revision;
  uint32_t transition_count;
  uint32_t rejected_command_count;
} app_status_t;

typedef struct {
  app_port_t port;
  app_status_t status;
  app_waveform_t last_task14_waveform;
} app_core_t;

bool AppCore_Init(app_core_t *core, const app_port_t *port);
bool AppCore_HandleCommand(app_core_t *core, app_command_t command);
void AppCore_SetLockActivity(app_core_t *core, app_activity_t activity);
void AppCore_SetTask5Activity(app_core_t *core,
                              app_activity_t activity);
void AppCore_ReportError(app_core_t *core, app_error_t error);
void AppCore_GetStatus(const app_core_t *core, app_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_CORE_H */
