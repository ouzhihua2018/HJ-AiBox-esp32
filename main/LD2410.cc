
#include "LD2410.h"

#define TAG "LD2410"


static void LD2410_wake_task(void *arg)
{   
   LD2410* this_ = (LD2410*) arg;
   while (true)
   {
     xEventGroupWaitBits(this_->LD2410_eventgroup_,LD2410_WAIT_BIT,true,true,portMAX_DELAY);
     vTaskDelay(pdMS_TO_TICKS(5000));
     if(gpio_get_level(this_->gpio_num_)==this_->active_level_) {
        ESP_LOGI(TAG,"wake!");
        this_->on_wake_word_detected_(this_->wake_word_);
    } else {
        ESP_LOGI(TAG,"human move, wake fall");
    }
   }
}

static void IRAM_ATTR LD2410_gpio_isr(void* arg){
    LD2410* this_ = (LD2410*) arg;
    int level = gpio_get_level(this_->gpio_num_);
    
    if(level == this_->active_level_) {
        xEventGroupSetBitsFromISR(this_->LD2410_eventgroup_, LD2410_WAIT_BIT, NULL);
    }
}

void LD2410::set_wake_callback(std::function<void(const std::string &wake_word)> on_wake_word_detected)
{
    on_wake_word_detected_ = on_wake_word_detected;
}

LD2410::LD2410(uint32_t active_level, gpio_num_t gpio_num) : active_level_(active_level), gpio_num_(gpio_num)
{   
    ESP_LOGI(TAG,"init GPIO%d", gpio_num_);
    LD2410_eventgroup_  = xEventGroupCreate();
    // GPIO 配置
    gpio_config_t io_conf = {}; //zero-initialize the config structure.
    io_conf.intr_type = GPIO_INTR_POSEDGE; // 上升沿产生中断
    io_conf.pin_bit_mask = (1ULL << gpio_num_); // 使用构造函数传入的 GPIO 编号
    io_conf.mode = GPIO_MODE_INPUT; //set as input mode
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;   //设置LD2410无人时输出低电平
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
     // 先安装 GPIO 中断服务
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    // 添加中断处理函数
    gpio_isr_handler_add(gpio_num_, LD2410_gpio_isr, this);
    
  
    xTaskCreate(LD2410_wake_task,
                            "wake_task", /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
                            2048,
                            this,
                            2,
                            &wake_task_handle_);
}

LD2410::~LD2410()
{
    gpio_isr_handler_remove(gpio_num_);
    vEventGroupDelete(LD2410_eventgroup_);
    vTaskDelete(wake_task_handle_);
}

