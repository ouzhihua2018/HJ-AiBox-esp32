#ifndef MICRO_WAKE_WORD_DETECT_H
#define MICRO_WAKE_WORD_DETECT_H
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <functional>
#include <string>
#include <vector>
#include "esphome/components/i2s_audio/microphone/i2s_audio_microphone.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#include "model.h"
#include "esp_timer.h"
class MicroWakeWordDetect{
public:
    MicroWakeWordDetect();
    ~MicroWakeWordDetect(); //gpio_num_t BCLK,gpio_num_t SD,gpio_num_t WS,i2s_port_t port,int sample_rate
    void InitializeWakeWordDetect();
    void OnWakeWordDetected(std::function<void(std::string wake_word)> callback) {callback_=callback;} ;
    void StartDetection();
    void Feed(std::vector<int16_t>& data) ;

    void Stop();
    bool IsRunning();
private:
    std::function<void(std::string wake_word)> callback_;
    esphome::micro_wake_word::MicroWakeWord wakeWord_ ;
    bool Detected_;
    esp_timer_handle_t micro_timer_handle_;
};
#endif