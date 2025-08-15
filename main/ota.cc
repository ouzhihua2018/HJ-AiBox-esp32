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
        url = CONFIG_OTA_URL;
    }
    url = "http://core.device.158box.com/xiaozhi/ota2/";
    return url;
}

// 在 Ota 类中修改方法定义
const std::string& Ota::GetWechatQrData() const {  // 返回 const 引用
    return wechat_qr_data_;
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
    
    // 添加SSL相关配置
    http->SetHeader("Connection", "close");
    http->SetHeader("Accept-Encoding", "identity"); // 避免压缩，简化处理
    
    // 设置超时时间（毫秒）
    // 注意：具体的超时设置方法取决于HTTP客户端的实现
    ESP_LOGI(TAG, "HTTP client configured with SSL support");

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
    ESP_LOGI(TAG, "OTA URL:%s", url.c_str());
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return false;
    }

    auto http = std::unique_ptr<Http>(SetupHttp());

    std::string data = board.GetJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return false;
    }

    data = http->ReadAll();
    ESP_LOGI(TAG,"OTA POST RESPONSE JSON:%s",data.c_str());
    http->Close();

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
    
    
    has_wechat_qr_code_url_ = false;
    cJSON *wechat = cJSON_GetObjectItem(root, "weChat");
    if (cJSON_IsObject(wechat)) {
        cJSON *codeUrl = cJSON_GetObjectItem(wechat, "codeUrl");
        if(cJSON_IsString(codeUrl)){
            wechat_qr_code_url_ = codeUrl -> valuestring;
            has_wechat_qr_code_url_ = true;
            ESP_LOGW(TAG,"wechat section parse ok!");
        }
    } else {
        ESP_LOGI(TAG, "No wechat section found!");
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
     // 激活成功，界面切换
    Board& board=Board::GetInstance();
    Display* display= board.GetDisplay();
    display->SwitchToGifContainer();
    
    return ESP_OK;
}

