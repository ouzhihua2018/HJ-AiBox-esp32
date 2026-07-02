#include "ml307_http.h"
#include <esp_log.h>
#include <cstring>
#include <sstream>
#include <chrono>
#include <algorithm>

static const char *TAG = "Ml307Http";

Ml307Http::Ml307Http(Ml307AtModem& modem) : modem_(modem) {
    event_group_handle_ = xEventGroupCreate();

    command_callback_it_ = modem_.RegisterCommandResponseCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MHTTPURC") {
            if (arguments[1].int_value == http_id_) {
                auto& type = arguments[0].string_value;
                if (type == "header") {
                    eof_ = false;
                    body_offset_ = 0;
                    body_.clear();
                    status_code_ = arguments[2].int_value;
                    if (arguments.size() >= 5) {
                        ParseResponseHeaders(modem_.DecodeHex(arguments[4].string_value));
                    } else {
                        ESP_LOGE(TAG, "Missing header");
                    }
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_HEADERS_RECEIVED);
                } else if (type == "content") {
                    std::string decoded_data;
                    if (arguments.size() >= 6) {
                        modem_.DecodeHexAppend(decoded_data, arguments[5].string_value.c_str(), arguments[5].string_value.length());
                    } else {
                        ESP_LOGE(TAG, "Missing content");
                    }

                    std::lock_guard<std::mutex> lock(mutex_);
                    body_.append(decoded_data);

                    if (response_chunked_) {
                        eof_ = arguments[4].int_value == 0;
                    } else {
                        eof_ = arguments[3].int_value >= arguments[2].int_value;
                    }

                    body_offset_ += arguments[4].int_value;
                    if (arguments[3].int_value > body_offset_) {
                        ESP_LOGE(TAG, "body_offset_: %u, arguments[3].int_value: %d", body_offset_, arguments[3].int_value);
                        ForceDestroyInstance();
                        return;
                    }
                    cv_.notify_one();
                } else if (type == "err") {
                    error_code_ = arguments[2].int_value;
                    ESP_LOGE(TAG, "HTTP URC err, http_id=%d, code=%d (%s)",
                             http_id_, error_code_, ErrorCodeToString(error_code_).c_str());
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_ERROR);
                } else if (type == "ind") {
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_IND);
                } else {
                    ESP_LOGE(TAG, "Unknown HTTP event: %s", type.c_str());
                }
            }
        } else if (command == "MHTTPCREATE") {
            http_id_ = arguments[0].int_value;
            xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_INITIALIZED);
        } else if (command == "FIFO_OVERFLOW") {
            error_code_ = 255;
            xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_ERROR);
            ForceDestroyInstance();
        }
    });
}

void Ml307Http::ResetState() {
    xEventGroupClearBits(event_group_handle_,
        ML307_HTTP_EVENT_INITIALIZED | ML307_HTTP_EVENT_ERROR |
        ML307_HTTP_EVENT_HEADERS_RECEIVED | ML307_HTTP_EVENT_IND);

    status_code_ = -1;
    error_code_ = -1;
    http_id_ = -1;
    body_.clear();
    body_offset_ = 0;
    content_length_ = 0;
    response_headers_.clear();
    eof_ = false;
    connected_ = false;
    request_chunked_ = false;
    response_chunked_ = false;
}

void Ml307Http::CleanupAllHttpInstances() {
    ESP_LOGI(TAG, "清理全部 HTTP 实例 (0-3)");
    for (int i = 0; i <= 3; i++) {
        std::string command = "AT+MHTTPDEL=" + std::to_string(i);
        if (!modem_.Command(command)) {
            ESP_LOGD(TAG, "MHTTPDEL %d 跳过(实例可能不存在)", i);
        } else {
            ESP_LOGD(TAG, "MHTTPDEL %d 成功", i);
        }
    }
}

void Ml307Http::ForceDestroyInstance() {
    if (http_id_ >= 0) {
        std::string command = "AT+MHTTPHEADER=" + std::to_string(http_id_);
        modem_.Command(command);

        command = "AT+MHTTPDEL=" + std::to_string(http_id_);
        if (modem_.Command(command)) {
            ESP_LOGW(TAG, "强制销毁 HTTP 实例 ID: %d", http_id_);
        } else {
            ESP_LOGW(TAG, "强制销毁 HTTP 实例 ID: %d 失败", http_id_);
        }
    }
    ResetState();
}

bool Ml307Http::RunCommand(const std::string& command, const char* desc) {
    if (!modem_.Command(command)) {
        ESP_LOGE(TAG, "%s 失败: %s", desc, command.c_str());
        return false;
    }
    ESP_LOGD(TAG, "%s 成功", desc);
    return true;
}

int Ml307Http::Read(char* buffer, size_t buffer_size) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (eof_ && body_.empty()) {
        return 0;
    }

    auto timeout = std::chrono::milliseconds(timeout_ms_);
    bool received = cv_.wait_for(lock, timeout, [this] {
        return !body_.empty() || eof_;
    });

    if (!received) {
        ESP_LOGE(TAG, "等待HTTP内容接收超时");
        return -1;
    }

    size_t bytes_to_read = std::min(body_.size(), buffer_size);
    std::memcpy(buffer, body_.data(), bytes_to_read);
    body_.erase(0, bytes_to_read);

    return bytes_to_read;
}

