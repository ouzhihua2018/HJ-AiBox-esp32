#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include <wifi_station.h>

#define TAG "DaaVoiceTerminal"

class DaaVoiceTerminal : public WifiBoard {
private:
    Button boot_button_;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            ResetWifiConfiguration();
        });
    }

public:
    DaaVoiceTerminal() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeButtons();
        ESP_LOGI(TAG, "Board ready (PCM I2S placeholder, DAA SPI not started)");
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            PCM_PCLK_GPIO,
            PCM_FSYNC_GPIO,
            PCM_DRX_GPIO,
            PCM_DTX_GPIO);
        return &codec;
    }
};

DECLARE_BOARD(DaaVoiceTerminal);
