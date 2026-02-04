#include "dual_network_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "iot/thing_manager.h"
#include "led/circular_strip.h"
#include "assets/lang_config.h"
#include "driver/ledc.h"
#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include "esp_lcd_gc9a01.h"
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "TDi-300-PH"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_16_4);

class TDi_300_PH : public DualNetworkBoard {
private:
 
    esp_timer_handle_t angle_read_timer_handle_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    int adc_raw_value_;
    //LcdDisplay* display_;
    adc_oneshot_unit_handle_t adc1_handle_;
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }
 
//     void InitializeLcdDisplay() {
//         esp_lcd_panel_io_handle_t panel_io = nullptr;
//         esp_lcd_panel_handle_t panel = nullptr;
//         // 液晶屏控制IO初始化
//         ESP_LOGD(TAG, "Install panel IO");
//         esp_lcd_panel_io_spi_config_t io_config = {};
//         io_config.cs_gpio_num = DISPLAY_CS_PIN;
//         io_config.dc_gpio_num = DISPLAY_DC_PIN;
//         io_config.spi_mode = DISPLAY_SPI_MODE;
//         io_config.pclk_hz = 40 * 1000 * 1000;
//         io_config.trans_queue_depth = 10;
//         io_config.lcd_cmd_bits = 8;
//         io_config.lcd_param_bits = 8;
//         ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

//         // 初始化液晶屏驱动芯片
//         ESP_LOGD(TAG, "Install LCD driver");
//         esp_lcd_panel_dev_config_t panel_config = {};
//         panel_config.reset_gpio_num = DISPLAY_RST_PIN;
//         panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
//         panel_config.bits_per_pixel = 16;
// #if defined(LCD_TYPE_ILI9341_SERIAL)
//         ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
// #elif defined(LCD_TYPE_GC9A01_SERIAL)
//         ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
//         gc9a01_vendor_config_t gc9107_vendor_config = {
//             .init_cmds = gc9107_lcd_init_cmds,
//             .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
//         };        
// #else
//         ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
// #endif
        
//         esp_lcd_panel_reset(panel);
 

