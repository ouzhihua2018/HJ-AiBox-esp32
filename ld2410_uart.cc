#include "ld2410_uart.h"
#include "application.h"
#include <string.h>
#define TAG "ld2410"
static void uart_event_task(void *arg)
{
    ld2410 *ld2410_obj = (ld2410 *)arg;
    uart_event_t event;
    while (1)
    {
        if (pdTRUE==xQueueReceive(ld2410_obj->uart_event_queue_, &event, portMAX_DELAY)) //阻塞等待
        {
            switch (event.type)
            {
                case UART_DATA:
                    ESP_LOGW(TAG, "UART_DATA_EVENT");
                    xEventGroupSetBits(ld2410_obj->uart_group_, LD2410_UART_DATA_AVAILABLE_EVENT);
                    break;
                case UART_BREAK:
                    ESP_LOGI(TAG, "UART_BREAK_EVENT");
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "UART_BUFFER_FULL_EVENT");
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "UART_FIFO_OVF_EVENT");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGI(TAG, "UART_FRAME_ERR_EVENT");
                default:
                    ESP_LOGI(TAG, "UART_EVENT_UNKNOWN");
                    break;
            }
        }
    }
}

static void uart_data_task(void *arg)
{
    ld2410 *ld2410_obj = (ld2410 *)arg;
    size_t len;
    while (1)
    {   
        xEventGroupWaitBits(ld2410_obj->uart_group_, LD2410_UART_DATA_AVAILABLE_EVENT, pdTRUE, pdTRUE,portMAX_DELAY);
        
        uart_get_buffered_data_len(ld2410_obj->uart_port_num_, &len);
        if (len > 0)
        {   
            uart_read_bytes(ld2410_obj->uart_port_num_, ld2410_obj->Rx_buffer_, len, portMAX_DELAY);
            ESP_LOGW(TAG, "UART_DATA_RECEIVED: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",ld2410_obj->Rx_buffer_[0],ld2410_obj->Rx_buffer_[1]
                ,ld2410_obj->Rx_buffer_[2],ld2410_obj->Rx_buffer_[3],ld2410_obj->Rx_buffer_[4],ld2410_obj->Rx_buffer_[5]
                ,ld2410_obj->Rx_buffer_[6],ld2410_obj->Rx_buffer_[7],ld2410_obj->Rx_buffer_[8],ld2410_obj->Rx_buffer_[9]);
        
            if(ld2410_obj->Rx_buffer_[0]  == 0x6E && ld2410_obj->Rx_buffer_[4] == 0x62)
            {   
                if(ld2410_obj->config_status_) continue;; //配置写入过程上报数据不处理
                ESP_LOGI(TAG, "Report data");
                Application& app = Application::GetInstance();
                if(app.GetDeviceState() == kDeviceStateIdle 
                && (ld2410_obj->Rx_buffer_[1] == 0x02 || ld2410_obj->Rx_buffer_[1] == 0x03))
                {
                    ESP_LOGI(TAG, "ld2410 wake up");
                    
                    if ((ld2410_obj->on_wake_word_detected_))  ld2410_obj->on_wake_word_detected_(ld2410_obj->wake_word_);
                }
                // 连续10次读取无人状态判断为人走，结束对话
                if(app.GetDeviceState() == kDeviceStateListening 
                && (ld2410_obj->Rx_buffer_[1] == 0x00 || ld2410_obj->Rx_buffer_[1] == 0x01))
                {
                    ld2410_obj->No_Presence_times_++;
                    if(ld2410_obj->No_Presence_times_>=5)
                    {   
                       ESP_LOGI(TAG, "human move,End the conversation");
                       app.ToggleChatState();
                       ld2410_obj->No_Presence_times_ = 0;
                    }
                }
            }  else if(ld2410_obj->Rx_buffer_[6]  == 0xFF && ld2410_obj->Rx_buffer_[7] == 0x01){
                if(ld2410_obj->Rx_buffer_[8] == 0x00 && ld2410_obj->Rx_buffer_[9] == 0x00 )
                    xEventGroupSetBits(ld2410_obj->uart_group_, LD2410_CONFIG_ENABLE_SENDBACK);
                else 
                    ESP_LOGE(TAG, "配置使能失败");
            } else if(ld2410_obj->Rx_buffer_[6]  == 0x70 && ld2410_obj->Rx_buffer_[7] == 0x01){
                if(ld2410_obj->Rx_buffer_[8] == 0x00 && ld2410_obj->Rx_buffer_[9] == 0x00 ){
                   
                    xEventGroupSetBits(ld2410_obj->uart_group_, LD2410_WRITE_PARAMETER_SENDBACK);
                }
                else 
                    ESP_LOGE(TAG, "参数写入失败");
            }
            
        }
    }
}

