#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void*);
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f,const char*n,uint32_t s,void*a,uint32_t pr,TaskHandle_t*h,BaseType_t c){ (void)f;(void)n;(void)s;(void)a;(void)pr;(void)h;(void)c; return pdPASS; }
static inline void vTaskDelay(TickType_t t){ (void)t; }
