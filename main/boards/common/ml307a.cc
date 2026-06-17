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
                char* rx_buffer_ptr = &rx_buffer_[rx_buffer_.size() - available];
                uart_read_bytes(uart_num_, rx_buffer_ptr, available, portMAX_DELAY);
                //while (ParseResponse()) {}
                //先简单打印收到的内容
                ESP_LOGI(TAG, "received data: %s", rx_buffer_.c_str());
            }
        }
    }
}


Ml307A::~Ml307A() {
}