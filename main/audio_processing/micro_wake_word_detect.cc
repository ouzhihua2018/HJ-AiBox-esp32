#include "micro_wake_word_detect.h"
#include "esp_log.h"

#define TAG "micro_wake_word_detect"
MicroWakeWordDetect::MicroWakeWordDetect()
{

}

MicroWakeWordDetect::~MicroWakeWordDetect()
{

}

void MicroWakeWordDetect::InitializeWakeWordDetect()
{
    uint8_t *model = const_cast<uint8_t *>(stream_state_internal_quant_tflite);

    wakeWord_.add_wake_word_model(model, 0.99f,4, "xiaolexiaole", 30000); //22940
    wakeWord_.set_features_step_size(7);
    wakeWord_.add_detection_callback(std::move(callback_));
    wakeWord_.setup();
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            MicroWakeWordDetect* this_ = (MicroWakeWordDetect*)arg;
            this_->wakeWord_.loop();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "micro_wake_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &micro_timer_handle_);
    ESP_LOGI(TAG,"Initialized");
}
//这里我不希望loop空转，因此定时器与wakeWord状态同步
void MicroWakeWordDetect::StartDetection()
{   
    wakeWord_.start();
    vTaskDelay(pdMS_TO_TICKS(200)); //等待200ms再开启唤醒词定时器
    esp_timer_start_periodic(micro_timer_handle_,1000*1);  //1000*3    
}

void MicroWakeWordDetect::Stop()
{
    wakeWord_.stop();
    vTaskDelay(pdMS_TO_TICKS(200)); //等待200ms再开启唤醒词定时器
    esp_timer_stop(micro_timer_handle_);
    ESP_LOGW(TAG,"Stop");
}

// 模拟麦克风手动传音频帧到input_buffer，输入为256个样本   16ms
void MicroWakeWordDetect::Feed(std::vector<int16_t> &data)
{
    if (data.empty() || wakeWord_.ring_buffer_ == nullptr) {
        return;
    }

    wakeWord_.feed(data);
}

bool MicroWakeWordDetect::IsRunning()
{   
    return wakeWord_.is_running();
}
