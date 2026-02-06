#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Communication processing task
 * Handles socket/SPI communication
 */
void comm_task(void *pvParameters);

/**
 * Stop the communication task
 */
void comm_task_stop(void);

#ifdef __cplusplus
}
#endif
