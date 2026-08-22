#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure UART1/UART2 and start the transparent forwarding tasks. */
esp_err_t uart_bridge_start(void);

#ifdef __cplusplus
}
#endif
