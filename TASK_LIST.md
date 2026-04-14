# 项目任务列表

本文档详细列出了项目中所有的任务，包括显式创建的任务、背景任务和应用任务。

## 一、显式创建的FreeRTOS任务（xTaskCreate/xTaskCreatePinnedToCore/xTaskCreateStatic）

### 1. **background_task** (背景任务主循环)
- **位置**: `main/background_task.cc:9`
- **任务名**: `"background_task"`
- **栈大小**: 4096 * 7 = 28672 字节
- **优先级**: 2
- **核心**: 未指定（可在任意核心运行）
- **功能**: 执行通过`Schedule()`方法添加的异步任务
- **循环**: `BackgroundTaskLoop()` - 等待任务队列，执行任务

### 2. **audio_loop** (音频处理循环任务)
- **位置**: `main/application.cc:424` (CONFIG_USE_AUDIO_PROCESSOR) 或 `main/application.cc:430` (其他)
- **任务名**: `"audio_loop"`
- **栈大小**: 4096 * 2 = 8192 字节
- **优先级**: 8
- **核心**: 
  - 如果启用CONFIG_USE_AUDIO_PROCESSOR: Core 1 (固定)
  - 否则: 未指定
- **功能**: 循环处理音频输入输出数据
- **循环**: `AudioLoop()` - 调用`OnAudioInput()`和`OnAudioOutput()`

### 3. **audio_communication** (AFE音频通信任务)
- **位置**: `main/audio_processing/afe_audio_processor.cc:52`
- **任务名**: `"audio_communication"`
- **栈大小**: 4096 字节
- **优先级**: 3
- **核心**: 未指定
- **功能**: 处理AFE音频处理器的数据获取和输出
- **循环**: `AudioProcessorTask()` - 等待PROCESSOR_RUNNING事件，获取AFE处理结果

### 4. **audio_detection** (音频唤醒词检测任务)
- **位置**: `main/audio_processing/afe_wake_word.cc:69`
- **任务名**: `"audio_detection"`
- **栈大小**: 4096 字节
- **优先级**: 3
- **核心**: 未指定
- **功能**: 处理AFE唤醒词检测
- **循环**: `AudioDetectionTask()` - 等待DETECTION_RUNNING_EVENT事件，获取检测结果并feed到micro_wake_word

### 5. **encode_detect_packets** (唤醒词编码任务)
- **位置**: `main/audio_processing/afe_wake_word.cc:158`
- **任务名**: `"encode_detect_packets"`
- **栈大小**: 4096 * 8 = 32768 字节（静态分配在PSRAM）
- **优先级**: 2
- **核心**: 未指定
- **功能**: 将唤醒词PCM数据编码为Opus格式
- **特点**: 使用`xTaskCreateStatic`静态分配，一次性任务（执行完即删除）

### 6. **core0_monitor** (核心0监控任务)
- **位置**: `main/core_monitor.cc:32`
- **任务名**: `"core0_monitor"`
- **栈大小**: 4096 字节
- **优先级**: 1（最低优先级）
- **核心**: 未指定（注释说明绑定到Core 1）
- **功能**: 监控和打印所有任务状态
- **循环**: `core0_monitor_task()` - 每10秒打印一次任务列表
- **状态**: 默认被注释，需要调用`start_core1_monitor()`启用

### 7. **DnsServerTask** (DNS服务器任务)
- **位置**: `managed_components/78__esp-wifi-connect/dns_server.cc:38`
- **任务名**: `"DnsServerTask"`
- **栈大小**: 4096 字节
- **优先级**: 5
- **核心**: 未指定
- **功能**: 处理DNS服务器请求（用于WiFi配网）
- **循环**: `Run()` - 接收DNS请求并响应

### 8. **restart_task** (重启任务 - WiFi配网)
- **位置**: `managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc:754`
- **任务名**: `"restart_task"`
- **栈大小**: 4096 字节
- **优先级**: 5
- **核心**: 未指定
- **功能**: SmartConfig完成后延迟3秒重启设备
- **特点**: 一次性任务（执行完即删除）

### 9. **延迟重启任务** (WiFi配网)
- **位置**: `managed_components/78__esp-wifi-connect/wifi_configuration_ap.cc:422`
- **任务名**: 未命名（lambda函数）
- **栈大小**: 未指定（使用默认）
- **优先级**: 未指定（使用默认）
- **核心**: 未指定
- **功能**: 延迟200ms后停止Web服务器并重启
- **特点**: 一次性任务（执行完即删除）

## 二、背景任务（BackgroundTask）中执行的任务

背景任务通过`background_task_->Schedule()`方法添加，在`background_task`任务中顺序执行。

### 1. **Opus音频编码任务**
- **位置**: `main/application.cc:651`
- **触发**: `audio_processor_->OnOutput()`回调
- **功能**: 将AFE处理后的音频数据编码为Opus格式
- **执行**: `opus_encoder_->Encode()` - 编码后将数据包加入`audio_send_queue_`

### 2. **其他通过Schedule添加的异步任务**
- 所有通过`background_task_->Schedule()`添加的任务都在背景任务中执行
- 任务队列最大容量：30个任务
- 超过30个任务时会记录警告日志

## 三、应用任务（MainEventLoop）中执行的任务

应用任务通过`Application::Schedule()`方法添加，在`MainEventLoop()`中顺序执行。

### MainEventLoop任务
- **位置**: `main/application.cc:791`
- **优先级**: 3（动态提升）
- **功能**: 主事件循环，处理调度事件和音频发送事件
- **循环**: `MainEventLoop()` - 等待`SCHEDULE_EVENT`和`SEND_AUDIO_EVENT`事件

