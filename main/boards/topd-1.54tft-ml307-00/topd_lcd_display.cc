#include "topd_lcd_display.h"

#include <esp_log.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <libs/gif/lv_gif.h>
#include "board.h"
#include "display/lcd_display.h"
#include "font_awesome_symbols.h"

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
    ESP_LOGI(TAG,"TopdDisplay construct");
   
    
    SwitchToGifContainer(); 
    ESP_LOGI(TAG,"SetupGifContainer();");
    
   
};

void TopdEmojiDisplay::SwitchToGifContainer() {
    ESP_LOGI(TAG,"Switch To Gif Container");
    DisplayLockGuard lock(this);
    SetupHighTempWarningPopup();
    if (emotion_label_) {
        lv_obj_del(emotion_label_);
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
    }
    if (content_) {
        lv_obj_del(content_);
    }
    if (qr_image_object_) {
        lv_obj_del(qr_image_object_);
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

void TopdEmojiDisplay::SwitchToActivationStatusContainer()
{   
    ESP_LOGI(TAG,"Switch To Activation Status Container");
    DisplayLockGuard lock(this);
    
    if (emotion_label_) {
        lv_obj_del(emotion_label_);
        emotion_label_=nullptr;
    }

    if (emotion_gif_) {
        lv_obj_del(emotion_gif_);
        emotion_gif_=nullptr;
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
        chat_message_label_=nullptr;
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
 
    qr_image_object_ = lv_image_create(content_);
    lv_obj_set_size(qr_image_object_, width_ * 0.9, height_ * 0.9);
    ESP_LOGI(TAG,"qr_image_object_ size: %.2f, %.2f",width_ * 0.9,height_ * 0.9);
    lv_obj_align(qr_image_object_, LV_ALIGN_CENTER, 0, 0);    
    lv_obj_add_flag(qr_image_object_, LV_OBJ_FLAG_HIDDEN);  

    LcdDisplay::SetTheme("dark");
    ESP_LOGI(TAG,"SetupActivationStatusContainer END");
}

void TopdEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion || !emotion_gif_) {
        return;
    }

    DisplayLockGuard lock(this);

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

void TopdEmojiDisplay::SetWechatQrcodeImage(const lv_img_dsc_t *img_dsc)
{
    DisplayLockGuard lock(this);
    uint32_t img_width = 0, img_height = 0;
    if (qr_image_object_ == nullptr) {
        return;
    }
    const uint8_t * png_data = img_dsc->data;
    size_t png_len = img_dsc->data_size;
    const uint8_t* png_footer = png_data + (png_len - 12);
    ESP_LOGI(TAG, "再次包头检查: %02X %02X %02X %02X %02X %02X %02X %02X", 
        png_data[0], png_data[1], png_data[2], png_data[3], 
        png_data[4], png_data[5], png_data[6], png_data[7]);
    ESP_LOGI(TAG, "PNG 包尾检查: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            png_footer[0],  png_footer[1],  png_footer[2],  png_footer[3],
            png_footer[4],  png_footer[5],  png_footer[6],  png_footer[7],
            png_footer[8],  png_footer[9],  png_footer[10], png_footer[11]);

    if (img_dsc != nullptr && img_dsc->header.w > 0 && img_dsc->header.h > 0) {
    //打印图片尺寸信息
    lv_image_header_t img_header;
    if (lv_image_decoder_get_info(img_dsc, &img_header) != LV_RES_OK)
    {   
        ESP_LOGE(TAG,"[%s:%d] lv_img_decoder_get_info errror", __FUNCTION__, __LINE__);
        return;
    }
    img_width = img_header.w;
    img_height = img_header.h;
    printf("[%s:%d] img_width:%ld, img_height:%ld\n", __FUNCTION__, __LINE__, img_width, img_height);
    // show
    lv_image_set_src(qr_image_object_,img_dsc);
    lv_obj_clear_flag(qr_image_object_, LV_OBJ_FLAG_HIDDEN);

    if (emotion_label_ != nullptr) {
        lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
    }
      // 清除所有状态文字和聊天信息，确保屏幕只显示二维码
    SetStatus("");
    SetChatMessage("system", "");
    }
}
