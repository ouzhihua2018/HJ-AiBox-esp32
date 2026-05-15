#ifndef WBYLED_H
#define WBYLED_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "mcp_server.h"

class wbyled {
private:
    static constexpr int kDutyMax = 1023;

    esp_timer_handle_t effect_timer_{nullptr};

    enum class Effect : uint8_t {
        kNone,
        kMarquee,
        kBreathe,
        kCrossfade,
    };

    Effect effect_mode_{Effect::kNone};
    int effect_speed_ms_{200};
    int effect_intensity_{80};
    int effect_step_{0};
    uint32_t breathe_phase_{0};

    static void EffectTimerCallback(void *arg);
    void StopEffectTimer();
    void StartEffectTimer();
    void ApplyDuty(int white, int yellow, int blue);
    int PercentToDuty(int percent) const;
    void TickEffect();
    static bool EffectNameToMode(const std::string &name, Effect &out);

public:
    wbyled();
    ~wbyled();
    void Initwbyled(gpio_num_t motor_pwm_gpio, gpio_num_t motor_pwm2_gpio);
};

#endif
