#include "uart_bridge.h"

#include "board_config.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uart_port_t source;
    uart_port_t destination;
    QueueHandle_t *source_queue;
    uint64_t frame_gap_us;
    const char *task_name;
} forward_task_config_t;

static const char *TAG = "UART_BRIDGE";
#if BOARD_UART_DATA_LOG_ENABLE
static const char *UART2_RX_TAG = "UART2_RX";
static const char *UART2_TX_TAG = "UART2_TX";
static const char *LORA_TX_TAG = "LORA_TX";
#endif
static bool s_started;
static QueueHandle_t s_uart1_queue;
static QueueHandle_t s_uart2_queue;

static const forward_task_config_t s_uart1_to_uart2 = {
    .source = BOARD_UART1_PORT,
    .destination = BOARD_UART2_PORT,
    .source_queue = &s_uart1_queue,
    .frame_gap_us = BOARD_UART_FRAME_GAP_US(BOARD_UART1_BAUD_RATE),
    .task_name = "uart1_to_uart2",
};

#if BOARD_UART_BRIDGE_BIDIRECTIONAL
static const forward_task_config_t s_uart2_to_uart1 = {
    .source = BOARD_UART2_PORT,
    .destination = BOARD_UART1_PORT,
    .source_queue = &s_uart2_queue,
    .frame_gap_us = BOARD_UART_FRAME_GAP_US(BOARD_UART2_BAUD_RATE),
    .task_name = "uart2_to_uart1",
};
#endif

static esp_err_t configure_uart(uart_port_t port, int baud_rate,
                                gpio_num_t tx_gpio, gpio_num_t rx_gpio,
                                QueueHandle_t *event_queue)
{
    const uart_config_t config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(port, &config), TAG,
                        "UART%d parameter configuration failed", port);
    ESP_RETURN_ON_ERROR(
        uart_set_pin(port, tx_gpio, rx_gpio,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG, "UART%d pin configuration failed", port);
    ESP_RETURN_ON_ERROR(
        uart_driver_install(port, BOARD_UART_RX_BUFFER_SIZE,
                            BOARD_UART_TX_BUFFER_SIZE,
                            BOARD_UART_EVENT_QUEUE_SIZE, event_queue, 0),
        TAG, "UART%d driver installation failed", port);

    ESP_RETURN_ON_ERROR(
        uart_set_rx_full_threshold(port, BOARD_UART_RX_FULL_THRESHOLD),
        TAG, "UART%d RX threshold configuration failed", port);
    ESP_RETURN_ON_ERROR(
        uart_set_rx_timeout(port, 0),
        TAG, "UART%d RX timeout configuration failed", port);

    ESP_LOGI(TAG, "UART%d ready: TX=GPIO%d RX=GPIO%d, %d 8N1",
             port, tx_gpio, rx_gpio, baud_rate);
    return ESP_OK;
}

static void forward_frame(const forward_task_config_t *config,
                          const uint8_t *data, size_t length)
{
        int written = uart_write_bytes(config->destination, data, length);

#if BOARD_UART_DATA_LOG_ENABLE
        /* Keep optional data logging after uart_write_bytes(). */
        if (config->source == BOARD_UART2_PORT) {
            ESP_LOGI(UART2_RX_TAG, "GPIO%d received %d byte(s)",
                     BOARD_UART2_RX_GPIO, (int)length);
            ESP_LOG_BUFFER_HEX_LEVEL(UART2_RX_TAG, data, length,
                                     ESP_LOG_INFO);
        }
#endif
        if (written != (int)length) {
            ESP_LOGE(TAG,
                     "UART%d -> UART%d forwarding failed: read=%d written=%d",
                     config->source, config->destination, (int)length, written);
            return;
        }

        esp_err_t tx_result = uart_wait_tx_done(config->destination,
                                                pdMS_TO_TICKS(1000));
        if (tx_result != ESP_OK) {
            ESP_LOGE(TAG, "UART%d TX completion timeout: %s",
                     config->destination, esp_err_to_name(tx_result));
            return;
        }

#if BOARD_UART_DATA_LOG_ENABLE
        if (config->destination == BOARD_UART1_PORT) {
            ESP_LOGI(LORA_TX_TAG,
                     "GPIO%d TX completed: %d byte(s) from GPIO%d RX",
                     BOARD_UART1_TX_GPIO, written, BOARD_UART2_RX_GPIO);
            ESP_LOG_BUFFER_HEX_LEVEL(LORA_TX_TAG, data, written,
                                     ESP_LOG_INFO);
        } else if (config->destination == BOARD_UART2_PORT) {
            ESP_LOGI(UART2_TX_TAG, "GPIO%d TX completed: %d byte(s)",
                     BOARD_UART2_TX_GPIO, written);
            ESP_LOG_BUFFER_HEX_LEVEL(UART2_TX_TAG, data, written,
                                     ESP_LOG_INFO);
        }
#endif
}

