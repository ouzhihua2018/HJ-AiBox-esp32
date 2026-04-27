#ifndef MOTOR_H
#define MOTOR_H
#include "driver/ledc.h"
#include "mcp_server.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include <cmath>
#include <algorithm>

#define DEFAULT_MAX_ANGLE    360.0f   // ADC=4095对应最大角度
#define ANGLE_ERROR_THRESHOLD 1.0f    // 角度误差阈值（±1°）
#define ADC_CALIBRATION_OFFSET 0      // ADC零点校准偏移（根据实际硬件调整）

class motor
{
private:
    // 电机保护
    esp_timer_handle_t motor_protect_timer_handle_;
    // 检测部分
    esp_timer_handle_t angle_read_timer_handle_;
    bool direction_= false; 
    float target_angle_;
    adc_oneshot_unit_handle_t adc1_handle_;
    float AdcToAngle(int& adc_value);
    void InitAngleDetecter();
    void InitMotorProtect();
    void ReverseDirection();
    bool IsReachTargetAngle(float current_angle);
    bool IsMotorStall(float current_angle);
    // void StartMotor(int target_duty);
    // void StopMotor();
    // void SpeedUp();
    // void SlowDown();
    // void SetPwm(int pwm);
    std::function<void(bool)> on_reset_callback_;
public:
    motor();
    ~motor();
    void InitMotor(gpio_num_t MOTOR_PWM_GPIO,gpio_num_t MOTOR_PWM2_GPIO);
    void SetSpeedLevel(int level);
    void motor_test();
    int GetSpeed();
    void OnResetCallback(std::function <void(bool)> callback) {on_reset_callback_ = callback;};
};



#endif