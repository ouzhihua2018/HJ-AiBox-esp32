#ifndef TOPD_LCD_DISPLAY_H
#define TOPD_LCD_DISPLAY_H

#include <libs/gif/lv_gif.h>

#include "display/lcd_display.h"
//#include <esp_lvgl_port.h>
#include "otto_emoji_gif.h"

class HJEmojiDisplay : public SpiLcdDisplay {

protected:
    lv_obj_t* high_temp_popup_ = nullptr;  // 高温警告弹窗
    lv_obj_t* high_temp_label_ = nullptr;  // 高温警告标签

public:
    // 继承构造函数
    using SpiLcdDisplay::SpiLcdDisplay;
    
    void SetupHighTempWarningPopup() {
        // 创建高温警告弹窗
        high_temp_popup_ = lv_obj_create(lv_scr_act());  // 使用当前屏幕
        lv_obj_set_scrollbar_mode(high_temp_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(high_temp_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
        lv_obj_align(high_temp_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(high_temp_popup_, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_radius(high_temp_popup_, 10, 0);
        
        // 创建警告标签
        high_temp_label_ = lv_label_create(high_temp_popup_);
        lv_label_set_text(high_temp_label_, "警告：温度过高");
        lv_obj_set_style_text_color(high_temp_label_, lv_color_white(), 0);
        lv_obj_center(high_temp_label_);
        
        // 默认隐藏
        lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
    }

    void UpdateHighTempWarning(float chip_temp, float threshold = 75.0f) {
        if (high_temp_popup_ == nullptr) {
            ESP_LOGW("TopdEmojiDisplay ", "High temp popup not initialized!");
            return;
        }

        if (chip_temp >= threshold) {
            ShowHighTempWarning();
        } else {
            HideHighTempWarning();
        }
    }

    void ShowHighTempWarning() {
        if (high_temp_popup_ && lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void HideHighTempWarning() {
        if (high_temp_popup_ && !lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    } 
    void SetupGifContainer();
   
public:
     /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    HJEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy, DisplayFonts fonts);

    virtual ~HJEmojiDisplay() = default;

    // 重写表情设置方法
    virtual void SetEmotion(const char* emotion) override;

    // 重写聊天消息设置方法
    virtual void SetChatMessage(const char* role, const char* content) override;

    // 添加SetIcon方法声明
    virtual void SetIcon(const char* icon) override; 

   
    //otto 新增函数
private:
    
    lv_obj_t* emotion_gif_;  ///< GIF表情组件 >
    lv_obj_t* qr_image_object_ = nullptr;
    // 表情映射
    struct EmotionMap {
        const char* name;
        const lv_img_dsc_t* gif;
    };

    static const EmotionMap emotion_maps_[];
};

#endif // TOPD_LCD_DISPLAY_H