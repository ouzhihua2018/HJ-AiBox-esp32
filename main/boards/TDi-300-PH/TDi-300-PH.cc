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
#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include "motor.h"
#include "wbyLED.h"
#define TAG "TDi-300-PH-MainBoard"
#include "ml307a.h"


uint8_t uid[RC522_PICC_UID_SIZE_MAX] = {0};

static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;
    auto &app = Application::GetInstance();

    if (picc->state == RC522_PICC_STATE_ACTIVE)
    {
        ESP_LOGI(TAG,"RFID 已识别");
        for(int i=0;i<RC522_PICC_UID_SIZE_MAX;i++){
            if(!(picc->uid.value[i] == uid[i])&&(app.GetDeviceState()==kDeviceStateListening)){
                
                memcpy(uid,picc->uid.value,RC522_PICC_UID_SIZE_MAX);
                app.CharacterSwitch(uid, RC522_PICC_UID_SIZE_MAX);
                app.ResetDecoder();
                app.PlaySound(Lang::Sounds::P3_POPUP);
            }
        }
    }
    else if (picc->state == RC522_PICC_STATE_IDLE && event->old_state >= RC522_PICC_STATE_ACTIVE)
    {
        ESP_LOGI(TAG, "Card has been removed");
    }
}

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

class TDi_300_PH : public DualNetworkBoard
{
private:

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    PowerSaveTimer *power_save_timer_;
    PowerManager *power_manager_;
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    motor motor_;
    wbyled wby_led_;
    float target_angle_;
    int current_pwm_ = 0;
    int current_mode_ = 0;
    Ml307A ml307a_;
    void InitializeI2c()
    {
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
    void InitializePowerManager()
    {
        power_manager_ = new PowerManager(POWER_CHARGING_GPIO);
        power_manager_->OnTemperatureChanged([this](float chip_temp)
                                             {
                                                 // display_->UpdateHighTempWarning(chip_temp);
                                             });
        power_manager_->OnChargingStatusChanged([this](bool is_charging)
                                                {
            auto led2 = GetLed2();
            led2->FlashOnce();
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            } });
        power_manager_->OnLowBatteryStatusChanged([](bool low_battery) {
            auto& app = Application::GetInstance();
            if (low_battery) {
                app.RequestLowBatteryHalt();
            } else {
                app.ClearLowBatteryHalt();
            }
        });
        power_manager_->OnBatteryWarningLevelChanged([](uint8_t level) {
            auto& app = Application::GetInstance();
            app.Schedule([level, &app]() {
                if (app.GetDeviceState() == kDeviceStateLowBattery) {
                    return;
                }
                // 30% / 20% 仅提醒，不打断当前会话流程
                app.ResetDecoder();
                app.PlaySound(Lang::Sounds::P3_BATTERYLOW);
                
            });
        });
    }

    void InitializePowerSaveTimer()
    {
        // rtc_gpio_init(GPIO_NUM_3);
        // rtc_gpio_set_direction(GPIO_NUM_3, RTC_GPIO_MODE_OUTPUT_ONLY);
        // rtc_gpio_set_level(GPIO_NUM_3, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]()
                                            {
                                                ESP_LOGI(TAG, "Enabling sleep mode");
                                                // display_->SetChatMessage("system", "");
                                                // display_->SetEmotion("sleepy");
                                                // GetBacklight()->SetBrightness(1);
                                            });
        power_save_timer_->OnExitSleepMode([this]()
                                           {
                                               // display_->SetChatMessage("system", "");
                                               // display_->SetEmotion("neutral");
                                               // GetBacklight()->RestoreBrightness();
                                           });
        power_save_timer_->SetEnabled(true);
    }
#ifndef WBY_STYLE
    void InitializeRc522()
    {
        spi_bus_config_t spi_bus_conf = {
            .mosi_io_num = RC522_SPI_MOSI_GPIO,
            .miso_io_num = RC522_SPI_MISO_GPIO,
            .sclk_io_num = RC522_SPI_SCLK_GPIO,
        };
        rc522_spi_config_t driver_config = {
            .host_id = SPI3_HOST,
            .bus_config = &spi_bus_conf,
            .dev_config = {
                .spics_io_num = RC522_SPI_SCANNER_SDA_GPIO,
            },
            .rst_io_num = RC522_SCANNER_RST_GPIO,
        };
        rc522_spi_create(&driver_config, &driver);
        rc522_driver_install(driver);

        rc522_config_t scanner_config = {
            .driver = driver,
        };

        rc522_create(&scanner_config, &scanner);
        rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, NULL);
        //rc522_start(scanner);
    }
