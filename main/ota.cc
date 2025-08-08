#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        // 使用指定的服务器地址
        url = "http://8.142.109.195:30301/xiaozhi/ota2/";
    }
    return url;
}

std::string Ota::BuildOtaRequestJson() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();
    
    // 根据OTA接口协议构建请求JSON
    std::string json = "{";
    
    // 基本设备信息
    json += "\"deviceId\":\"" + SystemInfo::GetMacAddress() + "\",";
    json += "\"clientId\":\"" + board.GetUuid() + "\",";
    json += "\"version\":\"" + std::string(app_desc->version) + "\",";
    json += "\"boardType\":\"" + board.GetBoardType() + "\",";
    
    // 硬件信息
    json += "\"chipModel\":\"" + SystemInfo::GetChipModelName() + "\",";
    json += "\"flashSize\":" + std::to_string(SystemInfo::GetFlashSize()) + ",";
    json += "\"freeHeap\":" + std::to_string(SystemInfo::GetMinimumFreeHeapSize()) + ",";
    
    // 应用信息
    json += "\"appName\":\"" + std::string(app_desc->project_name) + "\",";
    json += "\"compileTime\":\"" + std::string(app_desc->date) + "T" + std::string(app_desc->time) + "Z\",";
    json += "\"idfVersion\":\"" + std::string(app_desc->idf_ver) + "\",";
    
    // 语言和其他配置
    json += "\"language\":\"" + std::string(Lang::CODE) + "\",";
    
    // 如果有序列号，添加序列号
    if (has_serial_number_) {
        json += "\"serialNumber\":\"" + serial_number_ + "\",";
    }
    
    // 移除最后的逗号并闭合JSON
    if (json.back() == ',') {
        json.pop_back();
    }
    json += "}";
    
    return json;
}

bool Ota::GetQRCodeInfoOnly() {
    ESP_LOGI(TAG, "=== Getting QR Code Info Only (No Firmware Check) ===");
    auto& board = Board::GetInstance();
    std::string url = GetCheckVersionUrl();
    
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return false;
    }

    ESP_LOGI(TAG, "QR Code Info Request URL: %s", url.c_str());
    
    auto http = std::unique_ptr<Http>(SetupHttp());
   // std::string data = BuildOtaRequestJson();
    std::string data = board.GetJson();
    ESP_LOGI(TAG, "QR Code Info Request JSON: %s", data.c_str());
    
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    ESP_LOGI(TAG, "Sending %s request for QR code info...", method.c_str());
    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to %s", url.c_str());
        return false;
    }

    auto status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "QR Code Info server response status: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to get QR code info, status code: %d", status_code);
        std::string error_response = http->ReadAll();
        ESP_LOGE(TAG, "Error response: %s", error_response.c_str());
        http->Close();
        return false;
    }

    std::string response_data = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "QR Code Info Response: %s", response_data.c_str()[0]);
  
    // 解析响应，但只关注二维码和激活信息，忽略固件版本
    cJSON *root = cJSON_Parse(response_data.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse QR code info response JSON");
        return false;
    }

    // 解析激活相关信息
    ParseActivationInfo(root);
    
    // 解析二维码信息
    ParseWeChatQRCode(root);
    
    // 解析MQTT和WebSocket配置（如果有的话）
    ParseMqttConfig(root);
    ParseWebSocketConfig(root);

    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "✅ QR Code Info retrieved successfully");
    return true;
}

Http* Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    auto http = board.CreateHttp();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
    }
    http->SetHeader("User-Agent", std::string(BOARD_NAME "/") + app_desc->version);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
