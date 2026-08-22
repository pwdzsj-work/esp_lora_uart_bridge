#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/*
 * SCH_Schematic1_2026-08-22.pdf:
 *
 * UART0 is connected to CH340B and is reserved for flashing and logs.
 * UART1: TXD1=IO17, RXD1=IO16
 * UART2: TXD2=IO32, RXD2=IO33
 */
#define BOARD_UART1_PORT              UART_NUM_1
#define BOARD_UART1_TX_GPIO           GPIO_NUM_17
#define BOARD_UART1_RX_GPIO           GPIO_NUM_16
#define BOARD_UART1_BAUD_RATE         9600

#define BOARD_UART2_PORT              UART_NUM_2
#define BOARD_UART2_TX_GPIO           GPIO_NUM_32
#define BOARD_UART2_RX_GPIO           GPIO_NUM_33
#define BOARD_UART2_BAUD_RATE         9600

#define BOARD_UART_RX_BUFFER_SIZE     2048
#define BOARD_UART_TX_BUFFER_SIZE     2048
#define BOARD_UART_MAX_FRAME_SIZE     2048
#define BOARD_UART_EVENT_QUEUE_SIZE   64

/* One RX interrupt per byte allows a precise software 3.5-character timer. */
#define BOARD_UART_RX_FULL_THRESHOLD  1
#define BOARD_UART_FRAME_GAP_BITS     35
#define BOARD_UART_FRAME_GAP_US(baud) \
    ((BOARD_UART_FRAME_GAP_BITS * 1000000ULL + (baud) - 1) / (baud))

/* 1: UART1 <-> UART2; 0: only UART1 RX -> UART2 TX. */
#define BOARD_UART_BRIDGE_BIDIRECTIONAL 1
