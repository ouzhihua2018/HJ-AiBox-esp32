// 消除USE_ESP32宏重定义警告（解决编译告警）
#ifdef USE_ESP32
#undef USE_ESP32
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task.h"  // ESP32专属任务核心查询接口
#include <cstring>
#define TAG "Core0Monitor"
#define MONITOR_INTERVAL 1000  // 每秒打印一次Core 0任务状态

// 监控任务（固定跑在Core 1，不干扰Core 0）
static void core0_monitor_task(void *arg) {
    while (1) {
    char InfoBuffer[512] = {0};
    memset(InfoBuffer, 0, 512); // 信息缓冲区清零
    vTaskList((char *)&InfoBuffer);
    printf("任务名  任务状态  优先级  剩余栈  任务序号  cpu核\r\n");
    printf("\r\n%s\r\n", InfoBuffer);


    vTaskDelay((10000) / portTICK_PERIOD_MS);

    }
}

// 启动Core 0监控（仅需在app_main中调用此函数）
void start_core1_monitor() {
    // 创建监控任务，绑定到Core 1（避免干扰Core 0）
    xTaskCreate(
        core0_monitor_task,
        "core0_monitor",  // 任务名
        4096,             // 栈大小（足够查询任务状态）
        NULL,             // 无参数
        1,                // 最低优先级，不抢占其他任务
        NULL             // 不保存任务句柄
    );
}