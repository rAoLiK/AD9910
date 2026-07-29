#include "app_hmi_map.h"

#include <stddef.h>

typedef struct {
  uint8_t page_id;
  uint8_t component_id;
  app_command_t command;
} app_hmi_touch_map_t;

static const app_hmi_touch_map_t s_touch_map[] = {
    /* page0/menu: Task1-4 menu, Task5. */
    {0U, 2U, APP_COMMAND_ENTER_TASK14},
    {0U, 3U, APP_COMMAND_ENTER_TASK5},

    /* page1/Task1-4: exit, Task2, Task3, 2/4/6/8 div, Task1. */
    {1U, 2U, APP_COMMAND_GO_MENU},
    {1U, 3U, APP_COMMAND_SELECT_TASK2},
    {1U, 4U, APP_COMMAND_SELECT_TASK3},
    {1U, 6U, APP_COMMAND_SET_AMPLITUDE_2DIV},
    {1U, 7U, APP_COMMAND_SET_AMPLITUDE_4DIV},
    {1U, 8U, APP_COMMAND_SET_AMPLITUDE_6DIV},
    {1U, 9U, APP_COMMAND_SET_AMPLITUDE_8DIV},
    {1U, 10U, APP_COMMAND_SELECT_TASK1},

    /* page2/Task5: exit. */
    {2U, 2U, APP_COMMAND_GO_MENU},
};

bool AppHmi_MapTouch(uint8_t page_id,
                     uint8_t component_id,
                     uint8_t touch_event,
                     app_command_t *command)
{
  uint32_t index;

  if ((command == NULL) || (touch_event != APP_HMI_TOUCH_PRESS)) {
    return false;
  }

  for (index = 0U;
       index < (sizeof(s_touch_map) / sizeof(s_touch_map[0]));
       ++index) {
    if ((s_touch_map[index].page_id == page_id) &&
        (s_touch_map[index].component_id == component_id)) {
      *command = s_touch_map[index].command;
      return true;
    }
  }
  return false;
}
