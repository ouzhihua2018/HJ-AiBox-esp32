#ifndef _ML307A_H_
#define _ML307A_H_

#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <string>
// #include <cstddef>
// #include <string>
// #include <vector>
// #include <list>
// #include <functional>
// #include <mutex>
#define DEFAULT_UART_NUM UART_NUM_2
#define DEFAULT_BAUD_RATE 115200
#define AT_EVENT_DATA_AVAILABLE BIT1



class Ml307A {
private:
    size_t rx_buffer_size_;
    std::string rx_buffer_;
    uart_port_t uart_num_;
    int tx_pin_;
    int rx_pin_;
    int baud_rate_;
    EventGroupHandle_t event_group_handle_;
    QueueHandle_t event_queue_handle_;
    TaskHandle_t event_task_handle_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
    void EventTask();
    void ReceiveTask() ;
    bool ParseResponse();
public:
    Ml307A(int tx_pin, int rx_pin, size_t rx_buffer_size);
    ~Ml307A();
};
#endif