#include "micro_wake_word_detect.h"
#include "esp_log.h"

#define TAG "micro_wake_word_detect"
MicroWakeWordDetect::MicroWakeWordDetect()
{

}

MicroWakeWordDetect::~MicroWakeWordDetect()
{

}
bool gDetected = false;
void wakeWordDetected(std::string detected_wake_word) { gDetected = true; }
void MicroWakeWordDetect::InitializeWakeWordDetect()
{
    uint8_t *model = const_cast<uint8_t *>(stream_state_internal_quant_tflite);

    wakeWord_.add_wake_word_model(model, 0.95f,10, "fuwuyuan", 36000); //22940
    wakeWord_.set_features_step_size(10);
    wakeWord_.add_detection_callback(std::move(callback_));
    wakeWord_.setup();
    //wakeWord_.start();
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            MicroWakeWordDetect* this_ = (MicroWakeWordDetect*)arg;
            this_->wakeWord_.loop();
            if (this_->Detected_ ) {
              //printf("Wake word detected!\n");
              this_->Detected_  = false;
              // start listening again
              this_->wakeWord_.start();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "micro_wake_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &micro_timer_handle_);
    ESP_LOGI(TAG,"Initialized");
}

void MicroWakeWordDetect::StartDetection()
{   
    wakeWord_.start();
    vTaskDelay(pdMS_TO_TICKS(200)); //等待5ms再开启唤醒词定时器
    esp_timer_start_periodic(micro_timer_handle_,1000*3); 
    // wakeWord_.start();
    // xTaskCreatePinnedToCore([](void* arg){
    //     MicroWakeWordDetect* this_ = (MicroWakeWordDetect*) arg;
    //     for (;;) {
    //         this_->wakeWord_.loop();
    //         if (gDetected ) {
    //           printf("Wake word detected!\n");
    //           gDetected  = false;
    //           // start listening again
    //           this_->wakeWord_.start();
    //         }
    //         vTaskDelay(pdMS_TO_TICKS(8));
    //       }
    // },"micro_wake_task", /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
    // 4096,
    // this,
    // 3,
    // NULL,
    // 1);
}


// 模拟麦克风手动传音频帧到input_buffer，输入为256个样本
void MicroWakeWordDetect::Feed(std::vector<int16_t> &data)
{
    if (data.empty() || wakeWord_.ring_buffer_ == nullptr) {
        return;
    }

    wakeWord_.feed(data);
}

void MicroWakeWordDetect::StartAgain()
{
    Detected_ = true;
    ESP_LOGW(TAG,"StartAgain");
}

void MicroWakeWordDetect::Stop()
{
    wakeWord_.stop();
    ESP_LOGW(TAG,"Stop");
}

bool MicroWakeWordDetect::IsRunning()
{   
    return wakeWord_.is_running();
}
