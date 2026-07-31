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

typedef enum {
  APP_BOARD_PATH_DIRECT = 0,
  APP_BOARD_PATH_DDS
} app_board_path_t;

typedef enum {
  APP_BOARD_SIGNAL_DAC = 0,
  APP_BOARD_SIGNAL_DDS
} app_board_signal_source_t;

typedef struct {
  uint8_t line;
  uint8_t circle;
  uint8_t infinity;
} app_board_button_counts_t;

void AppBoard_Init(void);
void AppBoard_SetPath(app_board_path_t path);
app_board_path_t AppBoard_GetPath(void);
void AppBoard_SetSignalSource(app_board_signal_source_t source);
app_board_signal_source_t AppBoard_GetSignalSource(void);
void AppBoard_EnableTask5Buttons(bool enable);
void AppBoard_TakeButtonCounts(app_board_button_counts_t *counts);
bool AppBoard_IsTask5ButtonPressed(uint8_t button_index);

#ifdef __cplusplus
}
#endif

#endif /* APP_BOARD_H */
