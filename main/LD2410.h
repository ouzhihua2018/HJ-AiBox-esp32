#ifndef __LD2410_H__
#define __LD2410_H__

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include <functional>
#include <string>

#define LD2410_DEFAULT_GPIO (gpio_num_t)(CONFIG_LD2410_LEVEL_GPIO)
#define ESP_INTR_FLAG_DEFAULT 0
#define LD2410_WAIT_BIT BIT1


    class LD2410
    {
    public:
        uint32_t active_level_  ; 
        EventGroupHandle_t LD2410_eventgroup_;
        gpio_num_t gpio_num_;
        
        std::string wake_word_ = "wn9_nihaoxiaozhi_tts";
        void set_wake_callback(std::function<void (const std::string &wake_word)> callback);
        std::function<void (const std::string& wake_word)> on_wake_word_detected_;
        LD2410(uint32_t active_level = 1,gpio_num_t gpio_num = LD2410_DEFAULT_GPIO);
        ~LD2410();
    private:
        TaskHandle_t wake_task_handle_;

    };
    
   
    
    
#endif