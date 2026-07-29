#ifndef APP_HMI_MAP_H
#define APP_HMI_MAP_H

#include "app_core.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_HMI_TOUCH_RELEASE (0U)
#define APP_HMI_TOUCH_PRESS   (1U)

bool AppHmi_MapTouch(uint8_t page_id,
                     uint8_t component_id,
                     uint8_t touch_event,
                     app_command_t *command);

#ifdef __cplusplus
}
#endif

#endif /* APP_HMI_MAP_H */
