#ifndef MICRO_WAKE_WORD_DETECT_H
#define MICRO_WAKE_WORD_DETECT_H

#pragma once

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "esphome/components/i2s_audio/microphone/i2s_audio_microphone.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#include "model.h"

// 独立任务栈（可放 PSRAM）；推理由 esp_timer 按微秒周期唤醒，避免 FREERTOS_HZ=100 时 vTaskDelay 最小 10ms。
#define MICRO_WW_TASK_RUNNING_BIT BIT0
#define MICRO_WW_TASK_STACK_WORDS (4096)
#define MICRO_WW_TASK_PRIORITY 6
#define MICRO_WW_TASK_CORE 1
#define MICRO_WW_LOOP_PERIOD_MS 4

class MicroWakeWordDetect {
public:
    MicroWakeWordDetect();
    ~MicroWakeWordDetect();
    void InitializeWakeWordDetect();
    void OnWakeWordDetected(std::function<void(std::string wake_word)> callback) { callback_ = callback; }
    void StartDetection();
    void Feed(const std::vector<int16_t>& data);
    size_t FreeSize();
    void Stop();
    bool IsRunning();
    esphome::micro_wake_word::MicroWakeWord wakeWord_;

private:
    void DetectionTask();
    static void PeriodTimerCallback(void* arg);

    std::function<void(std::string wake_word)> callback_;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t detect_task_handle_ = nullptr;
    StaticTask_t detect_task_buffer_;
    StackType_t* detect_task_stack_ = nullptr;
    esp_timer_handle_t period_timer_handle_ = nullptr;
    std::mutex loop_mutex_;
};

#endif
