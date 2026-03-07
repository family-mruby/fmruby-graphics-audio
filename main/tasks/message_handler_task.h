#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Message handler task
 * Processes incoming messages from comm_task via MessageBuffer
 * @param pvParameters MessageBufferHandle_t (from comm_task)
 */
void message_handler_task(void *pvParameters);

/**
 * Stop the message handler task
 */
void message_handler_task_stop(void);

#ifdef __cplusplus
}
#endif
