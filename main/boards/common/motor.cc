#include "motor.h"
#define TAG "MOTOR"

#define SPEED_STOP      0
#define SPEED_LOW       1
#define SPEED_MEDIUM    2
#define SPEED_HIGH      3

#define STALL_CHECK_INTERVAL_MS      2000   //1000ms 监测一次堵转
#define STALL_ANGLE_THRESHOLD        3.0f
#define STALL_CONSECUTIVE_TIMES      3

int pwm_array[4] = {0, 7000, 7555, 8192};

motor::motor()
{
}

motor::~motor()
{
}

void motor::InitMotor(gpio_num_t MOTOR_PWM_GPIO, gpio_num_t MOTOR_PWM2_GPIO)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 4883,
        .clk_cfg = LEDC_AUTO_CLK};

    esp_err_t timer_ret = ledc_timer_config(&timer_conf);
    if (timer_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LEDC timer init failed: %s", esp_err_to_name(timer_ret));
        return;
    }

    ledc_channel_config_t ledc_conf;
    ledc_conf.channel = LEDC_CHANNEL_1;
    ledc_conf.duty = 0;
    ledc_conf.gpio_num = MOTOR_PWM_GPIO;
    ledc_conf.intr_type = LEDC_INTR_DISABLE;
    ledc_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_conf.timer_sel = timer_conf.timer_num;
    ledc_conf.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
    ledc_conf.hpoint = 0;
    ledc_conf.flags.output_invert = 0;
    ledc_channel_config(&ledc_conf);

    ledc_conf.channel = LEDC_CHANNEL_2;
    ledc_conf.gpio_num = MOTOR_PWM2_GPIO;
    ledc_channel_config(&ledc_conf);
#ifndef WBY_STYLE
    InitAngleDetecter();
    InitMotorProtect();
#endif
    auto &mcp_server = McpServer::GetInstance();

    mcp_server.AddTool("self.motor.start",
                       "Start the motor at medium speed (speed level 2).\n"
                       "MANDATORY RULE: MUST call `self.get_device_status` first.\n"
                       "Only return the final result. Do NOT explain the process.",
                       PropertyList(),
                       [this](const PropertyList &properties) -> ReturnValue
                       {
                           SetSpeedLevel(SPEED_MEDIUM);
                           return true;
                       });

    mcp_server.AddTool("self.motor.stop",
                       "Stop the motor immediately.\n",
                       PropertyList(),
                       [this](const PropertyList &properties) -> ReturnValue
                       {
                           SetSpeedLevel(SPEED_STOP);
                           return true;
                       });

    mcp_server.AddTool("self.motor.adjust_speed",
                       "Adjust motor speed 0-3.\n"
                       "Only return the final result. Do NOT explain the process.",
                       PropertyList({Property("speed", kPropertyTypeInteger, 0, 3)}),
                       [this](const PropertyList &properties) -> ReturnValue
                       {
                           SetSpeedLevel(properties["speed"].value<int>());
                           return true;
                       });

    // ===================== 【复位：已修复】 =====================
    mcp_server.AddTool("self.motor.reset",
                       "Start moving the motor to the reset position.\n"
                       "你只需要回答电机已开始复位.",
                       PropertyList(),
                       [this](const PropertyList &properties) -> ReturnValue
                       {
                           target_angle_ = 0.0f;
                           
                           int adc_val;
                           adc_oneshot_read(adc1_handle_, ADC_CHANNEL_3, &adc_val);
                           float current_angle = AdcToAngle(adc_val);

                           if (IsReachTargetAngle(current_angle))
                           {
                               ESP_LOGI(TAG, "Already in reset position, no action.");
                               SetSpeedLevel(SPEED_STOP);
                            //    if(on_reset_callback_){
                            //         on_reset_callback_(true);
                            //    }
                               target_angle_ = -1.0f;
                               return true;
                           }

                           ESP_LOGI(TAG, "Start reset to 0°.");
                           esp_timer_start_periodic(angle_read_timer_handle_, 300 * 1000);
                           SetSpeedLevel(SPEED_LOW);
                           return true;
                       });

    mcp_server.AddTool("self.motor.reverse",
                       "Reverse motor direction.\n"
                       "Only return the final result. Do NOT explain the process.",
                       PropertyList(),
                       [this](const PropertyList &properties) -> ReturnValue
                       {
                           ReverseDirection();
                           return true;
                       });
}

