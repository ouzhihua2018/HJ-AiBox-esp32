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
#define TAG "TDi-300-PH-MainBoard"

static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = event->picc;
    auto &app = Application::GetInstance();

    if (picc->state == RC522_PICC_STATE_ACTIVE)
    {
        rc522_picc_print(picc);
        app.ResetDecoder();
        app.PlaySound(Lang::Sounds::P3_POPUP);
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
    esp_timer_handle_t angle_read_timer_handle_;
    int adc_raw_value_;
    adc_oneshot_unit_handle_t adc1_handle_;
    bool adc_initialized_;
    bool auto_motor_control_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    PowerSaveTimer *power_save_timer_;
    PowerManager *power_manager_;
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    motor motor_;
   
    esp_timer_handle_t* out_handle;
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

    // 延迟初始化ADC，避免在LEDC初始化之前影响RTC时钟
    void LazyInitializeAdc()
    {
        if (adc_initialized_)
        {
            ESP_LOGI(TAG, "ADC已经初始化，跳过重复初始化");
            return;
        }

        ESP_LOGI(TAG, "开始初始化ADC1 (GPIO4 -> ADC1_CHANNEL_3)...");

        // 获取ADC1单元实例
        //  注意：使用RC_FAST时钟源可能会影响RTC时钟，因此必须在LEDC初始化之后调用
        adc_oneshot_unit_init_cfg_t init_config1;
        init_config1.clk_src = ADC_RTC_CLK_SRC_RC_FAST;
        init_config1.unit_id = ADC_UNIT_1;
        init_config1.ulp_mode = ADC_ULP_MODE_DISABLE;

        esp_err_t ret = adc_oneshot_new_unit(&init_config1, &adc1_handle_);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC1单元创建失败：%s (0x%x)", esp_err_to_name(ret), ret);
            return;
        }
        ESP_LOGI(TAG, "ADC1单元创建成功");

        // 配置ADC1通道 - GPIO4对应ADC1_CHANNEL_3 (用于电位器角度检测)
        adc_oneshot_chan_cfg_t adc1_config;
        adc1_config.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc1_config.atten = ADC_ATTEN_DB_12; // 12dB衰减，支持0-3.3V输入

        ret = adc_oneshot_config_channel(adc1_handle_, ADC_CHANNEL_3, &adc1_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC1通道配置失败：%s (0x%x)", esp_err_to_name(ret), ret);
            adc_oneshot_del_unit(adc1_handle_);
            return;
        }

        adc_initialized_ = true;
        ESP_LOGI(TAG, "ADC1初始化成功 - GPIO4 -> ADC1_CHANNEL_3，用于电位器角度检测");
    }

    void InitializeAngleDetect()
    {

        adc_oneshot_unit_init_cfg_t init_config1;
        init_config1.clk_src = ADC_RTC_CLK_SRC_RC_FAST;
        init_config1.unit_id = ADC_UNIT_1;
        init_config1.ulp_mode = ADC_ULP_MODE_DISABLE;

        esp_err_t ret = adc_oneshot_new_unit(&init_config1, &adc1_handle_);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC1单元创建失败：%s (0x%x)", esp_err_to_name(ret), ret);
            return;
        }
        ESP_LOGI(TAG, "ADC1单元创建成功");

        // 配置ADC1通道 - GPIO4对应ADC1_CHANNEL_3 (用于电位器角度检测)
        adc_oneshot_chan_cfg_t adc1_config;
        adc1_config.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc1_config.atten = ADC_ATTEN_DB_12; // 12dB衰减，支持0-3.3V输入

        ret = adc_oneshot_config_channel(adc1_handle_, ADC_CHANNEL_3, &adc1_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC1通道配置失败：%s (0x%x)", esp_err_to_name(ret), ret);
            adc_oneshot_del_unit(adc1_handle_);
            return;
        }
        // 初始化角度传感定时器
        esp_timer_create_args_t angle_timer_args = {
            .callback = [](void *arg)
            {
                TDi_300_PH *this_ = (TDi_300_PH *)arg;
                if (this_->adc_initialized_)
                {
                    // 读取GPIO4上的电位器ADC值 (ADC1_CHANNEL_3)
                    esp_err_t ret = adc_oneshot_read(this_->adc1_handle_, ADC_CHANNEL_3, &(this_->adc_raw_value_));
                    if (ret != ESP_OK)
                    {
                        ESP_LOGE(TAG, "ADC读取失败：%s", esp_err_to_name(ret));
                        return;
                    }
                    // 定期输出ADC值用于调试（每10次输出一次，避免日志过多）
                    static int log_counter = 0;
                    if (++log_counter >= 10)
                    {
                        ESP_LOGI(TAG, "电位器ADC值: %d (范围: 0-4095)", this_->adc_raw_value_);
                        log_counter = 0;
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "ADC未初始化，无法读取电位器值");
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "angle_timer",
            .skip_unhandled_events = true};

        esp_err_t ret = esp_timer_create(&angle_timer_args, &angle_read_timer_handle_);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC定时器创建失败：%s (0x%x)", esp_err_to_name(ret), ret);
            return;
        }

        ret = esp_timer_start_periodic(angle_read_timer_handle_, 100 * 1000); // 每100ms读取一次
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC定时器启动失败：%s (0x%x)", esp_err_to_name(ret), ret);
            esp_timer_delete(angle_read_timer_handle_);
            return;
        }

        ESP_LOGI(TAG, "ADC角度检测定时器启动成功 (100ms间隔)");
    }

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
        rc522_start(scanner);
    }
    void InitializeButtons()
    {
        boot_button_.OnClick([this]()
                             {
            //power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            app.ToggleChatState(); });

        boot_button_.OnLongPress([this]()
                                 {
                                     auto &app = Application::GetInstance();
                                     ESP_LOGI(TAG, "BOOT TRIGGER");
                                     SwitchNetworkType();
                                     // ESP_LOGI(TAG,"当前LEVLE:%d",gpio_get_level(MOTOR_PWM_GPIO));
                                     // app.EmergencyWake();
                                 });
        boot_button_.OnMultipleClick([this]()
                                     {
            if (GetNetworkType() == NetworkType::WIFI) {
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.ResetWifiConfiguration();
            } }, 3);
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
    TDi_300_PH() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, 4096),
                   boot_button_(BOOT_BUTTON_GPIO),
                   volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                   volume_down_button_(VOLUME_DOWN_BUTTON_GPIO)
    {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeI2c();
        motor_.InitMotor(MOTOR_PWM_GPIO);

        // InitializeSpi();

        InitializeButtons();
        //motor_.motor_test();
        vTaskDelay(pdMS_TO_TICKS(50));
        InitializeAngleDetect();
        InitializeRc522();
        
    }

    virtual Led *GetLed() override
    {
        static CircularStrip led(BUILTIN_LED_GPIO, 2);
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
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
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
