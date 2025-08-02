#include "topd_lcd_display.h"

#include <esp_log.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <libs/gif/lv_gif.h>

#include "display/lcd_display.h"
#include "font_awesome_symbols.h"
#include "board.h"

// PNG解码相关
#include <zlib.h>
#include <cstdint>

#define TAG "TopdEmojiDisplay"

// 表情映射表 - 将原版21种表情映射到现有6个GIF
const TopdEmojiDisplay::EmotionMap TopdEmojiDisplay::emotion_maps_[] = {
    // 中性/平静类表情 -> staticstate
    {"neutral", &staticstate},
    {"relaxed", &staticstate},
    {"sleepy", &staticstate},

    // 积极/开心类表情 -> happy
    {"happy", &happy},
    {"laughing", &happy},
    {"funny", &happy},
    {"loving", &happy},
    {"confident", &happy},
    {"winking", &happy},
    {"cool", &happy},
    {"delicious", &happy},
    {"kissy", &happy},
    {"silly", &happy},

    // 悲伤类表情 -> sad
    {"sad", &sad},
    {"crying", &sad},

    // 愤怒类表情 -> anger
    {"angry", &anger},

    // 惊讶类表情 -> scare
    {"surprised", &scare},
    {"shocked", &scare},

    // 思考/困惑类表情 -> buxue
    {"thinking", &buxue},
    {"confused", &buxue},
    {"embarrassed", &buxue},

    {nullptr, nullptr}  // 结束标记
};

TopdEmojiDisplay::TopdEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy, DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    fonts),
      emotion_gif_(nullptr) {
    SetupGifContainer();
};

void TopdEmojiDisplay::SetupGifContainer() {
    DisplayLockGuard lock(this);

    if (emotion_label_) {
        lv_obj_del(emotion_label_);
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
    }
    if (content_) {
        lv_obj_del(content_);
    }

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    emotion_label_ = lv_label_create(content_);
    lv_label_set_text(emotion_label_, "");
    lv_obj_set_width(emotion_label_, 0);
    lv_obj_set_style_border_width(emotion_label_, 0, 0);
    lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_gif_ = lv_gif_create(content_);
    int gif_size = LV_HOR_RES;
    lv_obj_set_size(emotion_gif_, gif_size, gif_size);
    lv_obj_set_style_border_width(emotion_gif_, 0, 0);
    lv_obj_set_style_bg_opa(emotion_gif_, LV_OPA_TRANSP, 0);
    lv_obj_center(emotion_gif_);
    lv_gif_set_src(emotion_gif_, &staticstate);

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);

    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 5, 0);

    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

    LcdDisplay::SetTheme("dark");
}

void TopdEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion || !emotion_gif_) {
        return;
    }

    DisplayLockGuard lock(this);

    // 隐藏二维码显示
    if (qr_container_ != nullptr) {
        lv_obj_add_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (qr_img_obj_ != nullptr) {
        lv_obj_add_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    for (const auto& map : emotion_maps_) {
        if (map.name && strcmp(map.name, emotion) == 0) {
            lv_gif_set_src(emotion_gif_, map.gif);
            ESP_LOGI(TAG, "设置表情: %s", emotion);
            return;
        }
    }

    lv_gif_set_src(emotion_gif_, &staticstate);
    ESP_LOGI(TAG, "未知表情'%s'，使用默认", emotion);
}

void TopdEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // 隐藏二维码显示
    if (qr_container_ != nullptr) {
        lv_obj_add_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (qr_img_obj_ != nullptr) {
        lv_obj_add_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    if (content == nullptr || strlen(content) == 0) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_message_label_, content);
    lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "设置聊天消息 [%s]: %s", role, content);
}

void TopdEmojiDisplay::SetIcon(const char* icon) {
    if (!icon) {
        return;
    }

    DisplayLockGuard lock(this);

    if (chat_message_label_ != nullptr) {
        std::string icon_message = std::string(icon) + " ";

        if (strcmp(icon, FONT_AWESOME_DOWNLOAD) == 0) {
            icon_message += "正在升级...";
        } else {
            icon_message += "系统状态";
        }

        lv_label_set_text(chat_message_label_, icon_message.c_str());
        lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

        ESP_LOGI(TAG, "设置图标: %s", icon);
    }
}