void motor::InitMotorProtect()
{
    esp_timer_create_args_t motor_protect_args = {
        .callback = [](void *arg)
        {
            motor *this_ = (motor *)arg;
            if (this_->GetSpeed() == SPEED_STOP) return;

            int adc_raw;
            if (adc_oneshot_read(this_->adc1_handle_, ADC_CHANNEL_3, &adc_raw) != ESP_OK) return;
            float current_angle = this_->AdcToAngle(adc_raw);
            //ESP_LOGI(TAG,"当前角度%f",current_angle);
            if (this_->IsMotorStall(current_angle))
            {
                ESP_LOGE(TAG, "!!! MOTOR STALL PROTECTION TRIGGERED !!!");
                this_->SetSpeedLevel(SPEED_STOP);
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "motor_stall",
        .skip_unhandled_events = true
    };

    esp_timer_create(&motor_protect_args, &motor_protect_timer_handle_);
}

void motor::InitAngleDetecter()
{
    adc_oneshot_unit_init_cfg_t init_config1{};
    init_config1.clk_src = ADC_RTC_CLK_SRC_RC_FAST;
    init_config1.unit_id = ADC_UNIT_1;
    init_config1.ulp_mode = ADC_ULP_MODE_DISABLE;
    adc_oneshot_new_unit(&init_config1, &adc1_handle_);

    adc_oneshot_chan_cfg_t adc1_config{};
    adc1_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc1_config.atten = ADC_ATTEN_DB_12;
    adc_oneshot_config_channel(adc1_handle_, ADC_CHANNEL_3, &adc1_config);

    esp_timer_create_args_t angle_timer_args = {
        .callback = [](void *arg)
        {
            motor *this_ = (motor *)arg;
            int adc_raw;
            if (adc_oneshot_read(this_->adc1_handle_, ADC_CHANNEL_3, &adc_raw) != ESP_OK) return;
            float current_angle = this_->AdcToAngle(adc_raw);

            if (this_->IsReachTargetAngle(current_angle))
            {
                this_->SetSpeedLevel(SPEED_STOP);
                ESP_LOGI(TAG, "Reached target angle: %.1f, current: %.1f", this_->target_angle_, current_angle);
                esp_timer_stop(this_->angle_read_timer_handle_);
            //     if(this_->on_reset_callback_){
            //         this_->on_reset_callback_(true);
            //    }
                this_->target_angle_ = -1.0f;
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "angle_timer",
        .skip_unhandled_events = true
    };

    esp_timer_create(&angle_timer_args, &angle_read_timer_handle_);
}

int motor::GetSpeed()
{
    int current_pwm = 0;
    if (direction_)
        current_pwm = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    else
        current_pwm = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);

    if (current_pwm < pwm_array[1]) return SPEED_STOP;
    if (current_pwm < pwm_array[2]) return SPEED_LOW;
    if (current_pwm < pwm_array[3]) return SPEED_MEDIUM;
    return SPEED_HIGH;
}

void motor::SetSpeedLevel(int level)
{
    if (level < 0 || level > 3) level = SPEED_MEDIUM;
    int target_pwm = pwm_array[level];

    if (direction_)
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, target_pwm);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    }
    else
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, target_pwm);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    }
#ifndef WBY_STYLE
    if (level == SPEED_STOP)
    {
        
        if (esp_timer_is_active(motor_protect_timer_handle_))
            esp_timer_stop(motor_protect_timer_handle_);
        IsMotorStall(-100.0f);
    }
    else
    {
        if (!esp_timer_is_active(motor_protect_timer_handle_))
            esp_timer_start_periodic(motor_protect_timer_handle_, STALL_CHECK_INTERVAL_MS * 1000);
    }
#endif
}

void motor::ReverseDirection()
{
    int speed = GetSpeed();
    if (speed == SPEED_STOP)
    {
        ESP_LOGI(TAG, "Motor stopped, cannot reverse.");
        return;
    }
    direction_ = !direction_;
    SetSpeedLevel(speed);
    ESP_LOGI(TAG, "Reversed direction: %d", direction_);
}

float motor::AdcToAngle(int &adc_value)
{
    int val = std::max(0, std::min(4095, adc_value));
    float angle = (val / 4095.0f) * DEFAULT_MAX_ANGLE;
    return std::round(angle * 10) / 10.0f;
}

bool motor::IsReachTargetAngle(float current_angle)
{
    if (target_angle_ < 0.0f) return false;
    return fabs(current_angle - target_angle_) <= ANGLE_ERROR_THRESHOLD;
}

// ===================== 【类型修复】 =====================
bool motor::IsMotorStall(float current_angle)
{
    static float last_angle = 0.0f;
    static int stall_count = 0;

    if (fabs(current_angle - last_angle) > STALL_ANGLE_THRESHOLD)
    {
        stall_count = 0;
        last_angle = current_angle;
        return false;
    }

    stall_count++;
    last_angle = current_angle;

    if (stall_count >= STALL_CONSECUTIVE_TIMES)
    {
        stall_count = 0;
        return true;
    }
    return false;
}

void motor::motor_test()
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 6800);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}