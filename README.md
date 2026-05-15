**编译流程**:位于项目CMAKELISTS目录中，Set-target esp32s3，随后idf.py menuconfig选择要变更内容(底座默认不需要任何更改)，随后build编译。由于添加了唤醒词components组件，导致原有的管理组件依赖混乱，每次重新编译需手动在管理组件opus-encoder中添加REQUIRES 78__esp-opus。    78 ml307_mqtt 手动添加connected_ = true;  
以及通过减小心跳包频率和不清除会话来防止MQTT断开，ML307_MQTT修改为
    // Set clean session
    if (!modem_.Command(std::string("AT+MQTTCFG=\"clean\",") + std::to_string(mqtt_id_) + ",0")) {
        ESP_LOGE(TAG, "Failed to set MQTT clean session");
        return false;
    }
/
    // Set keep alive
    if (!modem_.Command(std::string("AT+MQTTCFG=\"pingreq\",") + std::to_string(mqtt_id_) + ",120")) {
        ESP_LOGE(TAG, "Failed to set MQTT keep alive");
        return false;
    }
if (type == "conn") {
                    if (arguments[2].int_value == 0) {
                        connected_ = true;
                        xEventGroupSetBits(event_group_handle_, MQTT_CONNECTED_EVENT);
                    } 
若4G无法连接鼎乐MQTT，尝试将MQtt.h 中 int keep_alive_seconds_ = 30;改大

**程序框架**:应用层(application.cc)->板级层(board.cc)->底层驱动层(各种组件)。

**逻辑流程**:(括号内表示当前状态)上电后，(开始)首先对板类加载，随后底层Codec初始化,opus编解码器初始化，创建音频采集播放任务audio_loop,开始对底层CODEC的采集和播放。启动系统状态定时器 每秒更新系统状态以及3秒更新系统时间。随后进入网络加载，OTA升级，MCP通用工具添加，实例化通讯协议并注册二进制音频数据和json回调,初始化AFE以及micro唤醒词，至此初始化完毕，进入空闲状态，开启唤醒词监测。主线程则进入*MainEventLoop*来处理音频发送任务和调度器任务。

​		当唤醒词唤醒或按键唤醒后，进入连接中状态，开启音频通道，(监听中)，音频送入后台后，会收到后台的tts start json，设备切换状态到讲话中，讲话后会收到tts stop json，设备状态切换到监听中继续监听对话。

​		当用户说再见后，会从后台收到tts goodbye json，回到空闲状态。

**细节**:

1. 板类加载: 单例模式，在这里创建板子用了非常巧妙的静态加载，通过在idf.py menuconfig中选择不同的板类，在CMakelists中加载不同的板类文件。而不同的板类文件中声明宏函数，会传入具体板类参数名，来实例化具体的板类。对于TDi-300来说，会依次初始化父类构造(初始化WIFI或4G板)，

2. 状态变化时做的事情: (1)监听时会开启AFE，并通知后台开始设备开始监听，停止唤醒词监听。底层CODEC一直是使能的，在*OnAudioInput*()循环任务中，只有当AFE开启时，才会循环去底层读取音频数据并feed。同时 只有当AFE开启时，才会循环执行fetch，并把音频包编码后放入音频发送队列发送到后台。(2) 讲话时会停止AFE，开启唤醒词(打断使用)，并重置opus解码器清空解码队列，并使能输出，音频才会循环从解码队列退队并播放。