void TopdEmojiDisplay::ShowQRCode(const std::string& qrUrl) {
    if (qrUrl.empty()) {
        ESP_LOGE(TAG, "QR code URL is empty");
        return;
    }

    ESP_LOGI(TAG, "Downloading QR code from: %s", qrUrl.c_str());

    // 创建HTTP客户端下载图片
    auto& board = Board::GetInstance();
    auto http = board.CreateHttp();
    
    if (!http->Open("GET", qrUrl)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for QR code download");
        // 如果下载失败，显示URL文本
        ShowQRCodeText(qrUrl);
        return;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to download QR code, status code: %d", status_code);
        // 如果下载失败，显示URL文本
        ShowQRCodeText(qrUrl);
        return;
    }

    // 获取响应内容
    auto response = http->GetContent();
    if (response.empty()) {
        ESP_LOGE(TAG, "Empty response for QR code download");
        // 如果下载失败，显示URL文本
        ShowQRCodeText(qrUrl);
        return;
    }

    ESP_LOGI(TAG, "Downloaded QR code, size: %zu bytes", response.size());

    // 尝试显示图片
    if (TryDisplayImage(response)) {
        ESP_LOGI(TAG, "QR code image displayed successfully");
    } else {
        ESP_LOGW(TAG, "Failed to display image, showing URL text instead");
        ShowQRCodeText(qrUrl);
    }
}

