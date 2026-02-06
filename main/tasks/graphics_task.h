#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Graphics rendering task
 * Handles display updates and rendering
 */
void graphics_task(void *pvParameters);

/**
 * Stop the graphics task
 */
void graphics_task_stop(void);

#ifdef __cplusplus
}
#endif