static void frame_gap_timer_callback(void *argument)
{
    QueueHandle_t *queue = argument;
    const uart_event_t frame_end_event = {
        .type = UART_EVENT_MAX,
    };

    (void)xQueueSend(*queue, &frame_end_event, 0);
}

static void forward_task(void *argument)
{
    const forward_task_config_t *config = argument;
    uint8_t *frame = malloc(BOARD_UART_MAX_FRAME_SIZE);
    size_t frame_length = 0;
    esp_timer_handle_t frame_gap_timer = NULL;

    if (frame == NULL) {
        ESP_LOGE(TAG, "UART%d frame buffer allocation failed", config->source);
        vTaskDelete(NULL);
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = frame_gap_timer_callback,
        .arg = config->source_queue,
        .dispatch_method = ESP_TIMER_TASK,
        .name = config->task_name,
    };
    if (esp_timer_create(&timer_args, &frame_gap_timer) != ESP_OK) {
        ESP_LOGE(TAG, "UART%d frame timer creation failed", config->source);
        free(frame);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Forwarding UART%d RX -> UART%d TX, frame gap=%llu us",
             config->source, config->destination,
             config->frame_gap_us);

    while (true) {
        uart_event_t event;
        if (xQueueReceive(*config->source_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining > 0) {
                size_t capacity = BOARD_UART_MAX_FRAME_SIZE - frame_length;
                size_t to_read = remaining < capacity ? remaining : capacity;
                int received = uart_read_bytes(config->source,
                                               frame + frame_length,
                                               to_read, 0);
                if (received <= 0) {
                    ESP_LOGE(TAG, "UART%d read failed", config->source);
                    break;
                }

                frame_length += received;
                remaining -= received;

                if (frame_length == BOARD_UART_MAX_FRAME_SIZE) {
                    ESP_LOGW(TAG,
                             "UART%d frame exceeds %d bytes; forwarding partial frame",
                             config->source, BOARD_UART_MAX_FRAME_SIZE);
                    forward_frame(config, frame, frame_length);
                    frame_length = 0;
                }
            }

            (void)esp_timer_stop(frame_gap_timer);
            esp_err_t timer_result = esp_timer_start_once(frame_gap_timer,
                                                          config->frame_gap_us);
            if (timer_result != ESP_OK) {
                ESP_LOGE(TAG, "UART%d frame timer start failed: %s",
                         config->source, esp_err_to_name(timer_result));
            }
        } else if (event.type == UART_EVENT_MAX) {
            if (frame_length > 0) {
                forward_frame(config, frame, frame_length);
                frame_length = 0;
            }
        } else if (event.type == UART_FIFO_OVF ||
                   event.type == UART_BUFFER_FULL) {
            ESP_LOGE(TAG, "UART%d RX overflow; discarding incomplete frame",
                     config->source);
            uart_flush_input(config->source);
            xQueueReset(*config->source_queue);
            frame_length = 0;
        } else if (event.type == UART_FRAME_ERR ||
                   event.type == UART_PARITY_ERR) {
            ESP_LOGE(TAG, "UART%d RX line error; discarding incomplete frame",
                     config->source);
            frame_length = 0;
        }
    }
}

esp_err_t uart_bridge_start(void)
{
    TaskHandle_t uart1_to_uart2_task = NULL;

    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        configure_uart(BOARD_UART1_PORT, BOARD_UART1_BAUD_RATE,
                       BOARD_UART1_TX_GPIO, BOARD_UART1_RX_GPIO,
                       &s_uart1_queue),
        TAG, "UART1 initialization failed");

    esp_err_t err = configure_uart(BOARD_UART2_PORT, BOARD_UART2_BAUD_RATE,
                                   BOARD_UART2_TX_GPIO, BOARD_UART2_RX_GPIO,
                                   &s_uart2_queue);
    if (err != ESP_OK) {
        uart_driver_delete(BOARD_UART1_PORT);
        return err;
    }

    if (xTaskCreate(forward_task, s_uart1_to_uart2.task_name, 3072,
                    (void *)&s_uart1_to_uart2, 10,
                    &uart1_to_uart2_task) != pdPASS) {
        uart_driver_delete(BOARD_UART2_PORT);
        uart_driver_delete(BOARD_UART1_PORT);
        return ESP_ERR_NO_MEM;
    }

#if BOARD_UART_BRIDGE_BIDIRECTIONAL
    if (xTaskCreate(forward_task, s_uart2_to_uart1.task_name, 3072,
                    (void *)&s_uart2_to_uart1, 10, NULL) != pdPASS) {
        vTaskDelete(uart1_to_uart2_task);
        uart_driver_delete(BOARD_UART2_PORT);
        uart_driver_delete(BOARD_UART1_PORT);
        return ESP_ERR_NO_MEM;
    }
#endif

    s_started = true;
    return ESP_OK;
}