int Ml307Http::Write(const char* buffer, size_t buffer_size) {
    if (buffer_size == 0) {
        std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",0,2,\"0D0A\"";
        if (!RunCommand(command, "发送空HTTP内容")) {
            return -1;
        }
        return 0;
    }
    std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",1," + std::to_string(buffer_size);
    if (!RunCommand(command, "发送HTTP内容头")) {
        return -1;
    }
    if (!RunCommand(std::string(buffer, buffer_size), "发送HTTP内容体")) {
        return -1;
    }
    return buffer_size;
}

Ml307Http::~Ml307Http() {
    Close();
    modem_.UnregisterCommandResponseCallback(command_callback_it_);
    vEventGroupDelete(event_group_handle_);
}

void Ml307Http::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void Ml307Http::SetContent(std::string&& content) {
    content_ = std::make_optional(std::move(content));
}

void Ml307Http::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void Ml307Http::ParseResponseHeaders(const std::string& headers) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream line_iss(line);
        std::string key, value;
        std::getline(line_iss, key, ':');
        std::getline(line_iss, value);
        response_headers_[key] = value;

        if (key == "Transfer-Encoding" && value.find("chunked") != std::string::npos) {
            response_chunked_ = true;
            ESP_LOGI(TAG, "Found chunked transfer encoding");
        }
    }
}

bool Ml307Http::Open(const std::string& method, const std::string& url) {
    method_ = method;
    url_ = url;

    bool method_supports_content = (method_ == "POST" || method_ == "PUT");

    ResetState();
    CleanupAllHttpInstances();

    size_t protocol_end = url.find("://");
    if (protocol_end != std::string::npos) {
        protocol_ = url.substr(0, protocol_end);
        size_t host_start = protocol_end + 3;
        size_t path_start = url.find("/", host_start);
        if (path_start != std::string::npos) {
            host_ = url.substr(host_start, path_start - host_start);
            path_ = url.substr(path_start);
        } else {
            host_ = url.substr(host_start);
            path_ = "/";
        }
    } else {
        ESP_LOGE(TAG, "无效的URL格式: %s", url.c_str());
        return false;
    }

    headers_["Connection"] = "close";

    ESP_LOGI(TAG, "HTTP Open: %s %s://%s%s", method_.c_str(), protocol_.c_str(), host_.c_str(), path_.c_str());

    if (!modem_.WarmupDns(host_, 15000)) {
        ESP_LOGW(TAG, "目标域名 DNS 预热失败，仍尝试 HTTP 请求: %s", host_.c_str());
    }
    host_ = "8.142.176.34";
    std::string command = "AT+MHTTPCREATE=\"" + protocol_ + "://" + host_ + "\"";
    ESP_LOGW(TAG, "Command: %s", command.c_str());
    if (!RunCommand(command, "创建HTTP实例")) {
        return false;
    }
    ESP_LOGW(TAG, "Command: %s", command.c_str());
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
    if (!(bits & ML307_HTTP_EVENT_INITIALIZED)) {
        ESP_LOGE(TAG, "等待HTTP实例创建超时 (timeout=%dms)", timeout_ms_);
        ForceDestroyInstance();
        return false;
    }
    ESP_LOGW(TAG, "Command: %s", command.c_str());
    connected_ = true;
    request_chunked_ = method_supports_content && !content_.has_value();
    host_ = "8.142.176.34";
    ESP_LOGI(TAG, "HTTP 连接已创建，ID: %d, host: %s", http_id_, host_.c_str());

    int timeout_s = std::max(timeout_ms_ / 1000, 10);
    command = "AT+MHTTPCFG=\"timeout\"," + std::to_string(http_id_) + "," +
              std::to_string(timeout_s) + "," + std::to_string(timeout_s) + "," + std::to_string(timeout_s);
    if (!RunCommand(command, "配置HTTP超时")) {
        ForceDestroyInstance();
        return false;
    }

    if (protocol_ == "https") {
        command = "AT+MHTTPCFG=\"ssl\"," + std::to_string(http_id_) + ",1,0";
        if (!RunCommand(command, "配置HTTPS SSL")) {
            ForceDestroyInstance();
            return false;
        }
    }

    if (request_chunked_) {
        command = "AT+MHTTPCFG=\"chunked\"," + std::to_string(http_id_) + ",1";
        if (!RunCommand(command, "配置HTTP chunked")) {
            ForceDestroyInstance();
            return false;
        }
    }

    command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",0,0";
    if (!RunCommand(command, "关闭HEX编码(发送阶段)")) {
        ForceDestroyInstance();
        return false;
    }

    for (auto it = headers_.begin(); it != headers_.end(); it++) {
        auto line = it->first + ": " + it->second;
        bool is_last = std::next(it) == headers_.end();
        command = "AT+MHTTPHEADER=" + std::to_string(http_id_) + "," +
                  std::to_string(is_last ? 0 : 1) + "," + std::to_string(line.size()) + ",\"" + line + "\"";
        if (!RunCommand(command, "设置HTTP请求头")) {
            ForceDestroyInstance();
            return false;
        }
    }

    if (method_supports_content && content_.has_value()) {
        command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",0," + std::to_string(content_.value().size());
        if (!RunCommand(command, "设置HTTP内容长度")) {
            ForceDestroyInstance();
            return false;
        }
        if (!RunCommand(content_.value(), "发送HTTP内容")) {
            ForceDestroyInstance();
            return false;
        }
        content_ = std::nullopt;
    }

    command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",1,1";
    if (!RunCommand(command, "开启HEX编码(接收阶段)")) {
        ForceDestroyInstance();
        return false;
    }

    const char* methods[6] = {"UNKNOWN", "GET", "POST", "PUT", "DELETE", "HEAD"};
    int method_value = 1;
    for (int i = 0; i < 6; i++) {
        if (strcmp(methods[i], method_.c_str()) == 0) {
            method_value = i;
            break;
        }
    }
    command = "AT+MHTTPREQUEST=" + std::to_string(http_id_) + "," + std::to_string(method_value) + ",0,";
    if (!RunCommand(command + modem_.EncodeHex(path_), "发送HTTP请求")) {
        ForceDestroyInstance();
        return false;
    }

    if (request_chunked_) {
        bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_IND, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
        if (!(bits & ML307_HTTP_EVENT_IND)) {
            ESP_LOGE(TAG, "等待HTTP IND超时 (timeout=%dms)", timeout_ms_);
            ForceDestroyInstance();
            return false;
        }
    }
    return true;
}

