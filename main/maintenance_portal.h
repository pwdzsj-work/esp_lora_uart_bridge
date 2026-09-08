#pragma once

#include "esp_err.h"

/** Monitor U4 and start the AP/web/OTA portal after a three-second press. */
esp_err_t maintenance_portal_init(void);
