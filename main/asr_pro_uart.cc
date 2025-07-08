#include "asr_pro_uart.h"

#define TAG "asr_pro"
static void uart_event_task(void *arg)
{
    asr_pro *asr_pro_obj = (asr_pro *)arg;
    uart_event_t event;
    while (1)
    {
        if (pdTRUE==xQueueReceive(asr_pro_obj->uart_event_queue_, &event, portMAX_DELAY)) //阻塞等待
        {
            switch (event.type)
            {
                case UART_DATA:
                    ESP_LOGI(TAG, "UART_DATA_EVENT");
                    xEventGroupSetBits(asr_pro_obj->uart_group_, ASR_UART_DATA_AVAILABLE_EVENT);
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
    asr_pro *asr_pro_obj = (asr_pro *)arg;
    size_t len;
    while (1)
    {   
        xEventGroupWaitBits(asr_pro_obj->uart_group_, ASR_UART_DATA_AVAILABLE_EVENT, pdTRUE, pdTRUE, portMAX_DELAY);
        uart_get_buffered_data_len(asr_pro_obj->uart_port_num_, &len);
        if (len > 0)
        {   

            uart_read_bytes(asr_pro_obj->uart_port_num_, asr_pro_obj->Rx_buffer_, len, portMAX_DELAY);
            asr_pro_obj->Rx_buffer_[len] = '\0';
            ESP_LOGI(TAG, "UART_DATA_RECEIVED: %02x", asr_pro_obj->Rx_buffer_[0]);
        }
        if ((asr_pro_obj->Rx_buffer_[0]==0x02) && (asr_pro_obj->on_wake_word_detected_)) 
        {   
            ESP_LOGI(TAG, "callback");
            asr_pro_obj->on_wake_word_detected_(asr_pro_obj->wake_word_);
        }
    }
}

asr_pro::asr_pro(int baud_rate,uart_port_t uart_port_num, gpio_num_t uart_tx_pin, gpio_num_t uart_rx_pin)
:baud_rate_(baud_rate), uart_port_num_(uart_port_num), uart_tx_pin_(uart_tx_pin), uart_rx_pin_(uart_rx_pin)
{   
    uart_event_queue_ = xQueueCreate(ASR_UART_EVENT_QUEUE_SIZE, sizeof(uart_event_t));
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
    
    uart_driver_install(uart_port_num_, ASR_UART_RX_BUFFER_SIZE, ASR_UART_TX_BUFFER_SIZE, ASR_UART_EVENT_QUEUE_SIZE, &uart_event_queue_, 0);

    uart_param_config(uart_port_num_, &uart_config);

    uart_set_pin(uart_port_num_, uart_tx_pin_, uart_rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(uart_event_task, "uart_event_task", 2048, this, 10, &event_task_handle_);

    xTaskCreate(uart_data_task, "uart_data_task", 2048, this, 10, &data_task_handle_);
}

asr_pro::~asr_pro()
{
    uart_driver_delete(uart_port_num_);
    vQueueDelete(uart_event_queue_);
    vEventGroupDelete(uart_group_);
    vTaskDelete(event_task_handle_);
    event_task_handle_ = NULL;
    vTaskDelete(data_task_handle_);
    data_task_handle_ = NULL;
}

void asr_pro::set_wake_callback(std::function<void(const std::string &wake_word)> on_wake_word_detected)
{
    on_wake_word_detected_ = on_wake_word_detected;
    
}

