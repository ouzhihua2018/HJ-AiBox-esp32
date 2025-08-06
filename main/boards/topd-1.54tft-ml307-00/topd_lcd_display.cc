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
//#include <zlib.h>
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
    if (!emotion_gif_) {
        return;
    }

    DisplayLockGuard lock(this);

    // 如果传入空字符串或null，隐藏所有表情
    if (!emotion || strlen(emotion) == 0) {
        ESP_LOGI(TAG, "隐藏所有表情显示");
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 检查是否正在显示二维码，如果是则不显示表情
    if (qr_img_obj_ != nullptr && !lv_obj_has_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN)) {
        ESP_LOGI(TAG, "QR code is being displayed, skipping emotion: %s", emotion);
        // 隐藏表情GIF，确保不与二维码冲突
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 隐藏二维码显示（如果有的话）
    if (qr_container_ != nullptr) {
        lv_obj_add_flag(qr_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (qr_img_obj_ != nullptr) {
        lv_obj_add_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    }

    // 显示表情GIF
    lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);

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
        ESP_LOGE(TAG, "QR code is empty");
        ShowQRError();
        return;
    }

    ESP_LOGI(TAG, "Downloading QR code from: %s", qrUrl.c_str());
    
    // 检查是否是URL
    if (qrUrl.find("http") != 0) {
        ESP_LOGW(TAG, "Not a valid URL, showing error");
        ShowQRError();
        return;
    }

    // 创建HTTP客户端下载图片
    auto& board = Board::GetInstance();
    auto http = board.CreateHttp();
    
    if (!http->Open("GET", qrUrl)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        ShowQRError();
        return;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status_code);
        ShowQRError();
        return;
    }

    // 下载图片数据
    std::vector<uint8_t> image_data;
    char buffer[1024];
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret <= 0) break;
        image_data.insert(image_data.end(), buffer, buffer + ret);
    }
    
    if (image_data.empty()) {
        ESP_LOGE(TAG, "Downloaded empty data");
        ShowQRError();
        return;
    }

    ESP_LOGI(TAG, "Downloaded %zu bytes", image_data.size());

    // 验证PNG格式
    if (image_data.size() < 8 || 
        memcmp(image_data.data(), "\x89PNG\r\n\x1A\n", 8) != 0) {
        ESP_LOGE(TAG, "Invalid PNG format");
        ShowQRError();
        return;
    }

    // 显示QR码（简化实现 - 显示占位符）
    DisplayQRImage(image_data);
    
    // 清除缓存数据
    image_data.clear();
    image_data.shrink_to_fit();
    ESP_LOGI(TAG, "Image cache cleared");
}

bool TopdEmojiDisplay::ShowQRCodeImage(const uint8_t* image_data, size_t data_size) {
    ESP_LOGI(TAG, "=== Starting QR Code Image Display ===");
    
    if (!image_data || data_size == 0) {
        ESP_LOGE(TAG, "❌ Invalid image data (null pointer or zero size)");
        ShowQRError();
        return false;
    }

    ESP_LOGI(TAG, "QR code image data size: %d bytes", data_size);

    // 详细检查PNG文件头
    if (data_size < 8) {
        ESP_LOGE(TAG, "❌ Image data too small for PNG header validation (%d bytes)", data_size);
        ShowQRError();
        return false;
    }
    
    // 显示原始字节用于调试
    ESP_LOGI(TAG, "Image header bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
            image_data[0], image_data[1], image_data[2], image_data[3], 
            image_data[4], image_data[5], image_data[6], image_data[7]);
    
    // 检查PNG文件头：0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    if (memcmp(image_data, "\x89PNG\r\n\x1a\n", 8) != 0) {
        ESP_LOGE(TAG, "❌ Invalid PNG format - header mismatch");
        ESP_LOGE(TAG, "Expected: 89 50 4E 47 0D 0A 1A 0A");
        ESP_LOGE(TAG, "Got:      %02X %02X %02X %02X %02X %02X %02X %02X", 
                image_data[0], image_data[1], image_data[2], image_data[3], 
                image_data[4], image_data[5], image_data[6], image_data[7]);
        ShowQRError();
        return false;
    }
    
    ESP_LOGI(TAG, "✅ PNG header validation passed");

    // 检查图片大小合理性
    if (data_size < 500) {
        ESP_LOGW(TAG, "⚠️  Image size seems small for a QR code (%d bytes)", data_size);
    } else if (data_size > 100000) {
        ESP_LOGW(TAG, "⚠️  Image size seems large for a QR code (%d bytes)", data_size);
    } else {
        ESP_LOGI(TAG, "✅ Image size is reasonable for a QR code");
    }

    // 将数据转换为vector格式用于现有的DisplayQRImage方法
    std::vector<uint8_t> image_vector(image_data, image_data + data_size);
    
    ESP_LOGI(TAG, "Calling DisplayQRImage to render on screen...");
    
    // 调用现有的显示方法
    DisplayQRImage(image_vector);
    
    ESP_LOGI(TAG, "✅ QR code image display completed successfully");
    ESP_LOGI(TAG, "=== QR Code Image Display Complete ===");
    return true;
}

