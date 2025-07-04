#ifndef __ASR_PRO_H__
#define __ASR_PRO_H__

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include <functional>
#include <string>


#define ASR_DEFAULT_GPIO GPIO_NUM_1
#define ESP_INTR_FLAG_DEFAULT 0
#define ASR_WAIT_BIT BIT0
    class asr_pro
    {
    public:
        uint32_t active_level_  ; 
        EventGroupHandle_t asr_eventgroup_;
        gpio_num_t gpio_num_;
        std::string wake_word_ = "wn9_nihaoxiaozhi_tts";
        void set_wake_callback(std::function<void (const std::string &wake_word)> callback);
        std::function<void (const std::string& wake_word)> on_wake_word_detected_;
         asr_pro(uint32_t active_level = 1,gpio_num_t gpio_num = ASR_DEFAULT_GPIO);
        ~asr_pro();
    private:
        TaskHandle_t wake_task_handle_;
        
    };
    
   
    
    
#endif