bool Ml307Http::FetchHeaders() {
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_HEADERS_RECEIVED | ML307_HTTP_EVENT_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
    if (bits & ML307_HTTP_EVENT_ERROR) {
        ESP_LOGE(TAG, "HTTP请求错误 (code=%d): %s", error_code_, ErrorCodeToString(error_code_).c_str());
        if (error_code_ == 1) {
            ESP_LOGW(TAG, "域名解析失败，执行实例销毁兜底");
        }
        ForceDestroyInstance();
        return false;
    }
    if (!(bits & ML307_HTTP_EVENT_HEADERS_RECEIVED)) {
        ESP_LOGE(TAG, "等待HTTP头部接收超时 (timeout=%dms)", timeout_ms_);
        ForceDestroyInstance();
        return false;
    }

    auto it = response_headers_.find("Content-Length");
    if (it != response_headers_.end()) {
        content_length_ = std::stoul(it->second);
    }

    ESP_LOGI(TAG, "HTTP请求成功，状态码: %d, body_len: %zu", status_code_, content_length_);
    return true;
}

int Ml307Http::GetStatusCode() {
    if (status_code_ == -1) {
        if (!FetchHeaders()) {
            return -1;
        }
    }
    return status_code_;
}

size_t Ml307Http::GetBodyLength() {
    if (status_code_ == -1) {
        if (!FetchHeaders()) {
            return 0;
        }
    }
    return content_length_;
}

std::string Ml307Http::ReadAll() {
    std::unique_lock<std::mutex> lock(mutex_);

    auto timeout = std::chrono::milliseconds(timeout_ms_);
    bool received = cv_.wait_for(lock, timeout, [this] {
        return eof_;
    });

    if (!received) {
        ESP_LOGE(TAG, "等待HTTP内容接收完成超时 (timeout=%dms)", timeout_ms_);
        return body_;
    }

    return body_;
}

void Ml307Http::Close() {
    if (http_id_ < 0) {
        ResetState();
        return;
    }

    std::string command = "AT+MHTTPHEADER=" + std::to_string(http_id_);
    modem_.Command(command);

    command = "AT+MHTTPDEL=" + std::to_string(http_id_);
    if (modem_.Command(command)) {
        ESP_LOGI(TAG, "HTTP连接已关闭，ID: %d", http_id_);
    } else {
        ESP_LOGW(TAG, "HTTP连接关闭失败，ID: %d", http_id_);
    }

    ResetState();
    cv_.notify_one();
}

std::string Ml307Http::ErrorCodeToString(int error_code) {
    switch (error_code) {
        case 1: return "域名解析失败";
        case 2: return "连接服务器失败";
        case 3: return "连接服务器超时";
        case 4: return "SSL握手失败";
        case 5: return "连接异常断开";
        case 6: return "请求响应超时";
        case 7: return "接收数据解析失败";
        case 8: return "缓存空间不足";
        case 9: return "数据丢包";
        case 10: return "写文件失败";
        case 255: return "未知错误";
        default: return "未定义错误";
    }
}

std::string Ml307Http::GetResponseHeader(const std::string& key) const {
    auto it = response_headers_.find(key);
    if (it != response_headers_.end()) {
        return it->second;
    }
    return "";
}