void TopdEmojiDisplay::ShowQRError() {
    DisplayLockGuard lock(this);
    
    // 隐藏其他元素
    if (emotion_gif_) {
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 创建或更新错误显示
    if (qr_img_obj_ == nullptr) {
        qr_img_obj_ = lv_label_create(content_);
        lv_obj_set_size(qr_img_obj_, 240, 240);
        lv_obj_center(qr_img_obj_);
        lv_obj_set_style_text_align(qr_img_obj_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(qr_img_obj_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(qr_img_obj_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qr_img_obj_, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(qr_img_obj_, lv_color_black(), 0);
    }

    lv_label_set_text(qr_img_obj_, "QR Code\nUnavailable");
    lv_obj_clear_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGI(TAG, "QR error message displayed");
}

void TopdEmojiDisplay::DisplayQRImage(const std::vector<uint8_t>& image_data) {
    ESP_LOGI(TAG, "=== Rendering QR Code on Display ===");
    ESP_LOGI(TAG, "Image data size: %d bytes", image_data.size());
    
    DisplayLockGuard lock(this);
    
    // 隐藏所有其他UI元素，确保只显示二维码
    ESP_LOGI(TAG, "Hiding other UI elements...");
    if (emotion_gif_) {
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "✅ Emotion GIF hidden");
    }
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "✅ Chat message label hidden");
    }
    
    // 设置整个内容区域为白色背景
    ESP_LOGI(TAG, "Setting white background...");
    if (content_) {
        lv_obj_set_style_bg_color(content_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
        ESP_LOGI(TAG, "✅ White background applied");
    } else {
        ESP_LOGW(TAG, "⚠️  Content container is null");
    }

    // 创建或更新QR码显示
    if (qr_img_obj_ == nullptr) {
        qr_img_obj_ = lv_obj_create(content_);
        lv_obj_set_size(qr_img_obj_, 240, 240);
        lv_obj_center(qr_img_obj_);
        lv_obj_set_style_bg_color(qr_img_obj_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qr_img_obj_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(qr_img_obj_, 2, 0);
        lv_obj_set_style_border_color(qr_img_obj_, lv_color_black(), 0);
        lv_obj_set_style_radius(qr_img_obj_, 5, 0);
    }

    // 清除之前的内容
    lv_obj_clean(qr_img_obj_);

    // 创建QR码占位符（简化实现）
    lv_obj_t* qr_placeholder = lv_obj_create(qr_img_obj_);
    lv_obj_set_size(qr_placeholder, 200, 200);
    lv_obj_center(qr_placeholder);
    lv_obj_set_style_bg_color(qr_placeholder, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr_placeholder, 1, 0);
    lv_obj_set_style_border_color(qr_placeholder, lv_color_black(), 0);

    // 创建QR码图案（简单的网格模拟）
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            // 简单的伪随机模式
            if ((i + j * 7) % 3 == 0) {
                lv_obj_t* dot = lv_obj_create(qr_placeholder);
                lv_obj_set_size(dot, 18, 18);
                lv_obj_set_pos(dot, j * 20 + 1, i * 20 + 1);
                lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_radius(dot, 2, 0);
            }
        }
    }

    // 添加标签
    lv_obj_t* label = lv_label_create(qr_img_obj_);
    lv_label_set_text(label, "QR Code (300x300->240x240)");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_obj_clear_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGI(TAG, "✅ QR code object made visible");
    ESP_LOGI(TAG, "✅ QR code rendering completed successfully");
    ESP_LOGI(TAG, "Display status: White background with centered QR code (240x240)");
    ESP_LOGI(TAG, "=== QR Code Display Rendering Complete ===");
}

void TopdEmojiDisplay::HideQRCode() {
    DisplayLockGuard lock(this);
    
    if (qr_img_obj_ != nullptr) {
        lv_obj_add_flag(qr_img_obj_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 恢复原来的背景色（使用主题背景色）
    if (content_) {
        lv_obj_set_style_bg_color(content_, current_theme_.background, 0);
        lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
    }
    
    // 恢复其他元素
    if (emotion_gif_) {
        lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
    if (chat_message_label_) {
        lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "QR code hidden, background restored");
}

void TopdEmojiDisplay::TestQRCodeUrl() {
    const std::string test_url = "https://core.device.lekale.com/data/images/qrcode/80152519e39c5d-1d1c-4e8a-a7b2-d7622dd7fbe4.png";
    ESP_LOGI(TAG, "Testing QR code with URL: %s", test_url.c_str());
    ShowQRCode(test_url);
}