ld2410::ld2410(int baud_rate,uart_port_t uart_port_num, gpio_num_t uart_tx_pin, gpio_num_t uart_rx_pin)
:baud_rate_(baud_rate), uart_port_num_(uart_port_num), uart_tx_pin_(uart_tx_pin), uart_rx_pin_(uart_rx_pin)
{   
    uart_event_queue_ = xQueueCreate(LD2410_UART_EVENT_QUEUE_SIZE, sizeof(uart_event_t));
    uart_group_ = xEventGroupCreate();
    //init uart
    uart_config_t uart_config = {
        .baud_rate = baud_rate_,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    uart_driver_install(uart_port_num_, LD2410_UART_RX_BUFFER_SIZE,LD2410_UART_TX_BUFFER_SIZE, LD2410_UART_EVENT_QUEUE_SIZE, &uart_event_queue_, 0);

    uart_param_config(uart_port_num_, &uart_config);

    uart_set_pin(uart_port_num_, uart_tx_pin_, uart_rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    //xTaskCreate(uart_event_task, "uart_event_task", 2048, this, 10, &event_task_handle_);
    xTaskCreatePinnedToCore(uart_event_task, "uart_event_task", 2048, this, 10, &event_task_handle_,1);
    
    xTaskCreate(uart_data_task, "uart_data_task", 2048, this, 10, &data_task_handle_);
}

ld2410::~ld2410()
{
    uart_driver_delete(uart_port_num_);
    vQueueDelete(uart_event_queue_);
    vEventGroupDelete(uart_group_);
    vTaskDelete(event_task_handle_);
    event_task_handle_ = NULL;
    vTaskDelete(data_task_handle_);
    data_task_handle_ = NULL;
}

void ld2410::set_wake_callback(std::function<void(const std::string &wake_word)> on_wake_word_detected)
{
    on_wake_word_detected_ = on_wake_word_detected;
    
}

bool ld2410::write_ld2410_parameter()
{      
    config_status_ = true;
    bool result = false;
    size_t len;
    uint8_t parameter[] = {0xFD,0XFC,0XFB,0XFA,0X04,0X00,0XFF,0X00,0X01,0X00,0X04,0X03,0X02,0X01};
    
    size_t param_len = sizeof(parameter) / sizeof(parameter[0]);
    // 使能配置（初始发送+1次重试）
    for (int retry = 0; retry < 5; ++retry) {
        const int bytes_written = uart_write_bytes(uart_port_num_,parameter, param_len);
        if (bytes_written != param_len) {
            ESP_LOGE(TAG, "参数发送失败，实际发送 %d/%d 字节", bytes_written, param_len);
            continue; // 发送失败直接重试
        }
       
        EventBits_t Bits=xEventGroupWaitBits(uart_group_,  LD2410_CONFIG_ENABLE_SENDBACK, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
     
        if(!(Bits & LD2410_CONFIG_ENABLE_SENDBACK)){
            ESP_LOGE(TAG, "配置使能超时，1s后重试");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue; // 发送失败直接重试
        }
        result = true;
        ESP_LOGW(TAG, "配置使能通过");
        break;
    }
    if(result == false){
        ESP_LOGE(TAG, "配置使能失败");
        config_status_ = false;
        return result; // 发送失败返回
    }
    
    // 通用参数写入
    result = false;
    uint8_t parameter2[] = {0xFD,0XFC,0XFB,0XFA,0X26,0X00,0X70,0X00,0X05,0X00,0X0C,0X00,0X00,0X00,0X0A,0X00,0X00,0X00,0X00,0X00
        ,0X06,0X00,0X0a,0X00,0X00,0X00,0X02,0X00,0X05,0X00,0X00,0X00,0X0C,0X00,0X05,0X00,0X00,0X00,0X0B,0X00,0X05,0X00,0X00,0X00,0X04,0X03,0X02,0X01};
    param_len = sizeof(parameter2) / sizeof(parameter2[0]);
    for (int retry = 0; retry <5; ++retry) {
        const int bytes_written = uart_write_bytes(uart_port_num_, parameter2, param_len);
        if (bytes_written != param_len) {
            ESP_LOGE(TAG, "参数2发送失败，实际发送 %d/%d 字节", bytes_written, param_len);
            continue; // 发送失败直接重试
        }
        
        // 等待确认
        EventBits_t Bits = xEventGroupWaitBits(
            uart_group_, 
            LD2410_WRITE_PARAMETER_SENDBACK,
            pdTRUE,       // 清除标志位
            pdTRUE,       // 等待所有指定标志位
            pdMS_TO_TICKS(500) // 超时时间
        );
        
        if (!(Bits & LD2410_WRITE_PARAMETER_SENDBACK)) {
            ESP_LOGI(TAG, "参数2回复超时，1s后重试");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue; // 发送失败直接重试
        }
        result = true;
        break;
    }
    if(result == false){
        ESP_LOGE(TAG, "参数2写入失败");
        config_status_ = false;
        return result; // 发送失败返回
    }
    ESP_LOGW(TAG, "LD2410参数写入完成");
    config_status_ = false;
    return result;
}
