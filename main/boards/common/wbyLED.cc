#include "wbyLED.h"

#include "config.h"
#include "esp_log.h"
#include <algorithm>
#include <cmath>

#define TAG "wbyled"

wbyled::wbyled() = default;

void wbyled::OnLedStateChanged(std::function<void(bool on)> callback)
{
    on_led_state_changed_ = std::move(callback);
}

void wbyled::NotifyLedStateChanged()
{
    bool on = IsLedOn();
    if (cached_led_on_.has_value() && cached_led_on_.value() == on) {
        return;
    }
    cached_led_on_ = on;
    if (on_led_state_changed_) {
        on_led_state_changed_(on);
    }
}

bool wbyled::IsLedOn() const
{
#ifdef WBY_STYLE
    if (effect_mode_ != Effect::kNone) {
        return true;
    }
    if (white_ > 0 || yellow_ > 0 || blue_ > 0) {
        return true;
    }
    return ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3) > 0 ||
           ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4) > 0 ||
           ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5) > 0;
#else
    return false;
#endif
}

wbyled::~wbyled()
{
#ifdef WBY_STYLE
    StopEffectTimer();
    if (effect_timer_ != nullptr) {
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
    }
#endif
}

bool wbyled::GetLedState(std::string &effect, int &speed_ms, int &intensity, int &white, int &yellow, int &blue)
{   
    switch (effect_mode_) {
    case Effect::kNone:
        effect = "none";
        break;
    case Effect::kMarquee:
        effect = "marquee";
        break;
    case Effect::kCrossfade:
        effect = "crossfade";
        break;
    }
    speed_ms = effect_speed_ms_;
    intensity = effect_intensity_;
    white = white_;
    yellow = yellow_;
    blue = blue_;
    return true;
}

int wbyled::PercentToDuty(int percent) const
{
    percent = std::max(0, std::min(100, percent));
    return (kDutyMax * percent) / 100;
}

void wbyled::ApplyDuty(int white, int yellow, int blue)
{
    white = std::max(0, std::min(kDutyMax, white));
    yellow = std::max(0, std::min(kDutyMax, yellow));
    blue = std::max(0, std::min(kDutyMax, blue));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, white);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, yellow);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, blue);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);
}

void wbyled::StopEffectTimer()
{
    if (effect_timer_ != nullptr && esp_timer_is_active(effect_timer_)) {
        esp_timer_stop(effect_timer_);
    }
    effect_mode_ = Effect::kNone;
}

void wbyled::StartEffectTimer()
{
    if (effect_timer_ == nullptr) {
        return;
    }
    if (esp_timer_is_active(effect_timer_)) {
        esp_timer_stop(effect_timer_);
    }
    int period_us = std::max(50, effect_speed_ms_) * 1000;
    esp_timer_start_periodic(effect_timer_, period_us);
}

void wbyled::EffectTimerCallback(void *arg)
{
    auto *self = static_cast<wbyled *>(arg);
    if (self != nullptr) {
        self->TickEffect();
    }
}

// void wbyled::TickEffect()
// {
//     const int peak = PercentToDuty(effect_intensity_);

