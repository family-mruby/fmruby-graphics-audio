#pragma once

// Graphics task
#define GRAPHICS_TASK_STACK_SIZE     8192
#define GRAPHICS_TASK_PRIORITY       5
#define GRAPHICS_TASK_CORE           0

// Audio task (highest priority - hard real-time 60Hz constraint)
#ifdef CONFIG_IDF_TARGET_LINUX
#define AUDIO_TASK_STACK_SIZE        (8192 * 2)
#else
#define AUDIO_TASK_STACK_SIZE        8192
#endif
#define AUDIO_TASK_PRIORITY          7
#define AUDIO_TASK_CORE              0

// Communication task (SPI slave / socket) - high priority for responsiveness
#ifdef CONFIG_IDF_TARGET_LINUX
#define COMM_TASK_STACK_SIZE         (8192 * 2)
#else
#define COMM_TASK_STACK_SIZE         8192
#endif
#define COMM_TASK_PRIORITY           6
#define COMM_TASK_CORE               0

// Message handler task - lower priority for application processing
#define MESSAGE_HANDLER_TASK_STACK_SIZE  8192
#define MESSAGE_HANDLER_TASK_PRIORITY    5
#define MESSAGE_HANDLER_TASK_CORE        0

// MessageBuffer: number of messages buffered between comm and handler tasks
#ifdef CONFIG_IDF_TARGET_LINUX
#define MSG_BUFFER_NUM_MESSAGES      1024
#else
#define MSG_BUFFER_NUM_MESSAGES      8
#endif