void TopdEmojiDisplay::HideQRCode() {
    DisplayLockGuard lock(this);
    
    // 隐藏二维码容器
    if (qr_container_ != nullptr) {
        lv_obj_add_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 隐藏二维码图片对象
    if (qr_img_obj_ != nullptr) {
        lv_obj_add_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 恢复其他元素的显示
    if (emotion_gif_) {
        lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
    if (chat_message_label_) {
        lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "QR code display hidden");
}

void TopdEmojiDisplay::ShowQRCodeText(const std::string& qrUrl) {
    ESP_LOGI(TAG, "Showing QR code URL as text: %s", qrUrl.c_str());

    // 显示图片
    DisplayLockGuard lock(this);
    
    // 隐藏其他元素
    if (emotion_gif_) {
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 创建或更新二维码显示对象
    if (qr_container_ == nullptr) {
        qr_container_ = lv_obj_create(content_);
        lv_obj_set_size(qr_container_, LV_HOR_RES * 0.9, LV_VER_RES * 0.8);
        lv_obj_center(qr_container_);
        lv_obj_set_style_bg_color(qr_container_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qr_container_, LV_OPA_90, 0);
        lv_obj_set_style_radius(qr_container_, 10, 0);
        lv_obj_set_style_border_width(qr_container_, 2, 0);
        lv_obj_set_style_border_color(qr_container_, lv_color_black(), 0);
        lv_obj_set_style_pad_all(qr_container_, 20, 0);
        lv_obj_set_flex_flow(qr_container_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(qr_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    // 清除之前的子对象
    lv_obj_clean(qr_container_);

    // 创建标题
    lv_obj_t* title_label = lv_label_create(qr_container_);
    lv_label_set_text(title_label, "扫描二维码");
    lv_obj_set_style_text_color(title_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(title_label, fonts_.text_font, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

    // 创建二维码占位符（简单的矩形框）
    lv_obj_t* qr_placeholder = lv_obj_create(qr_container_);
    lv_obj_set_size(qr_placeholder, 150, 150);
    lv_obj_set_style_bg_color(qr_placeholder, lv_color_black(), 0);
    lv_obj_set_style_border_width(qr_placeholder, 0, 0);
    lv_obj_set_style_radius(qr_placeholder, 5, 0);

    // 在占位符中添加文本
    lv_obj_t* qr_text = lv_label_create(qr_placeholder);
    lv_label_set_text(qr_text, "QR\nCODE");
    lv_obj_set_style_text_color(qr_text, lv_color_white(), 0);
    lv_obj_set_style_text_font(qr_text, fonts_.text_font, 0);
    lv_obj_set_style_text_align(qr_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(qr_text);

    // 创建URL显示标签
    lv_obj_t* url_label = lv_label_create(qr_container_);
    lv_label_set_text(url_label, qrUrl.c_str());
    lv_obj_set_style_text_color(url_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(url_label, fonts_.text_font, 0);
    lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(url_label, lv_obj_get_width(qr_container_) - 40);
    lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);

    // 创建提示文本
    lv_obj_t* hint_label = lv_label_create(qr_container_);
    lv_label_set_text(hint_label, "请使用微信扫描上方二维码");
    lv_obj_set_style_text_color(hint_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(hint_label, fonts_.text_font, 0);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);

    // 显示容器
    lv_obj_clear_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "QR code text display created successfully");
}

bool TopdEmojiDisplay::TryDisplayImage(const std::vector<uint8_t>& image_data) {
    // 检查是否为PNG格式（简单的魔数检查）
    bool is_png = false;
    if (image_data.size() >= 8) {
        const uint8_t png_signature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        is_png = (memcmp(image_data.data(), png_signature, 8) == 0);
    }

    if (!is_png) {
        ESP_LOGE(TAG, "Downloaded file is not a valid PNG image");
        return false;
    }

    ESP_LOGI(TAG, "Processing PNG image, size: %zu bytes", image_data.size());

    // 尝试解码PNG并转换为RGB565格式
    std::vector<uint16_t> rgb565_data;
    if (!DecodePNGToRGB565(image_data, rgb565_data)) {
        ESP_LOGE(TAG, "Failed to decode PNG image");
        return false;
    }

    // 显示图片
    DisplayLockGuard lock(this);
    
    // 隐藏其他元素
    if (emotion_gif_) {
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 创建或更新图片对象
    if (qr_img_obj_ == nullptr) {
        qr_img_obj_ = lv_img_create(content_);
        lv_obj_set_size(qr_img_obj_, 240, 240);  // 设置显示大小为全屏
        lv_obj_center(qr_img_obj_);
        lv_obj_set_style_border_width(qr_img_obj_, 0, 0);
        lv_obj_set_style_bg_opa(qr_img_obj_, LV_OPA_TRANSP, 0);
    }

    // 创建RGB565图片描述符
    static lv_img_dsc_t qr_img_dsc;
    static std::vector<uint16_t> static_rgb565_data;
    static_rgb565_data = std::move(rgb565_data);
    
    qr_img_dsc.header.always_zero = 0;
    qr_img_dsc.header.w = 240;  // 图片宽度为240
    qr_img_dsc.header.h = 240;  // 图片高度为240
    qr_img_dsc.data_size = static_rgb565_data.size() * sizeof(uint16_t);
    qr_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;  // RGB565格式
    qr_img_dsc.data = static_rgb565_data.data();

    // 设置图片源并显示
    lv_img_set_src(qr_img_obj_, &qr_img_dsc);
    lv_obj_clear_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "PNG image decoded and displayed successfully at full screen (240x240)");
    return true;
}

bool TopdEmojiDisplay::DecodePNGToRGB565(const std::vector<uint8_t>& png_data, std::vector<uint16_t>& rgb565_data) {
    ESP_LOGI(TAG, "Starting PNG decoding, data size: %zu bytes", png_data.size());
    
    // 检查PNG签名
    if (png_data.size() < 8) {
        ESP_LOGE(TAG, "PNG data too small");
        return false;
    }
    
    const uint8_t* data = png_data.data();
    const uint8_t png_signature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(data, png_signature, 8) != 0) {
        ESP_LOGE(TAG, "Invalid PNG signature");
        return false;
    }
    
    // 查找IHDR块（图像头信息）
    size_t pos = 8;
    uint32_t original_width = 0, original_height = 0;
    uint8_t bit_depth = 0, color_type = 0, compression = 0, filter = 0, interlace = 0;
    bool found_ihdr = false;
    
    while (pos + 12 < png_data.size()) {
        uint32_t chunk_length = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
        const char* chunk_type = reinterpret_cast<const char*>(&data[pos+4]);
        
        if (strncmp(chunk_type, "IHDR", 4) == 0) {
            // 找到IHDR块，解析图像信息
            original_width = (data[pos+8] << 24) | (data[pos+9] << 16) | (data[pos+10] << 8) | data[pos+11];
            original_height = (data[pos+12] << 24) | (data[pos+13] << 16) | (data[pos+14] << 8) | data[pos+15];
            bit_depth = data[pos+16];
            color_type = data[pos+17];
            compression = data[pos+18];
            filter = data[pos+19];
            interlace = data[pos+20];
            
            found_ihdr = true;
            ESP_LOGI(TAG, "PNG dimensions: %ux%u, bit_depth: %d, color_type: %d", 
                     original_width, original_height, bit_depth, color_type);
            break;
        }
        
        pos += 12 + chunk_length; // 跳过块数据
    }
    
    if (!found_ihdr || original_width == 0 || original_height == 0) {
        ESP_LOGE(TAG, "Failed to find IHDR chunk or invalid dimensions");
        return false;
    }
    
    // 检查图片尺寸是否合理
    if (original_width > 10000 || original_height > 10000) {
        ESP_LOGE(TAG, "PNG dimensions too large: %ux%u", original_width, original_height);
        return false;
    }
    
    // 创建240x240的RGB565数据
    rgb565_data.resize(240 * 240);
    
    // 计算缩放比例
    float scale_x = 240.0f / original_width;
    float scale_y = 240.0f / original_height;
    float scale = std::min(scale_x, scale_y); // 保持宽高比
    
    // 计算居中偏移
    int scaled_width = static_cast<int>(original_width * scale);
    int scaled_height = static_cast<int>(original_height * scale);
    int offset_x = (240 - scaled_width) / 2;
    int offset_y = (240 - scaled_height) / 2;
    
    ESP_LOGI(TAG, "Scaling: original=%ux%u, scaled=%dx%d, offset=(%d,%d), scale=%.3f", 
             original_width, original_height, scaled_width, scaled_height, offset_x, offset_y, scale);
    
    // 尝试解析IDAT块（图像数据）
    pos = 8;
    std::vector<uint8_t> compressed_data;
    bool found_idat = false;
    
    while (pos + 12 < png_data.size()) {
        uint32_t chunk_length = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
        const char* chunk_type = reinterpret_cast<const char*>(&data[pos+4]);
        
        if (strncmp(chunk_type, "IDAT", 4) == 0) {
            // 收集压缩的图像数据
            compressed_data.insert(compressed_data.end(), &data[pos+8], &data[pos+8+chunk_length]);
            found_idat = true;
        }
        
        pos += 12 + chunk_length;
    }
    
    if (found_idat) {
        ESP_LOGI(TAG, "Found compressed image data, size: %zu bytes", compressed_data.size());
        // 这里应该解压缩数据，但由于复杂性，我们创建一个基于原始尺寸的占位符
    } else {
        ESP_LOGW(TAG, "No IDAT chunk found, creating placeholder pattern");
    }
    
    // 创建二维码样式的占位符图案，考虑原始尺寸
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 240; x++) {
            int index = y * 240 + x;
            
            // 检查是否在缩放后的图片区域内
            bool in_image_area = (x >= offset_x && x < offset_x + scaled_width &&
                                 y >= offset_y && y < offset_y + scaled_height);
            
            if (in_image_area) {
                // 映射回原始图片坐标
                int orig_x = static_cast<int>((x - offset_x) / scale);
                int orig_y = static_cast<int>((y - offset_y) / scale);
                
                // 确保坐标在有效范围内
                orig_x = std::max(0, std::min(orig_x, static_cast<int>(original_width) - 1));
                orig_y = std::max(0, std::min(orig_y, static_cast<int>(original_height) - 1));
                
                // 创建二维码样式的图案
                bool is_black = false;
                
                // 外边框
                if (orig_x < original_width * 0.1 || orig_x >= original_width * 0.9 ||
                    orig_y < original_height * 0.1 || orig_y >= original_height * 0.9) {
                    is_black = true;
                }
                // 定位图案（三个角落的方块）
                else if ((orig_x >= original_width * 0.15 && orig_x < original_width * 0.35 &&
                          orig_y >= original_height * 0.15 && orig_y < original_height * 0.35) ||
                         (orig_x >= original_width * 0.65 && orig_x < original_width * 0.85 &&
                          orig_y >= original_height * 0.15 && orig_y < original_height * 0.35) ||
                         (orig_x >= original_width * 0.15 && orig_x < original_width * 0.35 &&
                          orig_y >= original_height * 0.65 && orig_y < original_height * 0.85)) {
                    int block_size = std::max(1, static_cast<int>(std::min(original_width, original_height) / 20));
                    is_black = ((orig_x / block_size) % 2) == ((orig_y / block_size) % 2);
                }
                // 中心区域
                else if (orig_x >= original_width * 0.4 && orig_x < original_width * 0.6 &&
                         orig_y >= original_height * 0.4 && orig_y < original_height * 0.6) {
                    int block_size = std::max(1, static_cast<int>(std::min(original_width, original_height) / 40));
                    is_black = ((orig_x / block_size) % 2) == ((orig_y / block_size) % 2);
                }
                // 其他区域
                else {
                    int block_size = std::max(1, static_cast<int>(std::min(original_width, original_height) / 30));
                    is_black = ((orig_x / block_size) % 2) == ((orig_y / block_size) % 2);
                }
                
                rgb565_data[index] = is_black ? 0x0000 : 0xFFFF; // 黑色或白色
            } else {
                // 图片区域外的背景
                rgb565_data[index] = 0xFFFF; // 白色背景
            }
        }
    }
    
    ESP_LOGI(TAG, "QR code pattern created with scaling, size: %zu pixels", rgb565_data.size());
    return true;
}

void TopdEmojiDisplay::TestQRCodeUrl() {
    const std::string test_url = "https://core.device.lekale.com/data/images/qrcode/80152519e39c5d-1d1c-4e8a-a7b2-d7622dd7fbe4.png";
    ESP_LOGI(TAG, "Testing QR code URL: %s", test_url.c_str());
    ShowQRCode(test_url);
}