bool Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return false;
    }

    ESP_LOGI(TAG, "=== OTA Request Details ===");
    ESP_LOGI(TAG, "URL: %s", url.c_str());
    ESP_LOGI(TAG, "Device-Id: %s", SystemInfo::GetMacAddress().c_str());
    ESP_LOGI(TAG, "Client-Id: %s", board.GetUuid().c_str());

    auto http = std::unique_ptr<Http>(SetupHttp());

    // 根据OTA接口协议构建请求数据
    std::string data = BuildOtaRequestJson(); 
    ESP_LOGI(TAG, "OTA Request JSON length: %d bytes", data.length());
    ESP_LOGI(TAG, "OTA Request JSON: %s", data.c_str());
    
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    ESP_LOGI(TAG, "Sending %s request to OTA server...", method.c_str());
    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to %s", url.c_str());
        return false;
    }

    auto status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "OTA server response status: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        // 读取错误响应内容用于调试
        std::string error_response = http->ReadAll();
        ESP_LOGE(TAG, "Error response: %s", error_response.c_str());
        http->Close();
        return false;
    }

    data = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "OTA server response length: %d bytes", data.length());
    ESP_LOGI(TAG, "OTA server response: %s", data.c_str());

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                settings.SetString(item->string, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                settings.SetInt(item->string, item->valueint);
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    // 解析 weChat 节点（二维码链接）- 从OTA响应中直接获取
    ESP_LOGI(TAG, "=== Parsing WeChat QR Code URL ===");
    cJSON *wechat = cJSON_GetObjectItem(root, "weChat");
    if (cJSON_IsObject(wechat)) {
        ESP_LOGI(TAG, "✅ Found weChat section in OTA response");
        cJSON *codeUrl = cJSON_GetObjectItem(wechat, "codeUrl");
        if (cJSON_IsString(codeUrl) && codeUrl->valuestring != nullptr) {
            wechat_code_url_ = codeUrl->valuestring;
            if (!wechat_code_url_.empty()) {
                ESP_LOGI(TAG, "✅ Got WeChat QR code URL: %s", wechat_code_url_.c_str());
                ESP_LOGI(TAG, "QR code URL length: %d characters", wechat_code_url_.length());
                
                // 验证URL格式
                if (wechat_code_url_.find("http") == 0 && wechat_code_url_.find(".png") != std::string::npos) {
                    ESP_LOGI(TAG, "✅ QR code URL format validation passed");
                } else {
                    ESP_LOGW(TAG, "⚠️  QR code URL format may be invalid");
                }
            } else {
                ESP_LOGW(TAG, "⚠️  QR code URL is empty string");
                wechat_code_url_.clear();
            }
        } else {
            ESP_LOGW(TAG, "⚠️  codeUrl field is not a valid string or is null");
        }
    } else {
        ESP_LOGI(TAG, "❌ No weChat section found in OTA response");
        ESP_LOGI(TAG, "Available JSON keys in response:");
        cJSON *item = root->child;
        while (item) {
            ESP_LOGI(TAG, "  - %s", item->string ? item->string : "null");
            item = item->next;
        }
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return true;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void Ota::Upgrade(const std::string& firmware_url) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto http = std::unique_ptr<Http>(Board::GetInstance().CreateHttp());
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return;
    }

    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (!image_header_checked) {
            image_header.append(buffer, ret);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));
                ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                auto current_version = esp_app_get_description()->version;
                if (memcmp(new_app_info.version, current_version, sizeof(new_app_info.version)) == 0) {
                    ESP_LOGE(TAG, "Firmware version is the same, skipping upgrade");
                    return;
                }

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    return;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }
        auto err = esp_ota_write(update_handle, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return;
        }
    }
    http->Close();

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful, rebooting in 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

void Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    upgrade_callback_ = callback;
    Upgrade(firmware_url_);
}

std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = std::unique_ptr<Http>(SetupHttp());

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}

