#include "status_led.h"

#include "board_config.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

#define RUN_LED_TOGGLE_PERIOD_MS 250
#define LORA_LED_PULSE_MS         100
#define LED_TASK_POLL_MS          20

static TaskHandle_t s_led_task;

static void set_led(gpio_num_t gpio, bool on)
{
    int level = on ? BOARD_LED_ACTIVE_LEVEL : !BOARD_LED_ACTIVE_LEVEL;
    (void)gpio_set_level(gpio, level);
}

static void status_led_task(void *argument)
{
    (void)argument;

    bool run_led_on = false;
    bool lora_led_on = false;
    TickType_t run_led_changed_at = xTaskGetTickCount();
    TickType_t lora_activity_at = 0;
    const TickType_t run_period = pdMS_TO_TICKS(RUN_LED_TOGGLE_PERIOD_MS);
    const TickType_t lora_pulse = pdMS_TO_TICKS(LORA_LED_PULSE_MS);

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LED_TASK_POLL_MS)) > 0) {
            lora_activity_at = xTaskGetTickCount();
            if (!lora_led_on) {
                lora_led_on = true;
                set_led(BOARD_LORA_LED_GPIO, true);
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - run_led_changed_at) >= run_period) {
            run_led_changed_at = now;
            run_led_on = !run_led_on;
            set_led(BOARD_RUN_LED_GPIO, run_led_on);
        }

        if (lora_led_on && (now - lora_activity_at) >= lora_pulse) {
            lora_led_on = false;
            set_led(BOARD_LORA_LED_GPIO, false);
        }
    }
}

esp_err_t status_led_init(void)
{
    if (s_led_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << BOARD_RUN_LED_GPIO) |
                        (1ULL << BOARD_LORA_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    set_led(BOARD_RUN_LED_GPIO, false);
    set_led(BOARD_LORA_LED_GPIO, false);

    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 2,
                    &s_led_task) != pdPASS) {
        s_led_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void status_led_note_lora_activity(void)
{
    if (s_led_task != NULL) {
        xTaskNotifyGive(s_led_task);
    }
}
