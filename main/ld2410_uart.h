#ifndef __LD2410_UART_H__
#define __LD2410_UART_H__
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <string>
#include <functional>

#define LD2410_UART_DATA_AVAILABLE_EVENT (1<<0)
#define LD2410_CONFIG_ENABLE_SENDBACK (1<<1)
#define LD2410_WRITE_PARAMETER_SENDBACK (1<<2)
#define LD2410_UART_RX_BUFFER_SIZE (256)
#define LD2410_UART_TX_BUFFER_SIZE (256)
#define LD2410_UART_EVENT_QUEUE_SIZE (10)

#define LD2410_DEFAULT_BAUD_RATE 115200
#define LD2410_DEFAULT_UART_NUM UART_NUM_2

#define LD2410_DEFAULT_TX_PIN GPIO_NUM_2
#define LD2410_DEFAULT_RX_PIN GPIO_NUM_1


    class ld2410
    {
    public:
        int baud_rate_;
        uart_port_t uart_port_num_;
       
        gpio_num_t uart_tx_pin_;
        gpio_num_t uart_rx_pin_;    

        QueueHandle_t uart_event_queue_;
        EventGroupHandle_t uart_group_;
        uint8_t No_Presence_times_ = 0;
        std::string wake_word_ = "nihao";
        ld2410(int baud_rate = LD2410_DEFAULT_BAUD_RATE, uart_port_t uart_port_num = LD2410_DEFAULT_UART_NUM, gpio_num_t uart_tx_pin = LD2410_DEFAULT_TX_PIN, gpio_num_t uart_rx_pin = LD2410_DEFAULT_RX_PIN);
        ~ld2410();
        uint8_t Rx_buffer_[LD2410_UART_RX_BUFFER_SIZE];
        void set_wake_callback(std::function<void(const std::string& wake_word)> on_wake_word_detected);
        bool write_ld2410_parameter();
        std::function<void(const std::string& wake_word)> on_wake_word_detected_;
        bool config_status_;
    private:
        TaskHandle_t event_task_handle_;
        TaskHandle_t data_task_handle_;
        
    };
    
   
    
    
#endif