### 通过Application::Schedule()添加的任务（部分列表）

1. **显示更新任务** (`main/application.cc:326, 336, 340, 358, 369`)
   - 更新显示状态、图标、消息等

2. **协议消息处理任务** (`main/application.cc:495, 508, 515, 529, 538, 545`)
   - 处理WebSocket/MQTT消息
   - 显示聊天消息
   - 更新情绪显示

3. **唤醒词处理任务** (`main/application.cc:699, 1212`)
   - 处理唤醒词检测结果
   - 启动/停止检测

4. **音频输出控制任务** (`main/application.cc:684`)
   - 处理VAD状态变化
   - 控制音频输出

5. **时钟更新任务** (`main/application.cc:765`)
   - 每10秒更新一次时钟显示

6. **MQTT协议任务** (`main/protocols/mqtt_protocol.cc:81`)
   - 处理MQTT消息

7. **ML307板卡任务** (`main/boards/common/ml307_board.cc:38`)
   - ML307网络板卡相关任务

8. **IoT设备任务** (`main/iot/thing.cc:82`)
   - IoT设备方法调用

9. **其他调度任务** (`main/application.cc:387, 574, 600, 1218, 1222, 1259, 1268`)
   - 各种应用状态更新和处理

## 四、ESP定时器任务（ESP_TIMER_TASK）

ESP定时器使用`ESP_TIMER_TASK`分发方式，会在ESP-IDF的定时器任务中执行回调。

### 1. **clock_timer** (时钟定时器)
- **位置**: `main/application.cc:77`
- **名称**: `"clock_timer"`
- **周期**: 1秒（1000000微秒）
- **功能**: 更新状态栏，每10秒打印系统信息
- **回调**: `Application::OnClockTimer()`

### 2. **microwakeword** (微唤醒词定时器)
- **位置**: `main/application.cc:89`
- **名称**: `"microwakeword"`
- **触发**: 启动后5秒触发一次
- **功能**: 启动微唤醒词检测和AFE唤醒词检测
- **回调**: `micro_wake_word_->StartDetection()` 和 `wake_word_->StartDetection()`

### 3. **strip_timer** (LED灯带定时器)
- **位置**: `main/led/circular_strip.cc:32`
- **名称**: `"strip_timer"`
- **功能**: LED灯带动画回调
- **回调**: `CircularStrip::strip_callback_()`
- **特点**: 每个CircularStrip实例都有自己的定时器

### 4. **angle_timer** (角度检测定时器 - TDi-300-PH)
- **位置**: `main/boards/TDi-300-PH/TDi-300-PH.cc:204`
- **名称**: `"angle_timer"`
- **周期**: 100ms（100000微秒）
- **功能**: 读取电位器ADC值
- **回调**: 读取GPIO4的ADC值，可选自动控制电机转速

### 5. **backlight_timer** (背光定时器)
- **位置**: `main/boards/common/backlight.cc:12`
- **名称**: `"backlight_timer"`
- **功能**: 背光亮度渐变定时器
- **回调**: `Backlight::OnTransitionTimer()`
- **特点**: 每5ms更新一次亮度

### 6. **notification_timer** (通知定时器)
- **位置**: `main/display/display.cc:19` 和 `main/display/lvgl_display/lvgl_display.cc:20`
- **名称**: `"notification_timer"`
- **功能**: 隐藏通知标签，显示状态标签
- **回调**: 更新显示状态

### 7. **power_save_timer** (省电定时器)
- **位置**: `main/boards/common/power_save_timer.cc`
- **功能**: 管理设备省电模式
- **回调**: 进入/退出睡眠模式

## 五、任务优先级总结

| 优先级 | 任务名 | 说明 |
|--------|--------|------|
| 1 | core0_monitor | 最低优先级，监控任务 |
| 2 | background_task | 背景任务主循环 |
| 2 | encode_detect_packets | 唤醒词编码（一次性） |
| 3 | audio_communication | AFE音频通信 |
| 3 | audio_detection | 音频唤醒词检测 |
| 3 | MainEventLoop | 应用主事件循环（动态提升） |
| 5 | DnsServerTask | DNS服务器 |
| 5 | restart_task | 重启任务（一次性） |
| 8 | audio_loop | 音频处理循环（最高优先级） |

## 六、任务核心分配

- **Core 1固定**: `audio_loop` (如果启用CONFIG_USE_AUDIO_PROCESSOR)
- **其他任务**: 未指定核心，由FreeRTOS调度器分配

## 七、任务执行流程

1. **启动阶段**:
   - `app_main()` → `Application::Start()`
   - 创建`background_task`
   - 创建`audio_loop`任务
   - 启动各种定时器

2. **运行阶段**:
   - `audio_loop`持续处理音频I/O
   - `background_task`执行异步任务
   - `MainEventLoop`处理应用事件
   - 定时器任务定期执行回调

3. **任务通信**:
   - 通过`Schedule()`方法添加任务到队列
   - 通过事件组(`EventGroup`)同步
   - 通过互斥锁保护共享资源

## 八、注意事项

1. **背景任务队列限制**: 最多30个待执行任务
2. **音频任务优先级最高**: `audio_loop`优先级为8，确保实时性
3. **定时器任务**: 使用ESP定时器，在ESP-IDF定时器任务中执行
4. **一次性任务**: 部分任务执行完即删除（如编码任务、重启任务）
5. **任务栈大小**: 根据功能需求分配，最大的是`encode_detect_packets`（32KB）








