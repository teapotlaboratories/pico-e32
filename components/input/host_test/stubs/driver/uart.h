#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define UART_NUM_0 0
typedef int uart_port_t;
static inline bool uart_is_driver_installed(uart_port_t p){ (void)p; return true; }
static inline esp_err_t uart_driver_install(uart_port_t p,int rx,int tx,int q,void*qh,int f){ (void)p;(void)rx;(void)tx;(void)q;(void)qh;(void)f; return ESP_OK; }
static inline int uart_read_bytes(uart_port_t p,void*b,uint32_t l,uint32_t t){ (void)p;(void)b;(void)l;(void)t; return 0; }
