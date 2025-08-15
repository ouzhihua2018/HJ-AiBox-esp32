# OTA HTTPS 下载修复说明

## 问题描述
在使用4G网络（ML307模块）下载HTTPS图片时，出现SSL握手失败的问题。

## 修复内容

### 1. 增强的SSL配置
- 在 `SetupHttp()` 函数中添加了SSL相关配置
- 设置了 `Connection: close` 和 `Accept-Encoding: identity` 头部
- 添加了SSL配置日志输出

### 2. 专门的HTTPS下载函数
- 新增 `Download_Qrcode_Https()` 函数，专门处理HTTPS下载
- 实现了重试机制，最多重试3次
- 添加了详细的错误日志和状态检查

### 3. 自动URL检测
- 修改 `Download_Qrcode()` 函数，自动检测HTTPS URL
- 对于HTTPS URL，自动使用专门的HTTPS下载函数
- 对于HTTP URL，使用原有的下载逻辑

### 4. SSL配置函数
- 新增 `ConfigureSslForHttps()` 函数，统一管理SSL配置
- 设置SSL相关请求头
- 提供SSL配置的日志输出

### 5. 测试功能
- 新增 `TestHttpsDownload()` 函数，用于测试HTTPS连接
- 创建了 `ota_test.cc` 测试文件
- 提供了完整的测试流程

## 使用方法

### 基本使用
```cpp
Ota ota;
bool success = ota.Download_Qrcode();
```

### 测试HTTPS连接
```cpp
Ota ota;
bool success = ota.TestHttpsDownload("https://example.com/test");
```

### 运行完整测试
```cpp
RunOtaHttpsTest();
```

## 修复的关键点

1. **SSL握手重试**：实现了3次重试机制，每次间隔2秒
2. **错误处理**：添加了详细的错误日志和响应内容输出
3. **连接管理**：正确设置连接头部，避免连接复用问题
4. **数据验证**：验证下载数据的完整性和PNG头部
5. **超时处理**：在重试之间添加适当的延迟

## 日志输出
修复后的代码会输出详细的日志信息：
- URL类型检测（HTTP/HTTPS）
- SSL配置状态
- 连接尝试次数
- 响应状态码
- 下载数据大小
- 错误详情

## 注意事项

1. 确保ML307模块的SSL/TLS配置正确
2. 检查网络连接稳定性
3. 验证服务器证书的有效性
4. 监控内存使用情况，避免大文件下载时的内存问题

## 测试建议

1. 首先测试基本的HTTPS连接
2. 然后测试小图片下载
3. 最后测试实际的二维码图片下载
4. 在不同网络环境下测试（WiFi vs 4G）

## 后续优化建议

1. 添加SSL证书验证配置
2. 实现断点续传功能
3. 添加下载进度回调
4. 优化内存使用，支持流式下载