bool Ota::DownloadAndDisplayQRCode() {
    ESP_LOGI(TAG, "=== Starting QR Code Download ===");
    
    if (wechat_code_url_.empty()) {
        ESP_LOGE(TAG, "❌ QR code URL is empty, make sure CheckVersion() was called first");
        return false;
    }

    ESP_LOGI(TAG, "QR code URL: %s", wechat_code_url_.c_str());
    ESP_LOGI(TAG, "URL length: %d characters", wechat_code_url_.length());

    // 验证URL格式
    if (wechat_code_url_.find("http") != 0) {
        ESP_LOGE(TAG, "❌ Invalid QR code URL format (missing http/https)");
        return false;
    }

    auto& board = Board::GetInstance();
    auto http = std::unique_ptr<Http>(board.CreateHttp());

    // 设置下载图片的请求头
    http->SetHeader("User-Agent", "ESP32-QRCode-Downloader/1.0");
    http->SetHeader("Accept", "image/png,image/*,*/*");
    http->SetHeader("Cache-Control", "no-cache");
    
    ESP_LOGI(TAG, "Opening HTTP connection for QR code download...");
    if (!http->Open("GET", wechat_code_url_)) {
        ESP_LOGE(TAG, "❌ Failed to open HTTP connection for QR code download");
        ESP_LOGE(TAG, "   URL: %s", wechat_code_url_.c_str());
        return false;
    }

    auto status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "QR code download response status: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "❌ Failed to download QR code, status code: %d", status_code);
        
        // 读取错误响应用于调试
        std::string error_response = http->ReadAll();
        if (!error_response.empty()) {
            ESP_LOGE(TAG, "Error response body: %s", error_response.c_str());
        }
        
        http->Close();
        return false;
    }

    // 读取图片数据
    ESP_LOGI(TAG, "Reading QR code image data...");
    std::string image_data = http->ReadAll();
    http->Close();

    if (image_data.empty()) {
        ESP_LOGE(TAG, "❌ Downloaded QR code image is empty");
        return false;
    }

    ESP_LOGI(TAG, "✅ QR code image downloaded successfully");
    ESP_LOGI(TAG, "Image size: %d bytes", image_data.length());

    // 验证PNG头部
    if (image_data.length() >= 8) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(image_data.data());
        // PNG文件头：89 50 4E 47 0D 0A 1A 0A
        if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
            ESP_LOGI(TAG, "✅ PNG header validation passed");
            ESP_LOGI(TAG, "PNG signature: %02X %02X %02X %02X %02X %02X %02X %02X", 
                    data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
        } else {
            ESP_LOGW(TAG, "⚠️  PNG header validation failed - may not be a valid PNG file");
            ESP_LOGW(TAG, "File signature: %02X %02X %02X %02X %02X %02X %02X %02X", 
                    data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
        }
    } else {
        ESP_LOGW(TAG, "⚠️  Image data too small to validate PNG header");
    }

    // 检查图片大小是否合理（QR码图片通常在1KB-50KB之间）
    if (image_data.length() < 500) {
        ESP_LOGW(TAG, "⚠️  Image size seems too small for a QR code (%d bytes)", image_data.length());
    } else if (image_data.length() > 100000) {
        ESP_LOGW(TAG, "⚠️  Image size seems too large for a QR code (%d bytes)", image_data.length());
    } else {
        ESP_LOGI(TAG, "✅ Image size is reasonable for a QR code");
    }
    
    // 将图片数据保存到成员变量中，供Application调用
    qr_image_data_ = image_data;
    
    ESP_LOGI(TAG, "=== QR Code Download Complete ===");
    return true;
}

bool Ota::TestQRCodeDownload(const std::string& test_url) {
    ESP_LOGI(TAG, "Testing QR code download with URL: %s", test_url.c_str());
    
    // 临时保存原URL
    std::string original_url = wechat_code_url_;
    
    // 设置测试URL
    wechat_code_url_ = test_url;
    
    // 执行下载和显示
    bool result = DownloadAndDisplayQRCode();
    
    // 恢复原URL
    wechat_code_url_ = original_url;
    
    ESP_LOGI(TAG, "QR code test %s", result ? "SUCCESS" : "FAILED");
    return result;
}

void Ota::ParseActivationInfo(cJSON* root) {
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }
}

void Ota::ParseWeChatQRCode(cJSON* root) {
    ESP_LOGI(TAG, "=== Parsing WeChat QR Code URL ===");
    cJSON *wechat = cJSON_GetObjectItem(root, "weChat");
    if (cJSON_IsObject(wechat)) {
        ESP_LOGI(TAG, "✅ Found weChat section in response");
        cJSON *codeUrl = cJSON_GetObjectItem(wechat, "codeUrl");
        if (cJSON_IsString(codeUrl) && codeUrl->valuestring != nullptr) {
            wechat_code_url_ = codeUrl->valuestring;
            if (!wechat_code_url_.empty()) {
                ESP_LOGI(TAG, "✅ Got WeChat QR code URL: %s", wechat_code_url_.c_str());
                ESP_LOGI(TAG, "QR code URL length: %d characters", wechat_code_url_.length());
                
                // 验证URL格式
                if (wechat_code_url_.find("http") == 0 && wechat_code_url_.find(".png") != std::string::npos) {
                    ESP_LOGI(TAG, "✅ QR code URL format validation passed");
                } else {
                    ESP_LOGW(TAG, "⚠️  QR code URL format may be invalid");
                }
            } else {
                ESP_LOGW(TAG, "⚠️  QR code URL is empty string");
                wechat_code_url_.clear();
            }
        } else {
            ESP_LOGW(TAG, "⚠️  codeUrl field is not a valid string or is null");
        }
    } else {
        ESP_LOGI(TAG, "❌ No weChat section found in response");
    }
}

void Ota::ParseMqttConfig(cJSON* root) {
    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found");
    }
}

void Ota::ParseWebSocketConfig(cJSON* root) {
    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                settings.SetString(item->string, item->valuestring);
            } else if (cJSON_IsNumber(item)) {
                settings.SetInt(item->string, item->valueint);
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found");
    }
}