//     switch (effect_mode_) {
//     case Effect::kNone:
//         break;
//     case Effect::kMarquee: {
//         int i = effect_step_ % 3;
//         effect_step_++;
//         if (i == 0) {
//             ApplyDuty(peak, 0, 0);
//         } else if (i == 1) {
//             ApplyDuty(0, peak, 0);
//         } else {
//             ApplyDuty(0, 0, peak);
//         }
//         break;
//     }
//     case Effect::kBreathe: {
//         breathe_phase_ = (breathe_phase_ + 1) % 256;
//         float rad = static_cast<float>(breathe_phase_) * (2.0f * static_cast<float>(M_PI) / 256.0f);
//         float factor = (std::sin(rad) + 1.0f) * 0.5f;
//         int d = static_cast<int>(peak * factor);
//         ApplyDuty(d, d, d);
//         break;
//     }
//     case Effect::kCrossfade: {
//         int sub = effect_step_ % 180;
//         effect_step_++;
//         if (sub < 60) {
//             float t = sub / 59.0f;
//             int w = static_cast<int>((1.0f - t) * peak);
//             int y = static_cast<int>(t * peak);
//             ApplyDuty(w, y, 0);
//         } else if (sub < 120) {
//             float t = (sub - 60) / 59.0f;
//             int y = static_cast<int>((1.0f - t) * peak);
//             int b = static_cast<int>(t * peak);
//             ApplyDuty(0, y, b);
//         } else {
//             float t = (sub - 120) / 59.0f;
//             int b = static_cast<int>((1.0f - t) * peak);
//             int w = static_cast<int>(t * peak);
//             ApplyDuty(w, 0, b);
//         }
//         break;
//     }
//     }
// }
void wbyled::TickEffect()
{
    const int peak = PercentToDuty(effect_intensity_);

    switch (effect_mode_) {
    case Effect::kNone:
        break;
    case Effect::kMarquee: {
        int i = effect_step_ % 3;
        effect_step_++;
        if (i == 0) {
            ApplyDuty(peak, 0, 0);
        } else if (i == 1) {
            ApplyDuty(0, peak, 0);
        } else {
            ApplyDuty(0, 0, peak);
        }
        break;
    }
    case Effect::kCrossfade: {
        int sub = effect_step_ % 512;
        effect_step_++;

        if (sub < 170) {
            // 白 → 黄
            float t = sub / 169.0f;
            int w = static_cast<int>((1.0f - t) * peak);
            int y = static_cast<int>(t * peak);
            ApplyDuty(w, y, 0);
        } else if (sub < 341) {
            // 黄 → 蓝
            float t = (sub - 170) / 169.0f;
            int y = static_cast<int>((1.0f - t) * peak);
            int b = static_cast<int>(t * peak);
            ApplyDuty(0, y, b);
        } else {
            // 蓝 → 白
            float t = (sub - 341) / 169.0f;
            int b = static_cast<int>((1.0f - t) * peak);
            int w = static_cast<int>(t * peak);
            ApplyDuty(w, 0, b);
        }
        break;
    }
    }
}
bool wbyled::EffectNameToMode(const std::string &name, Effect &out)
{
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') {
            lower.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            lower.push_back(static_cast<char>(c));
        }
    }
    if (lower == "none" || lower == "off" || lower == "solid") {
        out = Effect::kNone;
        return true;
    }
    if (lower == "marquee" || lower == "chase" || lower == "running") {
        out = Effect::kMarquee;
        return true;
    }
    if (lower == "crossfade" || lower == "gradient" || lower == "fade") {
        out = Effect::kCrossfade;
        return true;
    }
    // UTF-8 literals: do not pass through ASCII-only lowercasing
    if (name == "\xe8\xb7\x91\xe9\xa9\xac\xe7\x81\xaf" /* 跑马灯 */) {
        out = Effect::kMarquee;
        return true;
    }
    if (name == "\xe6\xb8\x90\xe5\x8f\x98" /* 渐变 */) {
        out = Effect::kCrossfade;
        return true;
    }
    return false;
}
void wbyled::stopwbyled(){
    StopEffectTimer();
    // 清除单独灯亮度
    white_ = 0;
    yellow_ = 0;
    blue_ = 0;
    ApplyDuty(0, 0, 0);
    NotifyLedStateChanged();
}
void wbyled::Initwbyled(gpio_num_t motor_pwm_gpio, gpio_num_t motor_pwm2_gpio)
{
    (void)motor_pwm_gpio;
    (void)motor_pwm2_gpio;

#ifdef WBY_STYLE
    const ledc_timer_config_t rgb_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&rgb_timer));

    ledc_channel_config_t ch = {
        .gpio_num = RGB_W,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_3,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = false},
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ch.gpio_num = RGB_Y;
    ch.channel = LEDC_CHANNEL_4;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ch.gpio_num = RGB_B;
    ch.channel = LEDC_CHANNEL_5;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    const esp_timer_create_args_t targs = {
        .callback = &wbyled::EffectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wby_rgb_fx",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &effect_timer_));
    NotifyLedStateChanged();

    auto &mcp = McpServer::GetInstance();

    // mcp.AddTool(
    //     "self.wby_rgb.set_channels",
    //     "Set WBY ambient RGB channels (white / yellow / blue) brightness 0-100. Stops any running light effect. "
    //     "Map user phrases: 白/暖白 -> white; 黄/暖黄 -> yellow; 蓝/冷蓝 -> blue. "
    //     "Examples: yellow+blue on -> yellow=80,blue=80,white=0; all warm -> white+yellow. "
    //     "仅返回操作结果，不要解释过程。",
    //     PropertyList({Property("white", kPropertyTypeInteger, 0, 0, 100),
    //                   Property("yellow", kPropertyTypeInteger, 0, 0, 100),
    //                   Property("blue", kPropertyTypeInteger, 0, 0, 100)}),
    //     [this](const PropertyList &properties) -> ReturnValue {
    //         StopEffectTimer();
    //         //清除效果
    //         effect_mode_ = Effect::kNone;
    //         effect_speed_ms_ = -1;
    //         effect_intensity_ = -1;
            
    //         white_ = properties["white"].value<int>();
    //         yellow_ = properties["yellow"].value<int>();
    //         blue_ = properties["blue"].value<int>();
    //         ApplyDuty(PercentToDuty(white_), PercentToDuty(yellow_), PercentToDuty(blue_));
    //         NotifyLedStateChanged();
    //         return true;
    //     });

//Examples: yellow+blue on -> yellow=80,blue=80,white=0; all warm -> white+yellow
        mcp.AddTool(
            "self.wby_rgb.set_channels",
            "Set WBY ambient RGB channels (white / yellow / blue) brightness 0-100. Stops any running light effect. "
            "Map user phrases: 白/暖白 -> white; 黄/暖黄 -> yellow; 蓝/冷蓝 -> blue. "
            "Examples: yellow on -> yellow=80,blue=0,white=0;"
            "同一时间只允许一个灯光开启，若用户提出同时打开所有灯光或同时打开白光和黄光则反馈无法同时打开"
            "仅返回操作结果，不要解释过程。",
            PropertyList({Property("white", kPropertyTypeInteger, 0, 0, 100),
                        Property("yellow", kPropertyTypeInteger, 0, 0, 100),
                        Property("blue", kPropertyTypeInteger, 0, 0, 100)}),
            [this](const PropertyList &properties) -> ReturnValue {
                StopEffectTimer();
                //清除效果
                effect_mode_ = Effect::kNone;
                effect_speed_ms_ = -1;
                effect_intensity_ = -1;
                
                int white = properties["white"].value<int>();
                int yellow = properties["yellow"].value<int>();
                int blue = properties["blue"].value<int>();

                int w = 0, y = 0, b = 0;
                if (white > 0) {
                    w = white;
                } else if (yellow > 0) {
                    y = yellow;
                } else if (blue > 0) {
                    b = blue;
                }

                white_ = w;
                yellow_ = y;
                blue_ = b;

                ApplyDuty(PercentToDuty(white_), PercentToDuty(yellow_), PercentToDuty(blue_));
                NotifyLedStateChanged();
                return true;
            });


    mcp.AddTool(
        "self.wby_rgb.off",
        "Turn off all WBY RGB channels and stop effects (全关灯并停止跑马灯/渐变等).",
        PropertyList(),
        [this](const PropertyList &) -> ReturnValue {
            StopEffectTimer();
            // 清除单独灯亮度
            white_ = 0;
            yellow_ = 0;
            blue_ = 0;
            ApplyDuty(0, 0, 0);
            NotifyLedStateChanged();
            return true;
        });

    mcp.AddTool(
        "self.wby_rgb.set_effect",
        "Run a light effect on white/yellow/blue channels. Stops previous effect. "
        "effect: 'marquee' (跑马灯, cycles W->Y->B one at a time), "
        "'crossfade' (渐变, smooth blend W<->Y<->B), "
        "'none'|'off'|'solid' to stop and keep current PWM until set_channels/off. "
        "speed_ms: step period 50-2000 (smaller = faster)，渐变效果默认50速度. intensity: peak brightness 1-100. "
        "用户说跑马灯 -> marquee; 渐变/流水变色 -> crossfade;",
        PropertyList({Property("effect", kPropertyTypeString),
                      Property("speed_ms", kPropertyTypeInteger, 50, 50, 2000),
                      Property("intensity", kPropertyTypeInteger, 80, 1, 100)}),
        [this](const PropertyList &properties) -> ReturnValue {
            Effect mode = Effect::kNone;
            if (!EffectNameToMode(properties["effect"].value<std::string>(), mode)) {
                return std::string("unknown effect");
            }
            // 清除单独灯亮度
            white_ = -1;
            yellow_ = -1;
            blue_ = -1;
            effect_speed_ms_ = properties["speed_ms"].value<int>();
            effect_intensity_ = properties["intensity"].value<int>();
            StopEffectTimer();
            if (mode == Effect::kNone) {
                NotifyLedStateChanged();
                return true;
            }
            effect_mode_ = mode;
            effect_step_ = 0;
            breathe_phase_ = 0;
            TickEffect();
            StartEffectTimer();
            NotifyLedStateChanged();
            return true;
        });

    ESP_LOGI(TAG, "WBY RGB MCP tools registered");
#endif
}
