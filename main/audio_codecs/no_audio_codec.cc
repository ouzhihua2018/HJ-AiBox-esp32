#include "no_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <vector>
#include <cstdint>

#define TAG "NoAudioCodec"

NoAudioCodec::~NoAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

NoAudioCodecDuplex::NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            // ESP-IDF std Philips 固定 2 个 slot。16-bit × 2 = 32 PCLK/帧 = 256 kHz，
            // 正好是 Si3050 合法 PCLK 下限。Si3050 16-bit linear 占前 16 bit，后 16 bit 空槽。
            // 不能改成单 slot（128 kHz），手册 Table 8 没有这一档。
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            // Si3050 只认 FSYNC 上升沿。标准 Philips 左槽 WS=低，上升沿会落到右槽。
            // 拉高左槽，使上升沿对准我们占用的 timeslot，再配 TXS=RXS=1。
            .ws_pol = true,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "PCM duplex: 16-bit slots, 8 kHz FSYNC, 256 kHz PCLK");
}

int NoAudioCodec::Write(const int16_t* data, int samples) {
    std::vector<int16_t> buffer(samples);
    int32_t volume_factor = static_cast<int32_t>(pow(double(output_volume_) / 100.0, 2) * 65536);
    for (int i = 0; i < samples; i++) {
        int32_t temp = (static_cast<int32_t>(data[i]) * volume_factor) >> 16;
        if (temp > INT16_MAX) {
            buffer[i] = INT16_MAX;
        } else if (temp < INT16_MIN) {
            buffer[i] = INT16_MIN;
        } else {
            buffer[i] = static_cast<int16_t>(temp);
        }
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int16_t), &bytes_written, portMAX_DELAY));
    return bytes_written / sizeof(int16_t);
}

int NoAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }
    return bytes_read / sizeof(int16_t);
}
