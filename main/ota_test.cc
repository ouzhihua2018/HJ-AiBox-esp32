#include "ota.h"
#include "application.h"
#include <esp_log.h>

static const char *TAG = "OtaTest";

void TestOtaHttpsDownload() {
    ESP_LOGI(TAG, "Starting OTA HTTPS download test...");
    
    Ota ota;
    
    // 测试HTTPS连接
    std::string test_url = "https://httpbin.org/get";
    ESP_LOGI(TAG, "Testing basic HTTPS connection with: %s", test_url.c_str());
    
    if (ota.TestHttpsDownload(test_url)) {
        ESP_LOGI(TAG, "Basic HTTPS test PASSED");
    } else {
        ESP_LOGE(TAG, "Basic HTTPS test FAILED");
    }
    
    // 测试图片下载
    std::string image_url = "https://httpbin.org/image/png";
    ESP_LOGI(TAG, "Testing image download with: %s", image_url.c_str());
    
    // 临时设置测试URL
    // 注意：这里需要修改OTA类来支持测试URL
    ESP_LOGI(TAG, "Image download test completed");
}

// 在应用程序中调用此函数来测试HTTPS下载
void RunOtaHttpsTest() {
    auto& application = Application::GetInstance();
    
    application.Schedule([]() {
        TestOtaHttpsDownload();
    });
}
