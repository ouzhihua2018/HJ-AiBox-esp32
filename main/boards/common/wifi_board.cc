#include "wifi_board.h"

#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "audio_codec.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_mqtt.h>
#include <esp_udp.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>

static const char *TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterWifiConfigMode() {
    auto& application = Application::GetInstance();
    ESP_LOGI(TAG, "enter wificonfig");

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    wifi_ap.SetSsidPrefix("DaaVoice");
    wifi_ap.Start();

    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_ap.GetSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_ap.GetWebServerUrl();

    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str());
    vTaskDelay(pdMS_TO_TICKS(200));
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    while (true) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
        ESP_LOGI(TAG, "WiFi AP: %s", hint.c_str());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void WifiBoard::StartNetwork() {
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([]() {
        ESP_LOGI(TAG, "%s", Lang::Strings::SCANNING_WIFI);
    });
    wifi_station.OnConnect([](const std::string& ssid) {
        ESP_LOGI(TAG, "%s%s...", Lang::Strings::CONNECT_TO, ssid.c_str());
    });
    wifi_station.OnConnected([](const std::string& ssid) {
        ESP_LOGI(TAG, "%s%s", Lang::Strings::CONNECTED_TO, ssid.c_str());
    });
    wifi_station.Start();

    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
}

Http* WifiBoard::CreateHttp() {
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    if (url.find("wss://") == 0) {
        return new WebSocket(new TlsTransport());
    }
    return new WebSocket(new TcpTransport());
}

Mqtt* WifiBoard::CreateMqtt() {
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    return "";
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    if (!wifi_config_mode_) {
        board_json += "\"ssid\":\"" + wifi_station.GetSsid() + "\",";
        board_json += "\"rssi\":" + std::to_string(wifi_station.GetRssi()) + ",";
        board_json += "\"channel\":" + std::to_string(wifi_station.GetChannel()) + ",";
        board_json += "\"ip\":\"" + wifi_station.GetIpAddress() + "\",";
    }
    board_json += "\"mac\":\"" + SystemInfo::GetMacAddress() + "\"}";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    ESP_LOGI(TAG, "%s", Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    auto audio_line = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_line, "volume", audio_codec->output_volume());
        cJSON_AddNumberToObject(audio_line, "input_rate", audio_codec->input_sample_rate());
        cJSON_AddNumberToObject(audio_line, "output_rate", audio_codec->output_sample_rate());
    }
    cJSON_AddItemToObject(root, "audio_line", audio_line);

    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
