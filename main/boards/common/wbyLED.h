#ifndef WBYLED_H
#define WBYLED_H

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "mcp_server.h"
#include <functional>
#include <optional>

class wbyled {
private:
    static constexpr int kDutyMax = 1023;

    esp_timer_handle_t effect_timer_{nullptr};

    enum class Effect : uint8_t {
        kNone,
        kMarquee,
        kCrossfade,
    };

    Effect effect_mode_{Effect::kNone};
    int effect_speed_ms_{-1};
    int effect_intensity_{-1};
    int effect_step_{-1};
    int white_{0};
    int yellow_{0};
    int blue_{0};
    uint32_t breathe_phase_{0};

    static void EffectTimerCallback(void *arg);
    void StopEffectTimer();
    void StartEffectTimer();
    void ApplyDuty(int white, int yellow, int blue);
    int PercentToDuty(int percent) const;
    void TickEffect();
    static bool EffectNameToMode(const std::string &name, Effect &out);
    void NotifyLedStateChanged();
    std::optional<bool> cached_led_on_;
    std::function<void(bool)> on_led_state_changed_;

public:
    wbyled();
    ~wbyled();
    void stopwbyled();
    bool IsLedOn() const;
    bool GetLedState(std::string& effect, int& speed_ms, int& intensity, int& white, int& yellow, int& blue);
    /** effect: marquee | crossfade | none/off */
    bool SetEffect(const std::string& effect, int speed_ms = 50, int intensity = 80);
    void OnLedStateChanged(std::function<void(bool on)> callback);
    void Initwbyled(gpio_num_t motor_pwm_gpio, gpio_num_t motor_pwm2_gpio);
};

#endif
