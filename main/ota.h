#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

// 前向声明
struct cJSON;

class Ota {
public:
    Ota();
    ~Ota();

    bool CheckVersion();
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    void StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    const std::string& GetWeChatCodeUrl() const { return wechat_code_url_; } // 新增二维码链接获取方法
    bool HasWeChatCodeUrl() const { return !wechat_code_url_.empty(); }  // 检查是否有二维码链接
    bool DownloadAndDisplayQRCode();  // 下载并显示二维码
    bool TestQRCodeDownload(const std::string& test_url);  // 测试指定URL的二维码下载
    std::string  GetQRImageData() const { return qr_image_data_; }  // 获取下载的二维码图片数据
    std::string GetCheckVersionUrl();
    std::string BuildOtaRequestJson();  // 构建符合OTA接口协议的请求JSON
    bool GetQRCodeInfoOnly();  // 仅获取二维码和关联信息，不检查固件版本

private:
    void ParseActivationInfo(cJSON* root);  // 解析激活信息
    void ParseWeChatQRCode(cJSON* root);    // 解析二维码信息
    void ParseMqttConfig(cJSON* root);      // 解析MQTT配置
    void ParseWebSocketConfig(cJSON* root); // 解析WebSocket配置
    
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    std::string wechat_code_url_ = "http://8.142.109.195:30302/qrcode/927692b047a6f7-338f-4b57-a8c0-3f959e651126.png"; // 用于存储二维码链接
    std::string  qr_image_data_;   // 用于存储下载的二维码图片数据
    int activation_timeout_ms_ = 30000;

    void Upgrade(const std::string& firmware_url);
    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    Http* SetupHttp();
};

#endif // _OTA_H
