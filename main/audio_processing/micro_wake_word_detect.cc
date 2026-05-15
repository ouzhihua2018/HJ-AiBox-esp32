#include "micro_wake_word_detect.h"

#include "application.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "micro_wake_word_detect"

void MicroWakeWordDetect::PeriodTimerCallback(void* arg) {
    auto* self = static_cast<MicroWakeWordDetect*>(arg);
    if (self->detect_task_handle_ != nullptr) {
        xTaskNotifyGive(self->detect_task_handle_);
    }
}

MicroWakeWordDetect::MicroWakeWordDetect() {
    event_group_ = xEventGroupCreate();
}

MicroWakeWordDetect::~MicroWakeWordDetect() {
    if (period_timer_handle_ != nullptr) {
        esp_timer_stop(period_timer_handle_);
        esp_timer_delete(period_timer_handle_);
        period_timer_handle_ = nullptr;
    }
    if (detect_task_handle_ != nullptr) {
        vTaskDelete(detect_task_handle_);
        detect_task_handle_ = nullptr;
    }
    if (detect_task_stack_ != nullptr) {
        heap_caps_free(detect_task_stack_);
        detect_task_stack_ = nullptr;
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void MicroWakeWordDetect::InitializeWakeWordDetect() {
    const uint8_t* model = stream_state_internal_quant_tflite;
    //const uint8_t* model2 = stream_state_internal_quant_tflite2;
    wakeWord_.add_wake_word_model(model, 0.99f, 3, "xiaolexiaole", 34000);
    //wakeWord_.add_wake_word_model(model2, 0.99f, 3, "xiaolaxiaola", 34000);
    wakeWord_.set_features_step_size(4);
    wakeWord_.add_detection_callback(std::move(callback_));
    wakeWord_.setup();

    if (detect_task_stack_ == nullptr) {
        detect_task_stack_ = (StackType_t*)heap_caps_malloc(
            MICRO_WW_TASK_STACK_WORDS * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    }
    if (detect_task_stack_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate micro wake task stack in PSRAM");
        return;
    }

    detect_task_handle_ = xTaskCreateStaticPinnedToCore(
        [](void* arg) {
            auto this_ = (MicroWakeWordDetect*)arg;
            this_->DetectionTask();
            vTaskDelete(nullptr);
        },
        "micro_wake_task", MICRO_WW_TASK_STACK_WORDS, this, MICRO_WW_TASK_PRIORITY,
        detect_task_stack_, &detect_task_buffer_, MICRO_WW_TASK_CORE);

    if (detect_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create micro wake task");
        return;
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &MicroWakeWordDetect::PeriodTimerCallback;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "micro_ww_tick";
    timer_args.skip_unhandled_events = true;
    esp_err_t err = esp_timer_create(&timer_args, &period_timer_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
        "Initialized (timer-driven loop every %d ms; FreeRTOS tick=%d Hz -> do not use vTaskDelay for sub-10ms)",
        MICRO_WW_LOOP_PERIOD_MS, CONFIG_FREERTOS_HZ);
}

void MicroWakeWordDetect::StartDetection() {
    // Idle 里会 StartDetection；Application 里还有一次延迟 StartDetection（如 5s 定时器）。
    // 若重复调用 start()，旧实现会在 state!=IDLE 时仍先执行 load_models_()，损坏前端状态并崩溃。
    if ((xEventGroupGetBits(event_group_) & MICRO_WW_TASK_RUNNING_BIT) != 0 &&
        wakeWord_.is_running()) {
        ESP_LOGD(TAG, "StartDetection: already armed, skip duplicate start");
        if (period_timer_handle_ != nullptr) {
            const uint64_t period_us = (uint64_t)MICRO_WW_LOOP_PERIOD_MS * 1000ULL;
            esp_timer_stop(period_timer_handle_);
            esp_err_t err = esp_timer_start_periodic(period_timer_handle_, period_us);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    wakeWord_.start();
    xEventGroupSetBits(event_group_, MICRO_WW_TASK_RUNNING_BIT);
    if (period_timer_handle_ != nullptr) {
        const uint64_t period_us = (uint64_t)MICRO_WW_LOOP_PERIOD_MS * 1000ULL;
        esp_timer_stop(period_timer_handle_);
        esp_err_t err = esp_timer_start_periodic(period_timer_handle_, period_us);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        }
    }
}

void MicroWakeWordDetect::Stop() {
    if (period_timer_handle_ != nullptr) {
        esp_timer_stop(period_timer_handle_);
    }
    {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        wakeWord_.stop();
        // stop() 只把状态推到 STOP_MICROPHONE；必须再跑 loop() 才能经 STOPPING_MICROPHONE 回到 IDLE。
        // 若此处立刻 ClearBits，检测任务可能在 Take 返回后因 RUNNING=0 直接 break，从未调用 loop()，
        // 状态会永久卡在非 IDLE，start() 会报 "Wake word is already running"（按键等快路径更易触发）。
        const int k_max_stop_drain = 16;
        for (int i = 0; i < k_max_stop_drain && wakeWord_.is_running(); ++i) {
            wakeWord_.loop();
        }
        if (wakeWord_.is_running()) {
            ESP_LOGW(TAG, "Stop: state machine did not reach IDLE after drain");
        }
    }
    xEventGroupClearBits(event_group_, MICRO_WW_TASK_RUNNING_BIT);
    if (detect_task_handle_ != nullptr) {
        xTaskNotifyGive(detect_task_handle_);
    }
    ESP_LOGW(TAG, "Stop");
}

void MicroWakeWordDetect::Feed(const std::vector<int16_t>& data) {
    if (data.empty() || wakeWord_.ring_buffer_ == nullptr) {
        return;
    }
    wakeWord_.feed(data);
}

size_t MicroWakeWordDetect::FreeSize() {
    return wakeWord_.free_ring_buffer();
}

bool MicroWakeWordDetect::IsRunning() {
    return wakeWord_.is_running();
}

void MicroWakeWordDetect::DetectionTask() {
    TickType_t last_stack_log_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "Micro wake task started, prio=%d core=%d (loop tick from esp_timer, %d ms)",
        MICRO_WW_TASK_PRIORITY, MICRO_WW_TASK_CORE, MICRO_WW_LOOP_PERIOD_MS);

    while (true) {  
        xEventGroupWaitBits(
            event_group_, MICRO_WW_TASK_RUNNING_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        while (xEventGroupGetBits(event_group_) & MICRO_WW_TASK_RUNNING_BIT) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if ((xEventGroupGetBits(event_group_) & MICRO_WW_TASK_RUNNING_BIT) == 0) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(loop_mutex_);
                wakeWord_.loop();
            }

            TickType_t now = xTaskGetTickCount();
            if (now - last_stack_log_tick >= pdMS_TO_TICKS(10000)) {
                UBaseType_t high_water = uxTaskGetStackHighWaterMark(nullptr);
                ESP_LOGI(TAG, "micro_wake_task stack high water: %u words", (unsigned)high_water);
                last_stack_log_tick = now;
            }
        }
    }
}
