#include "uart_bridge.h"
#include "status_led.h"
#include "maintenance_portal.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>

static const char *TAG = "APP";

#define HEARTBEAT_PERIOD_MS 5000

static void heartbeat_task(void *argument)
{
    (void)argument;

    while (true) {
        int64_t uptime_seconds = esp_timer_get_time() / 1000000;
        ESP_LOGI(TAG,
                 "Heartbeat: program running, uptime=%" PRIi64
                 " s, free heap=%" PRIu32 " bytes",
                 uptime_seconds, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(maintenance_portal_init());

    BaseType_t task_result = xTaskCreate(heartbeat_task, "heartbeat", 2048,
                                         NULL, 1, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "UART transparent bridge starting");
    esp_err_t err = uart_bridge_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART bridge initialization failed: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "UART bridge initialization complete");
}
