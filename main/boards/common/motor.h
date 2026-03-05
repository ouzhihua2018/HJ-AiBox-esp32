#ifndef MOTOR_H
#define MOTOR_H
#include "driver/ledc.h"
#include "mcp_server.h"
#include "esp_log.h"
#include "driver/gpio.h"
class motor
{
private:
    bool working_ = false;
public:
    motor();
    ~motor();
    void InitMotor(gpio_num_t MOTOR_PWM_GPIO);
    void motor_test();
};



#endif