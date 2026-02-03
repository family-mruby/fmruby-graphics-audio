#include "comm_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "comm_interface.h"

static const char *TAG = "comm_task";
static volatile int task_running = 1;

void comm_test(void) {
  printf("SPI task started on core %d\n", (int)xPortGetCoreID());

  const comm_interface_t* comm = comm_get_interface();

  // SPI初期化
  if (comm->init() != 0) {
    printf("SPI initialization failed!\n");
    vTaskDelete(NULL);
    return;
  }

  printf("SPI initialized successfully, starting communication loop...\n");

  // テスト用のダミーデータ
  uint8_t test_data[] = {0xAA, 0x55, 0x01, 0x02, 0x03, 0x04};
  int send_count = 0;

  while (task_running) {
    // 通信処理
    int processed = comm->process();
    if (processed < 0) {
      printf("SPI process error\n");
    }

    // 5秒ごとにテストデータを送信
    send_count++;
    if (send_count >= 500) {  // 10ms * 500 = 5秒
      printf("SPI: Sending test data...\n");
      int sent = comm->send(test_data, sizeof(test_data));
      if (sent > 0) {
        printf("SPI: Sent %d bytes\n", sent);
      } else {
        printf("SPI: Send failed\n");
      }
      send_count = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void comm_task_stop(void) {
    task_running = 0;
}

void comm_task(void *pvParameters) {
    ESP_LOGI(TAG, "Communication task started on core %d", (int)xPortGetCoreID());

#ifndef CONFIG_IDF_TARGET_LINUX
    //testing SPI
    comm_test();
    return;
#else
    // Linux: Use comm_interface for socket communication
    const comm_interface_t *comm = COMM_INTERFACE;
    if (!comm) {
        ESP_LOGE(TAG, "Failed to get communication interface");
        vTaskDelete(NULL);
        return;
    }

    // Initialize communication interface (socket server)
    if (comm->init() < 0) {
        ESP_LOGE(TAG, "Communication interface initialization failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Communication interface initialized successfully");

    // Main communication processing loop
    while (task_running) {
        // Process incoming messages
        int result = comm->process();

        if (result < 0) {
            //ESP_LOGW(TAG, "Communication process error");
        }

        // Small delay to prevent busy waiting (~60 FPS)
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    // Cleanup communication interface
    comm->cleanup();

    ESP_LOGI(TAG, "Communication task stopped");
    vTaskDelete(NULL);
#endif
}
