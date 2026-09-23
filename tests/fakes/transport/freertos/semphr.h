#pragma once
#include <pthread.h>
#include <stdint.h>
typedef struct semaphore_fake *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,uint32_t);
void vSemaphoreDelete(SemaphoreHandle_t);
