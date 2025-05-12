#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 回调函数类型
typedef void (*human_sensor_callback_t)(bool state);

/**
 * @brief 初始化人体传感器
 * 
 * 初始化 GPIO 和检测任务，开始检测人体状态。
 * 
 * @return 
 *      - ESP_OK: 初始化成功
 *      - ESP_ERR_NO_MEM: 内存不足
 *      - ESP_ERR_INVALID_ARG: 参数无效
 *      - ESP_FAIL: 初始化失败
 */
esp_err_t human_sensor_init(void);

/**
 * @brief 停止人体传感器
 * 
 * 停止检测任务并释放资源。
 * 
 * @return 
 *      - ESP_OK: 停止成功
 *      - ESP_FAIL: 停止失败
 */
esp_err_t human_sensor_deinit(void);

/**
 * @brief 获取当前人体传感器状态
 * 
 * @return 
 *      - true: 检测到人体
 *      - false: 未检测到人体
 */
bool human_sensor_get_state(void);

/**
 * @brief 设置状态变化的回调函数
 * 
 * 当人体状态发生变化时，会调用该回调函数。
 * 
 * @param callback 回调函数，参数为当前状态（true 表示有人体，false 表示无人）。
 */
void human_sensor_set_callback(human_sensor_callback_t callback);

#ifdef __cplusplus
}
#endif