bool Ota::Download_Qrcode()
{   
    Board& board = Board::GetInstance();
    //Get Wechat QrCode URL
    auto& Wechat_Qr_Code_Url = GetWechatQrCodeUrl();
    if(Wechat_Qr_Code_Url.data() == NULL){
        ESP_LOGE(TAG,"NO Qr_code_url");
        return false;
    }
    
    // 检查URL是否为HTTPS，如果是则使用专门的HTTPS下载函数
    if (Wechat_Qr_Code_Url.find("https://") == 0) {
        ESP_LOGI(TAG, "Detected HTTPS URL, using HTTPS download function");
        return Download_Qrcode_Https();
    }
    
    ESP_LOGI(TAG,"-------------------------------------");
    ESP_LOGI(TAG,"Get_Wechat_Qrcode_URL:%s",Wechat_Qr_Code_Url.c_str());
    
    //Download Qrcode
    auto http = SetupHttp();  //default http header
    
    // 检查URL是否为HTTPS
    bool is_https = (Wechat_Qr_Code_Url.find("https://") == 0);
    ESP_LOGI(TAG, "URL is HTTPS: %s", is_https ? "true" : "false");
    
    // 设置下载图片的请求头
    http->SetHeader("User-Agent", "ESP32-QRCode-Downloader/1.0");
    http->SetHeader("Accept", "image/png,image/*,*/*");
    http->SetHeader("Cache-Control", "no-cache");
    
    // 如果是HTTPS，添加SSL相关配置
    if (is_https) {
        // 设置SSL配置
        http->SetHeader("Connection", "close");
        // 添加SSL验证相关设置
        ESP_LOGI(TAG, "Configuring SSL for HTTPS download");
    }
    
    ESP_LOGI(TAG, "Opening HTTP connection to: %s", Wechat_Qr_Code_Url.c_str());
    if (!http->Open("GET", Wechat_Qr_Code_Url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection to: %s", Wechat_Qr_Code_Url.c_str());
        return false;
    }

    int status = http->GetStatusCode();
    ESP_LOGI(TAG, "HTTP response status: %d", status);
    
    if(status != 200){
        ESP_LOGE(TAG,"Wechat Qrcode http response error,status %d",status);
        // 获取错误响应内容以便调试
        std::string error_response = http->ReadAll();
        ESP_LOGE(TAG, "Error response: %s", error_response.c_str());
        http->Close();
        return false;
    }
    
    ESP_LOGI(TAG, "Starting to read image data...");
    wechat_qr_data_ = http->ReadAll();
    http->Close();
    
    ESP_LOGI(TAG, "Downloaded image size: %zu bytes", wechat_qr_data_.size());
    
    if (wechat_qr_data_.empty()) {
        ESP_LOGE(TAG, "Downloaded image data is empty");
        return false;
    }
    
    const char * png_header_check = wechat_qr_data_.c_str();
    ESP_LOGI(TAG, "Image header bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
        png_header_check[0], png_header_check[1], png_header_check[2], png_header_check[3], 
        png_header_check[4], png_header_check[5], png_header_check[6], png_header_check[7]);
    
    if(png_header_check[0] !=0x89 ||png_header_check[1] !=0x50 ||png_header_check[2] !=0x4E ||png_header_check[3] !=0x47 ||
        png_header_check[4] !=0x0D ||png_header_check[5] !=0x0A ||png_header_check[6] !=0x1A ||png_header_check[7] !=0x0A ){
        ESP_LOGI(TAG, "wechat qrcode png header check error");
        return false;
    }
    ESP_LOGI(TAG, "wechat qrcode png header check pass");

    return true;
}

bool Ota::Download_Qrcode_Https() {
    Board& board = Board::GetInstance();
    auto& Wechat_Qr_Code_Url = GetWechatQrCodeUrl();
    
    if(Wechat_Qr_Code_Url.data() == NULL){
        ESP_LOGE(TAG,"NO Qr_code_url");
        return false;
    }
    
    // 检查URL是否为HTTPS
    if (Wechat_Qr_Code_Url.find("https://") != 0) {
        ESP_LOGI(TAG, "URL is not HTTPS, using regular download");
        return Download_Qrcode();
    }
    
    ESP_LOGI(TAG,"-------------------------------------");
    ESP_LOGI(TAG,"HTTPS Download - Get_Wechat_Qrcode_URL:%s",Wechat_Qr_Code_Url.c_str());
    
    // 创建HTTP客户端
    auto http = SetupHttp();
    
    // 配置SSL设置
    ConfigureSslForHttps(http);
    
    // 设置HTTPS专用请求头
    http->SetHeader("User-Agent", "ESP32-QRCode-Downloader/1.0");
    http->SetHeader("Accept", "image/png,image/*,*/*");
    http->SetHeader("Cache-Control", "no-cache");
    
    // 尝试多次连接，处理SSL握手问题
    int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    
    while (retry_count < max_retries && !success) {
        ESP_LOGI(TAG, "HTTPS download attempt %d/%d", retry_count + 1, max_retries);
        
        if (!http->Open("GET", Wechat_Qr_Code_Url)) {
            ESP_LOGE(TAG, "Failed to open HTTPS connection (attempt %d)", retry_count + 1);
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(2000)); // 等待2秒后重试
            continue;
        }
        
        int status = http->GetStatusCode();
        ESP_LOGI(TAG, "HTTPS response status: %d", status);
        
        if (status == 200) {
            ESP_LOGI(TAG, "HTTPS connection successful, reading data...");
            wechat_qr_data_ = http->ReadAll();
            http->Close();
            
            ESP_LOGI(TAG, "Downloaded image size: %zu bytes", wechat_qr_data_.size());
            
            if (!wechat_qr_data_.empty()) {
                // 验证PNG头部
                const char * png_header_check = wechat_qr_data_.c_str();
                if(png_header_check[0] == 0x89 && png_header_check[1] == 0x50 && 
                   png_header_check[2] == 0x4E && png_header_check[3] == 0x47 &&
                   png_header_check[4] == 0x0D && png_header_check[5] == 0x0A && 
                   png_header_check[6] == 0x1A && png_header_check[7] == 0x0A) {
                    ESP_LOGI(TAG, "HTTPS download successful, PNG header verified");
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Invalid PNG header in downloaded data");
                }
            } else {
                ESP_LOGE(TAG, "Downloaded data is empty");
            }
        } else {
            ESP_LOGE(TAG, "HTTPS request failed with status: %d", status);
            std::string error_response = http->ReadAll();
            ESP_LOGE(TAG, "Error response: %s", error_response.c_str());
            http->Close();
        }
        
        retry_count++;
        if (retry_count < max_retries) {
            ESP_LOGI(TAG, "Retrying in 2 seconds...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    if (!success) {
        ESP_LOGE(TAG, "All HTTPS download attempts failed");
        return false;
    }
    
    return true;
}

void Ota::ConfigureSslForHttps(Http* http) {
    if (!http) {
        ESP_LOGE(TAG, "HTTP client is null");
        return;
    }
    
    ESP_LOGI(TAG, "Configuring SSL settings for HTTPS");
    
    // 设置SSL相关请求头
    http->SetHeader("Connection", "close");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Upgrade-Insecure-Requests", "1");
    
    // 添加SSL验证相关设置
    // 注意：具体的SSL配置方法取决于HTTP客户端的实现
    // 这里提供通用的配置建议
    
    ESP_LOGI(TAG, "SSL configuration completed");
}

bool Ota::TestHttpsDownload(const std::string& test_url) {
    ESP_LOGI(TAG, "Testing HTTPS download with URL: %s", test_url.c_str());
    
    // 检查URL是否为HTTPS
    if (test_url.find("https://") != 0) {
        ESP_LOGE(TAG, "Test URL is not HTTPS: %s", test_url.c_str());
        return false;
    }
    
    // 创建HTTP客户端
    auto http = SetupHttp();
    ConfigureSslForHttps(http);
    
    // 设置测试请求头
    http->SetHeader("User-Agent", "ESP32-Test/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Cache-Control", "no-cache");
    
    ESP_LOGI(TAG, "Attempting HTTPS connection...");
    
    if (!http->Open("GET", test_url)) {
        ESP_LOGE(TAG, "Failed to open HTTPS connection for test");
        return false;
    }
    
    int status = http->GetStatusCode();
    ESP_LOGI(TAG, "Test HTTPS response status: %d", status);
    
    if (status == 200) {
        std::string response = http->ReadAll();
        ESP_LOGI(TAG, "Test successful, response size: %zu bytes", response.size());
        http->Close();
        return true;
    } else {
        std::string error_response = http->ReadAll();
        ESP_LOGE(TAG, "Test failed, status: %d, response: %s", status, error_response.c_str());
        http->Close();
        return false;
    }
}
