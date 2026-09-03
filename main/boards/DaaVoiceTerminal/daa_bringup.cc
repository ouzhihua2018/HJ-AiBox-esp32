#include "daa_bringup.h"
#include "config.h"
#include "audio_codecs/no_audio_codec.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <vector>
#include <cstdint>

#define TAG "DaaBringup"

static spi_device_handle_t s_spi = nullptr;
static NoAudioCodecDuplex* s_pcm = nullptr;

static constexpr float kPi = 3.14159265f;
static constexpr int kPcmFs = 8000;
static constexpr int kToneHz = 1000;
static constexpr int kChunk = 240;  // 对齐 DMA frame_num

static void HoldSclkHigh() {
    gpio_reset_pin(DAA_SPI_SCLK_GPIO);
    gpio_set_direction(DAA_SPI_SCLK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DAA_SPI_SCLK_GPIO, 1);
}

static void PulseReset() {
    gpio_reset_pin(DAA_RESET_GPIO);
    gpio_set_direction(DAA_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DAA_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(DAA_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void StartPcmClocks() {
    if (s_pcm == nullptr) {
        s_pcm = new NoAudioCodecDuplex(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            PCM_PCLK_GPIO,
            PCM_FSYNC_GPIO,
            PCM_DRX_GPIO,
            PCM_DTX_GPIO);
        s_pcm->Start();
        ESP_LOGI(TAG, "PCLK/PSYNC running (8 kHz FSYNC, 16-bit × 2 slots → 256 kHz PCLK)");
    }
}

static void InitSpiAfterReset() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = DAA_SPI_MOSI_GPIO;
    bus.miso_io_num = DAA_SPI_MISO_GPIO;
    bus.sclk_io_num = DAA_SPI_SCLK_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {};
    // Table 7: SDI sampled on SCLK rise → SPI mode 0. Idle low after RESET has already latched PCM+SPI.
    dev.mode = 0;
    dev.clock_speed_hz = 200 * 1000;
    dev.spics_io_num = DAA_SPI_CS_GPIO;
    dev.queue_size = 1;
    dev.cs_ena_pretrans = 2;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));
}

// Si3050 SPI 5.35: 16-bit host 看作两拍 16bit。
// 写: Control(0x20, CID=0) + Addr + Data + ignore
// 读: Control(0x60) + Addr + Data + Data 重复
static esp_err_t Si3050Write(uint8_t addr, uint8_t value) {
    spi_transaction_t t = {};
    t.flags = SPI_TRANS_USE_TXDATA;
    t.length = 32;
    t.tx_data[0] = 0x20;
    t.tx_data[1] = addr;
    t.tx_data[2] = value;
    t.tx_data[3] = 0x00;
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t Si3050Read(uint8_t addr, uint8_t* value) {
    spi_transaction_t t = {};
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 32;
    t.tx_data[0] = 0x60;
    t.tx_data[1] = addr;
    t.tx_data[2] = 0x00;
    t.tx_data[3] = 0x00;
    esp_err_t err = spi_device_transmit(s_spi, &t);
    if (err == ESP_OK) {
        *value = t.rx_data[2];
    }
    return err;
}

// 手册 5.32 / Register 33–37。必须先写 TXS/RXS 再置 PCME。
static void ConfigurePcmHighway() {
    // TXS=RXS=1：FSYNC 上升沿后再过 1 个 PCLK 才出 MSB（Figure 4，对 ESP32 I2S bit_shift）
    ESP_ERROR_CHECK(Si3050Write(34, 0x01));
    ESP_ERROR_CHECK(Si3050Write(35, 0x00));
    ESP_ERROR_CHECK(Si3050Write(36, 0x01));
    ESP_ERROR_CHECK(Si3050Write(37, 0x00));
    // REG33: PCME=1, PCMF=11 (16-bit linear), PHCF=0 (1 PCLK/bit), TRI=1, PCML=0
    ESP_ERROR_CHECK(Si3050Write(33, 0x39));
    uint8_t r33 = 0, r34 = 0, r36 = 0;
    Si3050Read(33, &r33);
    Si3050Read(34, &r34);
    Si3050Read(36, &r36);
    ESP_LOGI(TAG, "PCM cfg REG33=0x%02X (expect 0x39)  TXS=%u RXS=%u", r33, r34, r36);
}

static int ReadBatteryMilliVolts() {
    adc_oneshot_unit_handle_t adc = nullptr;
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &adc) != ESP_OK) {
        return -1;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &chan_cfg);

    int raw = 0;
    adc_oneshot_read(adc, ADC_CHANNEL_3, &raw);
    adc_oneshot_del_unit(adc);
    // 分压比板级未标，先打 raw。满量程约 12 dB → ~3100 mV 口电压。
    return raw;
}

