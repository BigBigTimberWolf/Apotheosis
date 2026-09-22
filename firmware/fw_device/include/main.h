#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include "handleCommands.h"
#include "InitSettings.h"
#include "USBSetup.h"
#include "usb_desc.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Task handles (extern, 定义在 tasks.cpp)
extern TaskHandle_t serial1TaskHandle;
extern TaskHandle_t serial0TaskHandle;
extern TaskHandle_t mouseMoveTaskHandle;
extern TaskHandle_t ledFlashTaskHandle;

// Task / ISR / helper prototypes
void serial0Task(void *pvParameters);
void serial1Task(void *pvParameters);
void mouseMoveTask(void *pvParameters);
void ledFlashTask(void *pvParameters);
void IRAM_ATTR serial0ISR();
void IRAM_ATTR serial1ISR();

void tasks();
void serial0RX();
void serial1RX();
void requestUSBDescriptors();

#endif
