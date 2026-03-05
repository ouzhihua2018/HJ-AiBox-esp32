#include "micro_wake_word_detect.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "application.h"
#define TAG "micro_wake_word_detect"
MicroWakeWordDetect::MicroWakeWordDetect()
{
    // 初始化互斥锁
  
}

MicroWakeWordDetect::~MicroWakeWordDetect()
{
    // 删除互斥锁
   
}

void MicroWakeWordDetect::InitializeWakeWordDetect()
{
    uint8_t *model = const_cast<uint8_t *>(stream_state_internal_quant_tflite);
    // feed一次256个样本 
    wakeWord_.add_wake_word_model(model, 0.94f,3, "xiaolexiaole", 30000); //22940
    wakeWord_.set_features_step_size(4);    
    wakeWord_.add_detection_callback(std::move(callback_));
    wakeWord_.setup();
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            MicroWakeWordDetect* this_ = (MicroWakeWordDetect*)arg;
            // 在定时器回调中也使用互斥锁保护
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
    vTaskDelay(pdMS_TO_TICKS(500)); //等待200ms再开启唤醒词定时器
    Application &app = Application::GetInstance();
    
    esp_timer_start_periodic(micro_timer_handle_,1000*3);  //1000*3   
    
    // if(kDeviceStateSpeaking == app.GetDeviceState()){
    //     ESP_LOGI(TAG,"Speaking状态，慢处理唤醒词");
    //     esp_timer_start_periodic(micro_timer_handle_,1000*60);  //1000*3   
    // }else{
    //     esp_timer_start_periodic(micro_timer_handle_,1000*1);  //1000*3   
    // }
}
    
void MicroWakeWordDetect::Stop()
{
    wakeWord_.stop();
    vTaskDelay(pdMS_TO_TICKS(500)); //等待200ms再开启唤醒词定时器
    esp_timer_stop(micro_timer_handle_);
    ESP_LOGW(TAG,"Stop");
}

// 模拟麦克风手动传音频帧到input_buffer，输入为256个样本   16ms
void MicroWakeWordDetect::Feed(const std::vector<int16_t> &data)
{   
    if (data.empty() || wakeWord_.ring_buffer_ == nullptr) {
        return;
    }

    // 在写入数据时使用互斥锁保护
   
        wakeWord_.feed(data);
    
}
size_t MicroWakeWordDetect::FreeSize()
{
    size_t free_size = 0;
    // 在查询大小时使用互斥锁保护
    
    free_size = wakeWord_.free_ring_buffer();
     
    return free_size;
}
bool MicroWakeWordDetect::IsRunning()
{   
    bool is_running = false;
    // 在查询运行状态时使用互斥锁保护
    
        is_running = wakeWord_.is_running();
    
    return is_running;
}