static void Gate0() {
    ESP_LOGI(TAG, "======== GATE 0  上电 / 不插电话线 ========");
    ESP_LOGI(TAG, "EN 硬件接高。RESET=IO10 先拉低，芯片保持安全挂机。");
    gpio_reset_pin(DAA_RESET_GPIO);
    gpio_set_direction(DAA_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DAA_RESET_GPIO, 0);

    int adc_raw = ReadBatteryMilliVolts();
    ESP_LOGI(TAG, "VBAT ADC IO4 raw=%d  (分压系数标定前只作有无变化)", adc_raw);
    ESP_LOGI(TAG, "请万用表量 Si3050 VDD=3.3V，DGND 与 IGND 不短路。");
    ESP_LOGI(TAG, "GATE0 停在这里。menuconfig 把 DAA_BRINGUP_GATE 改成 1 再进复位+SPI。");
}

static float Rms(const int16_t* x, int n) {
    double acc = 0;
    for (int i = 0; i < n; ++i) {
        acc += static_cast<double>(x[i]) * x[i];
    }
    return static_cast<float>(std::sqrt(acc / n));
}

// Goertzel：看收回来的能量是不是集中在 tone_hz，而不是一堆噪声。
static float GoertzelPower(const int16_t* x, int n, float tone_hz) {
    const int k = static_cast<int>(n * tone_hz / kPcmFs + 0.5f);
    const float w = 2.f * kPi * static_cast<float>(k) / static_cast<float>(n);
    const float coeff = 2.f * std::cos(w);
    float s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        s0 = static_cast<float>(x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static bool Gate1() {
    ESP_LOGI(TAG, "======== GATE 1  复位进 PCM+SPI，读寄存器 ========");
    ESP_LOGI(TAG, "顺序: PCLK先跑 → SCLK=1 → RESET 低≥1ms → RESET↑ → 再开 SPI");

    gpio_reset_pin(GPIO_NUM_39);
    gpio_reset_pin(GPIO_NUM_40);
    gpio_reset_pin(GPIO_NUM_41);
    gpio_reset_pin(GPIO_NUM_42);

    HoldSclkHigh();
    StartPcmClocks();
    vTaskDelay(pdMS_TO_TICKS(5));
    PulseReset();   //复位后，I2S PCLK已经跑超过10个bit，释放时SCLK为高，进入SPI+PCM模式
    InitSpiAfterReset();

    uint8_t r6 = 0xFF, r11 = 0xFF, r12 = 0xFF;
    bool ok = true;
    for (int i = 0; i < 5; ++i) {
        uint8_t a = 0xFF, b = 0xFF, c = 0xFF;
        if (Si3050Read(6, &a) != ESP_OK || Si3050Read(11, &b) != ESP_OK || Si3050Read(12, &c) != ESP_OK) {
            ESP_LOGE(TAG, "SPI 传输失败");
            ok = false;
            break;
        }
        ESP_LOGI(TAG, "read[%d]  REG6=0x%02X  REG11=0x%02X  REG12=0x%02X", i, a, b, c);
        if (i == 0) {
            r6 = a;
            r11 = b;
            r12 = c;
        } else if (a != r6 || b != r11 || c != r12) {
            ESP_LOGW(TAG, "连续读不一致");
            ok = false;
        }
    }

    if ((r6 == 0xFF && r11 == 0xFF && r12 == 0xFF) || (r6 == 0x00 && r11 == 0x00 && r12 == 0x00)) {
        ESP_LOGE(TAG, "FAIL: 总线像没挂上（全 0 或全 1）。核 SDI/SDO/CS/SCLK，以及 RESET↑ 时 SCLK 是否为高。");
        ok = false;
    }
    if ((r6 & 0x10) == 0) {
        ESP_LOGW(TAG, "REG6.PDL 不是 1。复位默认应为 PDL=1。核对 SPI 读写相位。");
    }

    ESP_LOGI(TAG, "%s  REG6=0x%02X REG11=0x%02X REG12=0x%02X",
             ok ? "GATE1 PASS" : "GATE1 FAIL", r6, r11, r12);
    if (ok) {
        ConfigurePcmHighway();
    }
    return ok;
}

// 手册 5.7：PDL=1（复位默认）时线路侧掉电，DRX 经片内 TX/RX 滤波器回到 DTX。
// 约 0.9 dB 衰减 + 群时延。不要写 OH，不要清 PDL。
static void Gate2() {
    ESP_LOGI(TAG, "======== GATE 2  PDL=1 数字回环（不插电话线） ========");
    ESP_LOGI(TAG, "IO14 DOUT→DRX → 片内滤波回环 → DTX→IO21 DIN。不经过 Si3019。");

    if (s_pcm == nullptr) {
        ESP_LOGE(TAG, "GATE2 FAIL: I2S 未启动");
        return;
    }

    uint8_t r6 = 0, r10 = 0, r33 = 0;
    Si3050Read(6, &r6);
    Si3050Read(10, &r10);
    Si3050Read(33, &r33);
    ESP_LOGI(TAG, "REG6=0x%02X (PDL 应为 1)  REG10=0x%02X (DDL=0，走滤波器回环)  REG33=0x%02X",
             r6, r10, r33);
    if ((r6 & 0x10) == 0) {
        ESP_LOGE(TAG, "GATE2 FAIL: PDL 已被清掉，DTX 不再走系统侧回环。回到 Gate 1，不要写 REG6=0。");
        return;
    }
    if ((r33 & 0x20) == 0) {
        ESP_LOGE(TAG, "GATE2 FAIL: PCME 未打开。Gate 1 应已写 REG33=0x39。");
        return;
    }

    const int amp = 8000;
    const int warmup_chunks = 10;   // 丢掉 DMA + FIR 群时延里的旧数
    const int measure_chunks = 8;
    const int measure_n = measure_chunks * kChunk;
    std::vector<int16_t> tx(kChunk);
    std::vector<int16_t> rx(kChunk);
    std::vector<int16_t> captured;
    captured.reserve(measure_n);

    int phase = 0;
    double tx_sq = 0;
    int tx_n = 0;

    for (int c = 0; c < warmup_chunks + measure_chunks; ++c) {
        for (int i = 0; i < kChunk; ++i) {
            float s = std::sin(2.f * kPi * static_cast<float>(kToneHz) * static_cast<float>(phase) /
                               static_cast<float>(kPcmFs));
            tx[i] = static_cast<int16_t>(amp * s);
            ++phase;
        }
        s_pcm->OutputData(tx);
        if (!s_pcm->InputData(rx)) {
            ESP_LOGE(TAG, "GATE2 FAIL: I2S 读失败");
            return;
        }
        if (c >= warmup_chunks) {
            captured.insert(captured.end(), rx.begin(), rx.end());
            for (int i = 0; i < kChunk; ++i) {
                tx_sq += static_cast<double>(tx[i]) * tx[i];
            }
            tx_n += kChunk;
        }
    }

    const float tx_rms = static_cast<float>(std::sqrt(tx_sq / tx_n));
    const float rx_rms = Rms(captured.data(), static_cast<int>(captured.size()));
    const float p1k = GoertzelPower(captured.data(), static_cast<int>(captured.size()), 1000.f);
    const float p2k = GoertzelPower(captured.data(), static_cast<int>(captured.size()), 2000.f);
    int16_t peak = 0;
    for (int16_t v : captured) {
        int16_t a = v < 0 ? static_cast<int16_t>(-v) : v;
        if (a > peak) {
            peak = a;
        }
    }

    ESP_LOGI(TAG, "LOOPBACK  TX RMS=%.1f  RX RMS=%.1f  peak=%d  1kHz=%.3e  2kHz=%.3e",
             tx_rms, rx_rms, peak, p1k, p2k);
    ESP_LOGI(TAG, "RX 头 8 样点: %d %d %d %d %d %d %d %d",
             captured[0], captured[1], captured[2], captured[3],
             captured[4], captured[5], captured[6], captured[7]);

    // 滤波器约 0.9 dB → RMS 约 0.9×。对齐错时 RMS 接近噪声，或 1 kHz 不比 2 kHz 强。
    const bool energy_ok = (rx_rms > 400.f) && (rx_rms > 0.2f * tx_rms);
    const bool tone_ok = (p1k > 8.f * p2k) && (p1k > 1e10f);
    if (energy_ok && tone_ok) {
        ESP_LOGI(TAG, "GATE2 PASS  1 kHz 从 DTX 回来了。PCM 四根线 + TXS/RXS=1 对齐可用。");
    } else {
        ESP_LOGE(TAG, "GATE2 FAIL  能量或频率不对。查 IO14/21 是否交叉、PCLK/FSYNC、bit_shift/TXS、REG33。");
        if (!energy_ok) {
            ESP_LOGE(TAG, "  RMS 太小：多半是 DRX/DTX 接反或 DTX 一直高阻（PCME 没开）。");
        }
        if (!tone_ok) {
            ESP_LOGE(TAG, "  有能量但不是 1 kHz：时序错位（TXS 或 bit_shift），样点被撕碎。");
        }
    }
}

void DaaBringupRun(int gate) {
    ESP_LOGW(TAG, "Bring-up gate=%d  (文档: docs/daa-bringup-test.html)", gate);
    ESP_LOGI(TAG, "PCM PCLK=IO38 PSYNC=IO39 DRX=IO14 DTX=IO21");
    ESP_LOGI(TAG, "SPI SCLK=IO11 SDI=IO41 SDO=IO42 CS=IO40  RESET=IO10 RGDT=IO12 INT=IO13  VBAT=IO4");

    if (gate <= 0) {
        Gate0();
        return;
    }
    const bool gate1_ok = Gate1();
    if (gate == 1) {
        return;
    }
    if (!gate1_ok) {
        ESP_LOGE(TAG, "Gate 1 未过，不进入 Gate %d。先把 SPI 读通。", gate);
        return;
    }
    if (gate >= 2) {
        Gate2();
    }
    if (gate == 2) {
        return;
    }
    ESP_LOGW(TAG, "Gate %d 尚未实现。停在 Gate 2。先确认回环再往下。", gate);
}
