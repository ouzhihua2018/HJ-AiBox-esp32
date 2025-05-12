#include "human_sensor.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define TAG "HUMAN_SENSOR"
static const gpio_num_t SENSOR_GPIO = GPIO_NUM_42;
static bool s_current_state = false;
static void (*s_callback)(bool) = NULL;
static SemaphoreHandle_t s_callback_mutex = NULL;
static bool s_task_running = true;

static void detection_task(void *arg) {
    while (s_task_running) {
        bool new_state = gpio_get_level(SENSOR_GPIO);
        if (new_state != s_current_state) {
            s_current_state = new_state;
            ESP_LOGI(TAG, "状态变化: %d", s_current_state);
            if (s_callback) {
                xSemaphoreTake(s_callback_mutex, portMAX_DELAY);
                s_callback(s_current_state);
                xSemaphoreGive(s_callback_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

esp_err_t human_sensor_init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    s_callback_mutex = xSemaphoreCreateMutex();
    xTaskCreate(detection_task, "human_det", 4096, NULL, 3, NULL);
    return ESP_OK;
}

bool human_sensor_get_state(void) {
    return s_current_state;
}

void human_sensor_set_callback(void (*callback)(bool state)) {
    if (s_callback_mutex == NULL) {
        s_callback_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_callback_mutex, portMAX_DELAY);
    s_callback = callback;
    xSemaphoreGive(s_callback_mutex);
}