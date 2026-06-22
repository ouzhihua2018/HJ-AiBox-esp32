#include "ml307a.h"
static const char *TAG = "Ml307A";
Ml307A::Ml307A(int tx_pin, int rx_pin, size_t rx_buffer_size)
: rx_buffer_size_(rx_buffer_size), uart_num_(DEFAULT_UART_NUM), tx_pin_(tx_pin), rx_pin_(rx_pin), baud_rate_(DEFAULT_BAUD_RATE) {
    event_group_handle_ = xEventGroupCreate();

    uart_config_t uart_config = {};
    uart_config.baud_rate = baud_rate_;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(uart_num_, rx_buffer_size_ , 0, 100, &event_queue_handle_, ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate([](void* arg) {
        auto ml307a = (Ml307A*)arg;
        ml307a->EventTask();
        vTaskDelete(NULL);
    }, "modem_event", 4096, this, 15, &event_task_handle_);

    xTaskCreate([](void* arg) {
        auto ml307a = (Ml307A*)arg;
        ml307a->ReceiveTask();
        vTaskDelete(NULL);
    }, "modem_receive", 4096 * 2, this, 15, &receive_task_handle_);
}


void Ml307A::EventTask() {
    uart_event_t event;
    while (true) {
        if (xQueueReceive(event_queue_handle_, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type)
            {
            case UART_DATA:
                xEventGroupSetBits(event_group_handle_, AT_EVENT_DATA_AVAILABLE);
                break;
            case UART_BREAK:
                ESP_LOGI(TAG, "break");
                break;
            case UART_BUFFER_FULL:
                ESP_LOGE(TAG, "buffer full");
                break;
            case UART_FIFO_OVF:
                ESP_LOGE(TAG, "FIFO overflow");
                //NotifyCommandResponse("FIFO_OVERFLOW", {});
                break;
            default:
                ESP_LOGE(TAG, "unknown event type: %d", event.type);
                break;
            }
        }
    }
}


void Ml307A::ReceiveTask() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_DATA_AVAILABLE, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & AT_EVENT_DATA_AVAILABLE) {
            size_t available;
            uart_get_buffered_data_len(uart_num_, &available);
            if (available > 0) {
                // Extend rx_buffer_ and read into buffer
                rx_buffer_.resize(rx_buffer_.size() + available);
                char* rx_buffer_ptr = &rx_buffer_[rx_buffer_.size() - available]; //手动追加
                uart_read_bytes(uart_num_, rx_buffer_ptr, available, portMAX_DELAY);
                //while (ParseResponse()) {}
                //先简单打印收到的内容
                ESP_LOGI(TAG, "received data: %s", rx_buffer_.c_str());
                
                ParseResponse();
               
                
            }
        }
    }
}
//oc_ring_cb:+CLCC: 1,1,4,0,0,"13089725655",129,"",0,0   oc_ring_cb:NO CARRIER
bool Ml307A::ParseResponse()   //return true 都是表示当前这条处理完成，但此次接收不一定全都处理完了，全部处理完返回false
{
    
    // auto end_pos = rx_buffer_.find("/r/n");
    // if(end_pos == std::string::npos){   //此次读取没有读到/r/n
    //     ESP_LOGE(TAG,"AT指令错误，未收到结束符，等待下次读取后一同拼接");
    //     return false;
    // }
    // if(end_pos == 0){  //空包不处理
    //     rx_buffer_.erase(0,2);
    //     return false;
    // }
    //直接按单次读取只会读到单条命令处理
    // auto pos = rx_buffer_.find(":");
    // auto command = rx_buffer_.substr(0,pos);
    if(rx_buffer_[0] == 'o' && rx_buffer_[1] == 'c' && rx_buffer_[2] == '_' && rx_buffer_[3] == 'r'&& rx_buffer_[4] == 'i'\
        && rx_buffer_[5] == 'n' && rx_buffer_[6] == 'g' && rx_buffer_[11] == '+' && rx_buffer_[12] == 'C' && rx_buffer_[13] == 'L'\
        && rx_buffer_[14] == 'C' && rx_buffer_[15] == 'C')
    {
        auto pos = rx_buffer_.find('"');
        auto number = rx_buffer_.substr(pos,11);
        ESP_LOGW(TAG,"收到电话:%s,自动接听",number);
        std::string at_command = "ATA\r\n";
        uart_write_bytes(uart_num_,at_command.c_str(),at_command.size());
        rx_buffer_.erase(0,rx_buffer_.size()); 
        return true;
    } else {
        ESP_LOGI(TAG,"未知命令直接擦除");
        rx_buffer_.erase(0,rx_buffer_.size()); 
        return true;
    }
    return false;
}

Ml307A::~Ml307A() {
}