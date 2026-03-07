#include "comm_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "comm_interface.h"
#include <string.h>

static const char *TAG = "comm_task";
static volatile int task_running = 1;

void comm_test(void) {
  ESP_LOGI(TAG, "SPI task started on core %d", (int)xPortGetCoreID());

  const comm_interface_t* comm = comm_get_interface();

  // SPI初期化
  if (comm->init() != 0) {
    ESP_LOGE(TAG, "SPI initialization failed!");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "SPI initialized successfully, starting communication loop...");

  // テスト用のダミーデータ
  uint8_t test_data[] = {0xAA, 0x55, 0x01, 0x02, 0x03, 0x04};
  int send_count = 0;

  while (task_running) {
    // 通信処理
    int processed = comm->process();
    if (processed < 0) {
      ESP_LOGE(TAG, "SPI process error");
    }

    // // 5秒ごとにテストデータを送信
    // send_count++;
    // if (send_count >= 500) {  // 10ms * 500 = 5秒
    //   printf("SPI: Sending test data...\n");
    //   int sent = comm->send(test_data, sizeof(test_data));
    //   if (sent > 0) {
    //     printf("SPI: Sent %d bytes\n", sent);
    //   } else {
    //     printf("SPI: Send failed\n");
    //   }
    //   send_count = 0;
    // }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void comm_task_stop(void) {
    task_running = 0;
}

void comm_task(void *pvParameters) {
    ESP_LOGI(TAG, "started on core %d", (int)xPortGetCoreID());

    MessageBufferHandle_t msg_buffer = (MessageBufferHandle_t)pvParameters;
    if (!msg_buffer) {
        ESP_LOGE(TAG, "Invalid MessageBuffer handle");
        vTaskDelete(NULL);
        return;
    }

#ifdef ENABLE_SPI_TEST
    //testing SPI
    comm_test();
    return;
#endif
    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        ESP_LOGE(TAG, "comm interface is NULL");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Calling comm->init()...");
    // Initialize communication interface
    int init_ret = comm->init();
    ESP_LOGI(TAG, "comm->init() returned %d", init_ret);
    if (init_ret < 0) {
        ESP_LOGE(TAG, "SPI init failed (%d)", init_ret);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SPI initialized OK. Entering main loop.");

    extern void spi_slave_print_stats(void);
    int loop_count = 0;

    // Message structure to send to message_handler_task
    typedef struct {
        uint8_t type;
        uint8_t seq;
        uint8_t sub_cmd;
        uint16_t payload_len;
        uint8_t payload[1024];  // MSG_QUEUE_MAX_PAYLOAD
    } message_data_t;

    // Main communication processing loop
    while (task_running) {
        // Process low-level communication (accept, read, decode frames)
        int frames_received = comm->process();

        if (frames_received < 0) {
            //ESP_LOGW(TAG, "Communication process error");
        }

        // Forward decoded messages to handler task via MessageBuffer
        uint8_t type, seq, sub_cmd;
        const uint8_t *payload;
        size_t payload_len;

        while (comm->receive_message(&type, &seq, &sub_cmd, &payload, &payload_len) > 0) {
            ESP_LOGD(TAG, "MSG: type=%u seq=%u sub_cmd=0x%02x len=%u",
                   type, seq, sub_cmd, (unsigned)payload_len);
            
            // Prepare message for handler task
            message_data_t msg = {
                .type = type,
                .seq = seq,
                .sub_cmd = sub_cmd,
                .payload_len = (uint16_t)payload_len
            };
            
            // Copy payload (safe as payload_len <= 1024)
            if (payload_len <= sizeof(msg.payload)) {
                memcpy(msg.payload, payload, payload_len);
            } else {
                ESP_LOGE(TAG, "Payload too large: %zu bytes", payload_len);
                continue;
            }
            
            // Send to message_handler_task via MessageBuffer
            size_t bytes_sent = xMessageBufferSend(msg_buffer, (void*)&msg, 
                                                   sizeof(message_data_t), pdMS_TO_TICKS(100));
            if (bytes_sent == 0) {
                ESP_LOGW(TAG, "Failed to send message to handler task (buffer full?)");
            }
        }

        // Print stats every 5 seconds
        loop_count++;
        if (loop_count % 5000 == 0) {
            spi_slave_print_stats();
        }

        // Only delay when idle (no frames processed).
        // When busy, loop immediately to re-queue SPI buffers promptly
        // and prevent the slave's transaction queue from being starved.
        if (frames_received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Cleanup communication interface
    comm->cleanup();

    ESP_LOGI(TAG, "Communication task stopped");
    vTaskDelete(NULL);
}
