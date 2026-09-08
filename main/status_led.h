#pragma once

#include "esp_err.h"

/** Initialize LED2 as the run indicator and LED3 as the LoRa indicator. */
esp_err_t status_led_init(void);

/** Extend the LED3 activity pulse after LoRa RX or TX activity. */
void status_led_note_lora_activity(void);
