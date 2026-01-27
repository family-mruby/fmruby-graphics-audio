#ifndef GRAPHICS_TASK_H
#define GRAPHICS_TASK_H

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

#endif // GRAPHICS_TASK_H
