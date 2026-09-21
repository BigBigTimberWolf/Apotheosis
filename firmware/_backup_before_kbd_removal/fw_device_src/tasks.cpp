#include "main.h"
#include "handleCommands.h"

#define NOTIFY_SERIAL0 1
#define NOTIFY_SERIAL1 2

TaskHandle_t serial1TaskHandle = NULL;
TaskHandle_t serial0TaskHandle = NULL;
TaskHandle_t mouseMoveTaskHandle = NULL;

void tasks() {
    BaseType_t xReturned;

    // Core 1 专享分配: 高速串口任务与移动任务全部绑定 Core 1，与 Core 0 (USB/系统) 隔离，杜绝跨核调度抖动
    // 优先级: mouseMoveTask=7 > serial0/1Task=6 > ClickTick=2，收到数据即抢占
    xReturned = xTaskCreatePinnedToCore(serial1Task, "Serial1Task", 4096, NULL, 6, &serial1TaskHandle, 1);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create Serial1Task");
    }

    xReturned = xTaskCreatePinnedToCore(serial0Task, "Serial0Task", 3072, NULL, 6, &serial0TaskHandle, 1);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create Serial0Task");
    }

    xReturned = xTaskCreatePinnedToCore(mouseMoveTask, "MouseMoveTask", 2048, NULL, 7, &mouseMoveTaskHandle, 1);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create MouseMoveTask");
    }

    // 独立轻量定时任务: 处理鼠标CLICK定时弹起(如果使用了0x12 CLICK命令),
    // 以及按键状态周期性重申看门狗 buttonWatchdogTick()。
    //
    // 说明: configTICK_RATE_HZ=1000, 所以原来的 vTaskDelay(pdMS_TO_TICKS(1)) 本来就是 1ms 节拍,
    // 并不是"空转刷调度器"。这里改用 vTaskDelayUntil 让周期不受 clickTick() 执行时间累积漂移,
    // 并把栈提到 2560 —— 本任务现在会走到 Mouse.press/release(TinyUSB SendReport), 需要余量。
    xReturned = xTaskCreatePinnedToCore([](void *) {
        TickType_t last = xTaskGetTickCount();
        for (;;) {
            clickTick();
            vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
        }
    }, "ClickTick", 2560, NULL, 2, NULL, 0);
    if (xReturned != pdPASS) {
        Serial0.println("Failed to create ClickTick");
    }
}

void IRAM_ATTR serial0ISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(serial0TaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void IRAM_ATTR serial1ISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(serial1TaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void serial0Task(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        serial0RX();
    }
}

void serial1Task(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        serial1RX();
    }
}
