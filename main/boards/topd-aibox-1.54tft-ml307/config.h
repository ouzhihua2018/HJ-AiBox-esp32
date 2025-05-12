
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_10
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

#define DISPLAY_SDA GPIO_NUM_41
#define DISPLAY_SCL GPIO_NUM_42
#define DISPLAY_RES GPIO_NUM_45
#define DISPLAY_DC GPIO_NUM_40
#define DISPLAY_CS GPIO_NUM_21

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define BACKLIGHT_INVERT false
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_20
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define ML307_RX_PIN GPIO_NUM_11
#define ML307_TX_PIN GPIO_NUM_12

#define USER_BUTTON_GPIO GPIO_NUM_17 // ASRPRO唤醒GPIO引脚

#define HUMAN_SENSOR_GPIO GPIO_NUM_18  // 人体传感器GPIO引脚

 // 配置GPIO为输入模式，上拉（根据模块输出特性可选）
    void init_human_sensor() {
    
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << HUMAN_SENSOR_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
    }

#endif // _BOARD_CONFIG_H_
