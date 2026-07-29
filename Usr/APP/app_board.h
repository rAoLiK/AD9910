#ifndef APP_BOARD_H
#define APP_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_RELAY_DIRECT_ACTIVE_HIGH
#define APP_RELAY_DIRECT_ACTIVE_HIGH (1U)
#endif

#define APP_BUTTON_EVENT_LINE      (1UL << 0)
#define APP_BUTTON_EVENT_CIRCLE    (1UL << 1)
#define APP_BUTTON_EVENT_INFINITY  (1UL << 2)

typedef enum {
  APP_BOARD_PATH_DIRECT = 0,
  APP_BOARD_PATH_DDS
} app_board_path_t;

void AppBoard_Init(void);
void AppBoard_SetPath(app_board_path_t path);
app_board_path_t AppBoard_GetPath(void);
void AppBoard_EnableTask5Buttons(bool enable);
uint32_t AppBoard_TakeButtonEvents(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BOARD_H */