#endif
    void InitializeButtons()
    {
        boot_button_.OnClick([this]()
                             {
            auto& app = Application::GetInstance();
            if (app.IsInShowcaseMode()) {
                ESP_LOGI(TAG, "BOOT: exit showcase");
                app.ExitShowcase();
                return;
            }
            ESP_LOGW(TAG,"BOOT BUTTON");
            if(app.background_audio_stream_){
                app.SetBackgroundAudioStreamActive(false);
            }
            app.ToggleChatState();
             });

        // boot_button_.OnLongPress([this]()
        //                          {
        //                              auto &app = Application::GetInstance();
        //                              ESP_LOGI(TAG, "BOOT TRIGGER");
        //                              SwitchNetworkType();
        //                              // ESP_LOGI(TAG,"当前LEVLE:%d",gpio_get_level(MOTOR_PWM_GPIO));
        //                              // app.EmergencyWake();
        //                          });
        // boot_button_.OnMultipleClick([this]()
        //                              {
        //     if (GetNetworkType() == NetworkType::WIFI) {
        //         auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
        //         wifi_board.ResetWifiConfiguration();
        //     } }, 3);

        volume_up_button_.OnClick([this]()
                                  {
                                      //power_save_timer_->WakeUp();
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
                                      // //GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
                                  });

        volume_up_button_.OnLongPress([this]()
                                      {
                                        //power_save_timer_->WakeUp();
                                        GetAudioCodec()->SetOutputVolume(100);
                                        //GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
                                      });
            
        volume_down_button_.OnClick([this]()
                                    {
                                        // power_save_timer_->WakeUp();
                                        auto codec = GetAudioCodec();
                                        auto &app = Application::GetInstance();
                                        auto volume = codec->output_volume() - 10;
                                        if (volume < 0)
                                        {
                                            volume = 0;
                                            if (app.GetDeviceState() == kDeviceStateIdle)
                                            {
                                                app.ResetDecoder();
                                                app.PlaySound(Lang::Sounds::P3_LIMIT);
                                            }
                                        }
                                        else
                                        {
                                            if (app.GetDeviceState() == kDeviceStateIdle)
                                            {
                                                app.ResetDecoder();
                                                app.PlaySound(Lang::Sounds::P3_MINUS);
                                            }
                                        }
                                        codec->SetOutputVolume(volume);
                                        // GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
                                    });

        volume_down_button_.OnLongPress([this]()
                                        {
                                            // power_save_timer_->WakeUp();
                                            GetAudioCodec()->SetOutputVolume(0);
                                            // GetDisplay()->ShowNotification(Lang::Strings::MUTED);
                                        });
    }

    void InitializeIot()
    {
        auto &thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Battery"));
    }

public:
    // ML307 UART 接收缓冲（传入 Ml307AtModem，驱动内为 size*2）。921600 下突发易 FIFO overflow，
    // overflow 会触发组件内 UDP 主动 Disconnect，表现为「未连接」洪泛；适当加大可缓解。
    TDi_300_PH() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, 8192),
                   boot_button_(BOOT_BUTTON_GPIO),
                   volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                   volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
                   ml307a_(ML307A_RX_PIN, ML307A_TX_PIN, 8192)
    {
        InitializePowerManager();
        
        InitializePowerSaveTimer();
        InitializeI2c();
        motor_.InitMotor(MOTOR_PWM_GPIO,MOTOR_PWM2_GPIO);
        motor_.OnResetCallback([&](bool result){
            auto& app = Application::GetInstance();
            app.NotifyResetResult(result);
        });
        // InitializeSpi();
#ifdef WBY_STYLE
        wby_led_.Initwbyled(MOTOR_PWM_GPIO, MOTOR_PWM2_GPIO);
        power_manager_->SetWbyLedOnProvider([this]() { return wby_led_.IsLedOn(); });
        wby_led_.OnLedStateChanged([this](bool /*on*/) {
            power_manager_->RefreshLowBatteryState();
        });
#else
        InitializeRc522();
#endif
        InitializeButtons();
        //motor_.motor_test();
        vTaskDelay(pdMS_TO_TICKS(50));
 
        
    }
    virtual void StartRfidScan() override
    {
        rc522_start(scanner);
    }
    virtual void StopRfidScan() override
    {
        rc522_pause(scanner);
    }
    virtual void StopMotorWork() override
    {
        motor_.SetSpeedLevel(0);
    }
    virtual void StopLedWork() override
    {
        wby_led_.stopwbyled();
    }
    virtual void OnEnterShowcaseMode() override
    {
        motor_.SetSpeedLevel(2);
        wby_led_.SetEffect("crossfade", 50, 85);
        //GetLed2()->OnStateChangedLed2();
    }
    virtual void OnExitShowcaseMode() override
    {
        StopMotorWork();
        StopLedWork();
        GetLed2()->OnStateChangedLed2();
    }
    virtual Led *GetLed() override
    {
        static CircularStrip led(BUILTIN_LED_GPIO, 5);
        return &led;
    }
    virtual Led *GetLed2() override
    {
        static CircularStrip led(LED_STRIP_GPIO, 42);
        return &led;
    }
    virtual AudioCodec *GetAudioCodec() override
    {
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
    virtual bool GetMotorSpeed(int &current_speed) override {
        current_speed = motor_.GetSpeed();
        return true;
    }
    virtual bool GetLedState(std::string& effect, int& speed_ms, int& intensity, int& white, int& yellow, int& blue) override {
        return wby_led_.GetLedState(effect, speed_ms, intensity, white, yellow, blue);

    }
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override
    {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual bool GetTemperature(float &esp32temp) override
    {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override
    {
        if (!enabled)
        {
            power_save_timer_->WakeUp();
        }
        DualNetworkBoard::SetPowerSaveMode(enabled);
    }
    
};
   
DECLARE_BOARD(TDi_300_PH);
