#include "uart_bridge.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "APP";

void app_main(void)
{
    ESP_LOGI(TAG, "UART transparent bridge starting");
    ESP_ERROR_CHECK(uart_bridge_start());
    ESP_LOGI(TAG, "UART bridge initialization complete");
}
