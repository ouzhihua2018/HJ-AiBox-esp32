#include "dual_network_board.h"
#include "audio_codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "iot/thing_manager.h"
#include "led/circular_strip.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include <freertos/FreeRTOS.h> //新增
#include <freertos/task.h> //新增
#include <sys/time.h>   //新增

#define TAG "TDi-300-PH-MainBoard"




class TDi_300_PH : public DualNetworkBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    
    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_CHARGING_GPIO);
        // power_manager_->OnTemperatureChanged([this](float chip_temp) {
        //     display_->UpdateHighTempWarning(chip_temp);
        // });
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        // rtc_gpio_init(GPIO_NUM_3);
        // rtc_gpio_set_direction(GPIO_NUM_3, RTC_GPIO_MODE_OUTPUT_ONLY);
        // rtc_gpio_set_level(GPIO_NUM_3, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            // display_->SetChatMessage("system", "");
            // display_->SetEmotion("sleepy");
            // GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            // display_->SetChatMessage("system", "");
            // display_->SetEmotion("neutral");
            //GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG,"BOOT TRIGGER");
                SwitchNetworkType();
          
            //app.EmergencyWake();
        });
        boot_button_.OnMultipleClick([this](){
            if (GetNetworkType() == NetworkType::WIFI) {
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.ResetWifiConfiguration();
            }
        },3);
        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto& app = Application::GetInstance();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
                if(app.GetDeviceState()==kDeviceStateIdle) {
                    app.ResetDecoder();
                    app.PlaySound(Lang::Sounds::P3_LIMIT);
                }
            }else{
                //ESP_LOGI(TAG,"PlaySound");
                if(app.GetDeviceState()==kDeviceStateIdle) {
                    app.ResetDecoder();
                    app.PlaySound(Lang::Sounds::P3_PLUS);
                }
            }

            codec->SetOutputVolume(volume);
            //GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            //GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });
   
        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto& app = Application::GetInstance();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
                if(app.GetDeviceState()==kDeviceStateIdle) {
                    app.ResetDecoder();
                    app.PlaySound(Lang::Sounds::P3_LIMIT);
                }
            } else{
                if(app.GetDeviceState()==kDeviceStateIdle) {
                    app.ResetDecoder();
                    app.PlaySound(Lang::Sounds::P3_MINUS);
                }
            }
            codec->SetOutputVolume(volume);
            //GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_down_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            //GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }
    
  
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Battery"));
    }

public:
    TDi_300_PH () :
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, 4096),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO)
    {   
        InitializeI2c();
        InitializePowerManager();
        InitializePowerSaveTimer();
        //InitializeSpi();
        InitializeButtons();
        //InitializeSt7789Display();  
        InitializeIot();
        //GetBacklight()->RestoreBrightness();
    }
    
    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 2);
        return &led;
    }
    virtual Led* GetLed2() override {
        static CircularStrip led(LED_STRIP_GPIO, 42);
        return &led;
    }
    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            GPIO_NUM_NC, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging)  override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        return true;
    }

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        DualNetworkBoard::SetPowerSaveMode(enabled);
    }
    
};

DECLARE_BOARD(TDi_300_PH);