//         esp_lcd_panel_init(panel);
//         esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
//         ESP_LOGI(TAG,"invert_color:%d",DISPLAY_INVERT_COLOR);
//         ESP_LOGI(TAG,"SWAP_XY:%d",DISPLAY_SWAP_XY);
//         esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
//         esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
// #ifdef  LCD_TYPE_GC9A01_SERIAL
//         panel_config.vendor_config = &gc9107_vendor_config;
// #endif
//        display_ = new SpiLcdDisplay(panel_io, panel,
//                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
//                                     {
//                                         .text_font = &font_puhui_14_1,
//                                         .icon_font = &font_awesome_16_4,
// #if CONFIG_USE_WECHAT_MESSAGE_STYLE
//                                         .emoji_font = font_emoji_32_init(),
// #else
//                                         .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
// #endif
//                                     });
//     }

       void InitializeLedc(){
        ledc_timer_config_t timer_conf ;
        timer_conf.clk_cfg  = LEDC_AUTO_CLK;
        timer_conf.duty_resolution = LEDC_TIMER_13_BIT;
        timer_conf.freq_hz = 4883;
        timer_conf.speed_mode =  LEDC_LOW_SPEED_MODE;
        timer_conf.timer_num = LEDC_TIMER_2;
        ledc_timer_config(&timer_conf);
        ledc_channel_config_t ledc_conf;
        ledc_conf.channel = LEDC_CHANNEL_1;
        ledc_conf.duty = 1;
        ledc_conf.gpio_num = MOTOR_PWM_GPIO;
        ledc_conf.intr_type =  LEDC_INTR_DISABLE;
        ledc_conf.speed_mode = LEDC_LOW_SPEED_MODE;
        ledc_conf.timer_sel = LEDC_TIMER_2;
        ledc_channel_config(&ledc_conf);
        ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,0) ; 
        ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
    }
    void InitializeAngleDetect(){
        //获取ADC1单元实例
        adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle_));
        //配置ADC1
        adc_oneshot_chan_cfg_t adc1_config ;
        adc1_config.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc1_config.atten = ADC_ATTEN_DB_12;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle_, ADC_CHANNEL_0, &adc1_config));
        //ADC读取定时器
        esp_timer_create_args_t angle_timer_args = {
            .callback = [](void* arg) {
                TDi_300_PH* this_ = (TDi_300_PH*)arg;
                adc_oneshot_read(this_->adc1_handle_,ADC_CHANNEL_0,&(this_->adc_raw_value_));
                ESP_LOGI(TAG,"ADC RAW VALUE :%d" , this_->adc_raw_value_);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "angle_timer_args",
            .skip_unhandled_events = true
        };
        esp_timer_create(&angle_timer_args,
            &angle_read_timer_handle_);
        esp_timer_start_periodic(angle_read_timer_handle_,1*1000*1000); 
    }
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            app.ToggleChatState();
        });
        
        boot_button_.OnLongPress([this]() { 
            ESP_LOGI(TAG,"开转");
            ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,8191/2) ;  //占空比范围为0~2**分辨率，13对应 0~8192
            ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
            // auto& app = Application::GetInstance();
            // ESP_LOGI(TAG,"BOOT TRIGGER");
            //     //SwitchNetworkType();
          
            // app.EmergencyWake();
        });
        
        boot_button_.OnMultipleClick([this](){
            if (GetNetworkType() == NetworkType::WIFI) {
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.ResetWifiConfiguration();
            }
        },3);
        volume_up_button_.OnClick([this]() {
        uint32_t current_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
        ESP_LOGI(TAG,"当前占空比%ld",current_duty);
        // 优化阈值：8191-1638=6553，避免步长叠加超出最大值
        uint32_t target_duty = current_duty >= (8191 - 1638) ? 8191 : (current_duty + 1638);
        // 新增：强制校验，防止极端情况越界
        if (target_duty > 8191) target_duty = 8191;
        ESP_LOGI(TAG,"设置目标占空比%ld",target_duty);
        ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,target_duty) ; 
        ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
            // auto codec = GetAudioCodec();
            // auto& app = Application::GetInstance();
            // auto volume = codec->output_volume() + 10;
            // if (volume > 100) {
            //     app.PlaySound(Lang::Sounds::P3_LIMIT);
            //     volume = 100;
            // }else{
            //     app.PlaySound(Lang::Sounds::P3_PLUS);
            // }
            // codec->SetOutputVolume(volume);
            // GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));  

        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);

        });

        volume_down_button_.OnClick([this]() {
            uint32_t current_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
            ESP_LOGI(TAG,"当前占空比%ld",current_duty);
            uint32_t target_duty = current_duty<=1638 ? 0 : (current_duty-1638);
            ESP_LOGI(TAG,"设置目标占空比%ld",target_duty);
            ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,target_duty) ; 
            ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
            // auto codec = GetAudioCodec();
            // auto& app = Application::GetInstance();
            // auto volume = codec->output_volume() - 10;
            // if (volume < 0) {
            //     app.PlaySound(Lang::Sounds::P3_LIMIT);
            //     volume = 0;
            // } else{
            //     app.PlaySound(Lang::Sounds::P3_MINUS);
            // }
            // codec->SetOutputVolume(volume);
            // GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }


public:
        TDi_300_PH() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, 4096),
        boot_button_(BOOT_BUTTON_GPIO) ,
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO)
        {
            // ===== 新增：放在构造函数第一行，最早执行 =====
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << MOTOR_PWM_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;       // 强制为输出模式
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;  // 关闭内部弱上拉（关键！）
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE; // 开启内部下拉
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(MOTOR_PWM_GPIO, 0);     // 强制输出0V
        InitializeSpi();
        //InitializeLcdDisplay();
        InitializeLedc();
        InitializeButtons();
        InitializeAngleDetect();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        
    }   


    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 2);
        return &led;
    }
    virtual Led* GetLed2() override {
        static CircularStrip led(BUILTIN_LED2_GPIO, 41);
        return &led;
    }
    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    // virtual Display* GetDisplay() override {
    //     return display_;
    // }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(TDi_300_PH);
