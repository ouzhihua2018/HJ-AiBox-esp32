#ifndef __ASR_PRO_UART_H__
#define __ASR_PRO_UART_H__
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <string>
#include <functional>

#define ASR_UART_DATA_AVAILABLE_EVENT (1<<0)

#define ASR_UART_RX_BUFFER_SIZE (256)
#define ASR_UART_TX_BUFFER_SIZE (256)
#define ASR_UART_EVENT_QUEUE_SIZE (10)

#define ASR_DEFAULT_BAUD_RATE 115200
#define ASR_DEFAULT_UART_NUM UART_NUM_2

#define ASR_DEFAULT_TX_PIN (gpio_num_t)(CONFIG_ASR_UART_TXD)
#define ASR_DEFAULT_RX_PIN (gpio_num_t)(CONFIG_ASR_UART_RXD)


    class asr_pro
    {
    public:
        int baud_rate_;
        uart_port_t uart_port_num_;
       
        gpio_num_t uart_tx_pin_;
        gpio_num_t uart_rx_pin_;    

        QueueHandle_t uart_event_queue_;
        EventGroupHandle_t uart_group_;
        
        std::string wake_word_ = "wn9_nihaoxiaozhi_tts";
        asr_pro(int baud_rate = ASR_DEFAULT_BAUD_RATE, uart_port_t uart_port_num = ASR_DEFAULT_UART_NUM, gpio_num_t uart_tx_pin = ASR_DEFAULT_TX_PIN, gpio_num_t uart_rx_pin = ASR_DEFAULT_RX_PIN);
        ~asr_pro();
        uint8_t Rx_buffer_[ASR_UART_RX_BUFFER_SIZE];
        void set_wake_callback(std::function<void(const std::string& wake_word)> on_wake_word_detected);
        std::function<void(const std::string& wake_word)> on_wake_word_detected_;

    private:
        TaskHandle_t event_task_handle_;
        TaskHandle_t data_task_handle_;
        
    };
    
   
    
    
#endif