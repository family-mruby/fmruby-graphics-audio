#include "comm_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "comm_interface.h"
#include "message_handler.h"

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
    printf("[comm_task] started on core %d\n", (int)xPortGetCoreID());
    ESP_LOGI(TAG, "Communication task started on core %d", (int)xPortGetCoreID());

#ifdef ENABLE_SPI_TEST
    //testing SPI
    comm_test();
    return;
#endif
    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        printf("[comm_task] ERROR: comm interface is NULL\n");
        vTaskDelete(NULL);
        return;
    }

    printf("[comm_task] Calling comm->init()...\n");
    // Initialize communication interface
    int init_ret = comm->init();
    printf("[comm_task] comm->init() returned %d\n", init_ret);
    if (init_ret < 0) {
        printf("[comm_task] ERROR: SPI init failed (%d)\n", init_ret);
        vTaskDelete(NULL);
        return;
    }

    printf("[comm_task] SPI initialized OK. Entering main loop.\n");

    extern void spi_slave_print_stats(void);
    int loop_count = 0;

    // Main communication processing loop
    while (task_running) {
        // Process low-level communication (accept, read, decode frames)
        int frames_received = comm->process();

        if (frames_received < 0) {
            //ESP_LOGW(TAG, "Communication process error");
        }

        // Process decoded messages
        uint8_t type, seq, sub_cmd;
        const uint8_t *payload;
        size_t payload_len;

        while (comm->receive_message(&type, &seq, &sub_cmd, &payload, &payload_len) > 0) {
            printf("[comm_task] MSG: type=%u seq=%u sub_cmd=0x%02x len=%u\n",
                   type, seq, sub_cmd, (unsigned)payload_len);
            // Handle message in application layer
            int result = message_handler_process(type, seq, sub_cmd, payload, payload_len);
            if (result < 0) {
                printf("[comm_task] handler FAILED: type=%u seq=%u sub_cmd=0x%02x\n",
                       type, seq, sub_cmd);
            }
        }

        // Print stats every 5 seconds
        loop_count++;
        if (loop_count % 5000 == 0) {
            spi_slave_print_stats();
        }

        // Small delay to prevent busy waiting
        // Use 1ms to handle high-frequency graphics commands
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Cleanup communication interface
    comm->cleanup();

    ESP_LOGI(TAG, "Communication task stopped");
    vTaskDelete(NULL);
}
