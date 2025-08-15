#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "sample.h"
#include "settings.h"
#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#else
#include "no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "esp_wake_word.h"
#else
#include "no_wake_word.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 7);

#if CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#else
    wake_word_ = std::make_unique<NoWakeWord>();
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota_.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota_.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota_.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota_.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
            wake_word_->StopDetection();
            // 预先关闭音频输出，避免升级过程有音频操作
            auto codec = board.GetAudioCodec();
            codec->EnableInput(false);
            codec->EnableOutput(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_decode_queue_.clear();
            }
            background_task_->WaitForCompletion();
            delete background_task_;
            background_task_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));

            ota_.StartUpgrade([display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            });

            // If upgrade success, the device will reboot and never reach here
            display->SetStatus(Lang::Strings::UPGRADE_FAILED);
            ESP_LOGI(TAG, "Firmware upgrade failed...");
            vTaskDelay(pdMS_TO_TICKS(3000));
<<<<<<< HEAD
<<<<<<< HEAD

            // 升级失败后，检查设备关联状态决定后续行为
            HandleDeviceActivationAndQRCode();

            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
=======
            Reboot();
>>>>>>> 0f4a466141c1cd8ff8001902e870902c724f7307
=======
            Reboot();
>>>>>>> topd-debug
            return;
        }

        // 无新版本：标记当前版本有效
        ota_.MarkCurrentVersionValid();
        
        // 是否已激活判断
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) {
            //已激活，保持当前界面继续执行
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            // Exit the loop if done checking new version
            break;
        }
        // 未激活，开始激活，界面切换
        display->SwitchToActivationStatusContainer();
      
        ESP_LOGW(TAG,"ota_.HasWeChatQrCodeUrl():%d",ota_.HasWeChatQrCodeUrl());
        // QrCode is shown to the user and waiting for the user to input
        if (ota_.HasWeChatQrCodeUrl()) {
            ShowWechatQrCode();
        }
       
        /////////////////////////////////////////////////////
        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
                ESP_LOGI(TAG, "Activation successful!");
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode() {
    auto& message = ota_.GetActivationMessage();
    auto& code = ota_.GetActivationCode();

    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound);
        }
    }
}

void Application::ShowWechatQrCode() {
    auto& message = ota_.GetActivationMessage();
    //Download Qrcode
    if(!ota_.Download_Qrcode()){
        ESP_LOGE(TAG,"wechat_qrcode_download_fail");
        return ;
    }
    ESP_LOGI(TAG,"=============================================");
    ESP_LOGI(TAG,"The QR code was successfully downloaded.");
    //show Qrcode
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    // 正确代码：获取引用后取指针           //careful 若返回非引用，临时string的c_str()会被释放
    const std::string& qr_data = ota_.GetWechatQrData(); // 引用指向有效内存
    const char* png_image = qr_data.c_str(); 
    qrcode_img.data = (uint8_t*)png_image;
    qrcode_img.data_size = qr_data.size(); // 同时传递正确的大小
    ESP_LOGI(TAG,"qrcode_img.data_size.%ld",qrcode_img.data_size);
    qrcode_img.header.cf = LV_COLOR_FORMAT_RAW;
    qrcode_img.header.w = 200;
    qrcode_img.header.h = 200;
    
    display->SetWechatQrcodeImage(&qrcode_img);
    ESP_LOGI(TAG,"=============================================");
    ESP_LOGI(TAG,"The QR code was show completed.");

    PlaySound(Lang::Sounds::P3_ACTIVATION_QRCODE);
    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    //Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion();

    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        AudioStreamPacket packet;
        packet.sample_rate = 16000;
        packet.frame_duration = 60;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    ESP_LOGI(TAG,"GetDisplay");
    auto display = board.GetDisplay();
    
    /* Setup the audio codec */
    ESP_LOGI(TAG,"audio start");
    auto codec = board.GetAudioCodec();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    if (aec_mode_ != kAecOff) {
        ESP_LOGI(TAG, "AEC mode: %d, setting opus encoder complexity to 0", aec_mode_);
        opus_encoder_->SetComplexity(0);
    } else if (board.GetBoardType() == "ml307") {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
    } else {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
    }

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, 1);
#else
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_);
#endif

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);
<<<<<<< HEAD

<<<<<<< HEAD
    // 按照用户要求的五步开机流程
    ESP_LOGI(TAG, "=== 开机流程开始 ===");

    // 第一步：确定网络状态
    ESP_LOGI(TAG, "第一步：确定网络状态");
    if (!InitializeNetworkConnection())
    {
        ESP_LOGE(TAG, "网络初始化失败，无法继续");
        display->SetChatMessage("system", "网络连接失败");
        return;
    }
=======
    ESP_LOGI(TAG,"startnetwork");
    /* Wait for the network to be ready */
    board.StartNetwork();
>>>>>>> topd-debug

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

<<<<<<< HEAD
    // 第二步：检查设备注册状态
    ESP_LOGI(TAG, "第二步：检查设备注册状态");
    if (!CheckDeviceRegistrationStatus())
    {
        ESP_LOGE(TAG, "无法检查设备注册状态");
        display->SetChatMessage("system", "设备状态检查失败");
        return;
    }

    // 如果设备已注册，直接跳转到固件版本确认
    if (IsDeviceRegistered())
    {
        ESP_LOGI(TAG, "设备已注册，跳转到固件版本确认");
        CheckNewVersion();
    }
    else
    {
        ESP_LOGI(TAG, "设备未注册，开始QR码注册流程");

        // 第三步：获取QR码下载URL
        ESP_LOGI(TAG, "第三步：通过POST请求获取QR码URL");
        if (!GetQRCodeDownloadUrl())
        {
            ESP_LOGE(TAG, "获取QR码URL失败");
            display->SetChatMessage("system", "二维码获取失败");
            return;
        }

        // 第四步：下载QR码图片并转换格式
        ESP_LOGI(TAG, "第四步：下载QR码图片并转换格式");
        if (!DownloadAndProcessQRCode())
        {
            ESP_LOGE(TAG, "QR码图片下载或处理失败");
            display->SetChatMessage("system", "二维码图片处理失败");
            return;
        }

        // 第五步：在TFT屏显示QR码
        ESP_LOGI(TAG, "第五步：在TFT屏显示QR码");
        DisplayQRCodeOnTFT();

        // 等待设备注册完成
        ESP_LOGI(TAG, "等待设备注册完成...");
        WaitForDeviceRegistration();

        // 注册完成后检查固件版本
        ESP_LOGI(TAG, "设备注册完成，检查固件版本");
        //CheckNewVersion();
    }
=======
    /* Wait for the network to be ready (align with application_Test.cc) */
    board.StartNetwork();
    
    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
    
    // Check for new firmware version or get the MQTT/Websocket address
    CheckNewVersion();
>>>>>>> 0f4a466141c1cd8ff8001902e870902c724f7307
=======
    // Check for new firmware version or get the MQTT broker address
    CheckNewVersion();
>>>>>>> topd-debug

    ESP_LOGI(TAG, "leave checknewversion");
    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
#if CONFIG_IOT_PROTOCOL_MCP
    McpServer::GetInstance().AddCommonTools();
#endif

    if (ota_.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_state_ == kDeviceStateSpeaking && audio_decode_queue_.size() < MAX_AUDIO_PACKETS_IN_QUEUE) {
            audio_decode_queue_.emplace_back(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }

#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
        }
#endif
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    background_task_->WaitForCompletion();
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
#if CONFIG_IOT_PROTOCOL_MCP
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
#endif
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (cJSON_IsArray(commands)) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
#endif
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    audio_processor_->Initialize(codec);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
#ifdef CONFIG_USE_SERVER_AEC
                {
                    std::lock_guard<std::mutex> lock(timestamp_mutex_);
                    if (!timestamp_queue_.empty()) {
                        packet.timestamp = timestamp_queue_.front();
                        timestamp_queue_.pop_front();
                    } else {
                        packet.timestamp = 0;
                    }

                    if (timestamp_queue_.size() > 3) { // 限制队列长度3
                        timestamp_queue_.pop_front(); // 该包发送前先出队保持队列长度
                        return;
                    }
                }
#endif
                std::lock_guard<std::mutex> lock(mutex_);
                if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                    ESP_LOGW(TAG, "Too many audio packets in queue, drop the oldest packet");
                    audio_send_queue_.pop_front();
                }
                audio_send_queue_.emplace_back(std::move(packet));
                xEventGroupSetBits(event_group_, SEND_AUDIO_EVENT);
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });

    wake_word_->Initialize(codec);
    wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (!protocol_) {
                return;
            }

            if (device_state_ == kDeviceStateIdle) {
                wake_word_->EncodeWakeWordData();

                if (!protocol_->IsAudioChannelOpened()) {
                    SetDeviceState(kDeviceStateConnecting);
                    if (!protocol_->OpenAudioChannel()) {
                        wake_word_->StartDetection();
                        return;
                    }
                }

                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD
                AudioStreamPacket packet;
                // Encode and send the wake word data to the server
                while (wake_word_->GetWakeWordOpus(packet.payload)) {
                    protocol_->SendAudio(packet);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
#else
                // Play the pop up sound to indicate the wake word is detected
                // And wait 60ms to make sure the queue has been processed by audio task
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_POPUP);
                vTaskDelay(pdMS_TO_TICKS(60));
#endif
                SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    wake_word_->StartDetection();
    ESP_LOGE(TAG,"thread running here!!!!!!!!!!!!");
    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);

<<<<<<< HEAD
    // 检查设备激活状态，决定后续显示逻辑
    if (ota_.HasActivationCode() || ota_.HasActivationChallenge())
    {
        // 设备未激活，保持激活状态，不设置为idle（避免显示neutral表情）
        ESP_LOGI(TAG, "Device is not activated, keeping activation state for QR code display");
        // 激活状态已在HandleDeviceActivationAndQRCode中设置
    }
    else
    {
        // 设备已激活，可以正常进入idle状态
        ESP_LOGI(TAG, "Device is activated, entering idle state");
        SetDeviceState(kDeviceStateIdle);
<<<<<<< HEAD

        if (protocol_started)
        {
            // 只显示版本号，不显示"版本"字样，覆盖时间显示位置
=======
        
        if (protocol_started) {
            // 设备已绑定，恢复正常 UI
>>>>>>> 0f4a466141c1cd8ff8001902e870902c724f7307
            std::string message = ota_.GetCurrentVersion();
            display->SetStatus(message.c_str());
            display->SetChatMessage("system", "");
            // Play the success sound to indicate the device is ready
            ResetDecoder();
            PlaySound(Lang::Sounds::P3_SUCCESS);
        }
=======
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
>>>>>>> topd-debug
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();
    
    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (ota_.HasServerTime()) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT | SEND_AUDIO_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SEND_AUDIO_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto packets = std::move(audio_send_queue_);
            lock.unlock();
            for (auto& packet : packets) {
                if (!protocol_->SendAudio(packet)) {
                    break;
                }
            }
        }

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();

    // Synchronize the sample rate and frame duration
    SetDecodeSampleRate(packet.sample_rate, packet.frame_duration);

    busy_decoding_audio_ = true;
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            return;
        }
        // Resample if the sample rate is different
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        codec->OutputData(pcm);
#ifdef CONFIG_USE_SERVER_AEC
        std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_queue_.push_back(packet.timestamp);
#endif
        last_output_time_ = std::chrono::steady_clock::now();
    });
}

void Application::OnAudioInput() {
    if (wake_word_->IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                wake_word_->Feed(data);
                return;
            }
        }
    }
    if (audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                audio_processor_->Feed(data);
                return;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(OPUS_FRAME_DURATION_MS / 2));
}

bool Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->input_enabled()) {
        return false;
    }

    if (codec->input_sample_rate() != sample_rate) {
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return false;
        }
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return false;
        }
    }
    return true;
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_processor_->Stop();
            wake_word_->StartDetection();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            timestamp_queue_.clear();
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            // Update the IoT states before sending the start listening command
#if CONFIG_IOT_PROTOCOL_XIAOZHI
            UpdateIotStates();
#endif

            // Make sure the audio processor is running
            if (!audio_processor_->IsRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                if (previous_state == kDeviceStateSpeaking) {
                    audio_decode_queue_.clear();
                    audio_decode_cv_.notify_all();
                    // FIXME: Wait for the speaker to empty the buffer
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
                opus_encoder_->ResetState();
                audio_processor_->Start();
                wake_word_->StopDetection();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_processor_->Stop();
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                wake_word_->StartDetection();
#else
                wake_word_->StopDetection();
#endif
            }
            ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_processor_->EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_processor_->EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_processor_->EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
<<<<<<< HEAD
        
        ESP_LOGI(TAG, "=== QR Code OTA Interface Test Complete ==="); });
}

void Application::TestOTARequestFormat()
{
    ESP_LOGI(TAG, "=== Testing OTA Request Format ===");

    Schedule([this]()
             {
        auto& board = Board::GetInstance();
        
        // 生成完整的设备JSON
        std::string device_json = board.GetJson();
        
        ESP_LOGI(TAG, "Generated Device JSON:");
        ESP_LOGI(TAG, "Length: %d bytes", device_json.length());
        ESP_LOGI(TAG, "Content: %s", device_json.c_str());
        
        // 验证JSON格式
        cJSON* json = cJSON_Parse(device_json.c_str());
        if (json) {
            ESP_LOGI(TAG, "✅ JSON format is valid");
            
            // 检查必要字段
            cJSON* version = cJSON_GetObjectItem(json, "version");
            cJSON* uuid = cJSON_GetObjectItem(json, "uuid");
            cJSON* application = cJSON_GetObjectItem(json, "application");
            cJSON* board_info = cJSON_GetObjectItem(json, "board");
            cJSON* ota = cJSON_GetObjectItem(json, "ota");
            
            ESP_LOGI(TAG, "Field checks:");
            ESP_LOGI(TAG, "  version: %s", version ? "✅ Present" : "❌ Missing");
            ESP_LOGI(TAG, "  uuid: %s", uuid ? "✅ Present" : "❌ Missing");
            ESP_LOGI(TAG, "  application: %s", application ? "✅ Present" : "❌ Missing");
            ESP_LOGI(TAG, "  board: %s", board_info ? "✅ Present" : "❌ Missing");
            ESP_LOGI(TAG, "  ota: %s", ota ? "✅ Present" : "❌ Missing");
            
            if (application) {
                cJSON* name = cJSON_GetObjectItem(application, "name");
                cJSON* version = cJSON_GetObjectItem(application, "version");
                cJSON* compile_time = cJSON_GetObjectItem(application, "compile_time");
                ESP_LOGI(TAG, "  application.name: %s", name && cJSON_IsString(name) ? name->valuestring : "❌ Missing");
                ESP_LOGI(TAG, "  application.version: %s", version && cJSON_IsString(version) ? version->valuestring : "❌ Missing");
                ESP_LOGI(TAG, "  application.compile_time: %s", compile_time && cJSON_IsString(compile_time) ? compile_time->valuestring : "❌ Missing");
            }
            
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "❌ JSON format is invalid!");
            const char* error = cJSON_GetErrorPtr();
            if (error) {
                ESP_LOGE(TAG, "JSON Error: %s", error);
            }
        }
        
        // 显示预期的curl命令格式
        ESP_LOGI(TAG, "Expected curl command format:");
        ESP_LOGI(TAG, "curl -X POST \"http://core.device.158box.com/xiaozhi/ota2/\" \\");
        ESP_LOGI(TAG, "  -H \"Content-Type: application/json\" \\");
        ESP_LOGI(TAG, "  -H \"Device-Id: %s\" \\", SystemInfo::GetMacAddress().c_str());
        ESP_LOGI(TAG, "  -H \"Client-Id: %s\" \\", board.GetUuid().c_str());
        ESP_LOGI(TAG, "  -d '%s'", device_json.c_str());
        
        ESP_LOGI(TAG, "=== OTA Request Format Test Complete ==="); });
}

bool Application::GetQRCodeInfoOnly()
{
    ESP_LOGI(TAG, "=== Getting QR Code Info Only ===");
    return ota_.GetQRCodeInfoOnly();
}

void Application::WaitForDeviceAssociation()
{
    ESP_LOGI(TAG, "=== Waiting for Device Association ===");

    const int MAX_WAIT_ITERATIONS = 60; // 最多等待60次，每次5秒，总共5分钟
    int wait_count = 0;

    auto display = Board::GetInstance().GetDisplay();

    while (wait_count < MAX_WAIT_ITERATIONS)
    {
        // 检查设备是否已关联
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge())
        {
            ESP_LOGI(TAG, "✅ Device association completed!");
            return;
        }

        // 每30秒重新获取一次状态
        if (wait_count % 6 == 0)
        {
            ESP_LOGI(TAG, "Checking association status... (%d/%d)", wait_count / 6 + 1, MAX_WAIT_ITERATIONS / 6);

            // 重新获取二维码信息以检查关联状态
            if (ota_.GetQRCodeInfoOnly())
            {
                if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge())
                {
                    ESP_LOGI(TAG, "✅ Device association detected!");
                    return;
                }
            }
        }

        // 更新显示状态
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg), "等待扫码关联... %d分钟", (wait_count * 5) / 60 + 1);
        display->SetStatus(status_msg);

        // 等待5秒
        vTaskDelay(pdMS_TO_TICKS(5000));
        wait_count++;

        // 检查设备状态，如果不再是激活状态则退出等待
        if (device_state_ != kDeviceStateActivating)
        {
            ESP_LOGI(TAG, "Device state changed, exiting association wait");
            break;
        }
    }

    if (wait_count >= MAX_WAIT_ITERATIONS)
    {
        ESP_LOGW(TAG, "⚠️  Device association wait timeout after %d minutes", MAX_WAIT_ITERATIONS * 5 / 60);
        display->SetStatus("关联超时，请重启设备");
    }
}

// === 五步开机流程实现 ===

<<<<<<< HEAD
bool Application::InitializeNetworkConnection()
{
    ESP_LOGI(TAG, "初始化网络连接");

    auto &board = Board::GetInstance();

    // 检查板卡类型并启动网络（避免使用dynamic_cast）
    std::string board_type = board.GetBoardType();
    ESP_LOGI(TAG, "检测到板卡类型: %s", board_type.c_str());

    if (board_type == "topd-1.54tft-ml307-00")
    {
        ESP_LOGI(TAG, "检测到307ML网络板卡，直接启动网络");
        board.StartNetwork();
        return true;
    }
    else
    {
        ESP_LOGI(TAG, "检测到WiFi网络板卡，检查连接状态");

        // 检查WiFi是否已配置并连接
        if (!WifiStation::GetInstance().IsConnected())
        {
            ESP_LOGI(TAG, "WiFi未连接，启动配置模式");
            auto display = board.GetDisplay();
            display->SetStatus("请连接WiFi");
            display->SetChatMessage("system", "请使用手机连接设备热点进行WiFi配置");

            // 启动WiFi配置模式 - 使用公共接口
            ESP_LOGI(TAG, "启动WiFi配置模式...");
            // 注意：这里需要通过公共方法启动WiFi配置
            // 暂时直接启动网络，WiFi配置由其他机制处理
            board.StartNetwork();

            // 等待WiFi连接完成（最多等待2分钟）
            int wait_count = 0;
            while (!WifiStation::GetInstance().IsConnected() && wait_count < 120)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                wait_count++;

                if (wait_count % 10 == 0)
                {
                    ESP_LOGI(TAG, "等待WiFi连接... %d/120秒", wait_count);
                }
            }

            if (!WifiStation::GetInstance().IsConnected())
            {
                ESP_LOGE(TAG, "WiFi连接超时");
                return false;
            }
        }
        else
        {
            ESP_LOGI(TAG, "WiFi已连接，启动网络服务");
            board.StartNetwork();
        }

        return true;
    }

    return false;
=======
bool Application::InitializeNetworkConnection() {
    // 简化：主程序不关心具体联网细节，全部交由板级（Board::StartNetwork）处理
    ESP_LOGI(TAG, "初始化网络连接（由Board负责）");
    Board::GetInstance().StartNetwork();
    return true;
>>>>>>> 0f4a466141c1cd8ff8001902e870902c724f7307
}

bool Application::CheckDeviceRegistrationStatus()
{
    ESP_LOGI(TAG, "检查设备注册状态");

    // 发送POST请求到OTA服务器检查注册状态
    return ota_.GetQRCodeInfoOnly();
}

bool Application::IsDeviceRegistered()
{
    // 如果没有激活码或激活挑战，说明设备已注册
   // bool registered = !ota_.HasActivationCode() && !ota_.HasActivationChallenge();
    bool registered= false;
    ESP_LOGI(TAG, "设备注册状态: %s", registered ? "已注册" : "未注册");
    return registered;
}

bool Application::GetQRCodeDownloadUrl()
{
    ESP_LOGI(TAG, "通过POST请求获取QR码下载URL");

    // 使用OTA接口协议发送POST请求到 http://core.device.158box.com/xiaozhi/ota2/
    if (!ota_.GetQRCodeInfoOnly())
    {
        ESP_LOGE(TAG, "获取QR码信息失败");
        return false;
    }

    if (!ota_.HasWeChatCodeUrl())
    {
        ESP_LOGE(TAG, "服务器未返回QR码URL");
        return false;
    }

    std::string qr_url = ota_.GetWeChatCodeUrl();
    ESP_LOGI(TAG, "成功获取QR码URL: %s", qr_url.c_str());
    return true;
}

bool Application::DownloadAndProcessQRCode()
{
    ESP_LOGI(TAG, "下载QR码图片并转换为TFT显示格式");

    if (!ota_.HasWeChatCodeUrl())
    {
        ESP_LOGE(TAG, "没有QR码URL");
        return false;
    }

    // 下载QR码图片
    if (!ota_.DownloadAndDisplayQRCode())
    {
        ESP_LOGE(TAG, "QR码图片下载失败");
        return false;
    }

    // 检查图片数据
    const std::string &qr_data = ota_.GetQRImageData();
    if (qr_data.empty())
    {
        ESP_LOGE(TAG, "QR码图片数据为空");
        return false;
    }

    ESP_LOGI(TAG, "QR码图片下载成功，大小: %d字节", qr_data.size());

    // 图片格式转换在显示层处理（PNG 300x300 -> 240x240显示）
    return true;
}

void Application::DisplayQRCodeOnTFT()
{
    ESP_LOGI(TAG, "在TFT屏幕显示QR码");

    auto display = Board::GetInstance().GetDisplay();

    // 确保不显示任何表情
    display->SetEmotion("");

    // 设置设备状态为激活中
    SetDeviceState(kDeviceStateActivating);
    display->SetStatus("请扫码注册设备");

    // 显示QR码（240x240分辨率适配300x300 PNG）
    if (ota_.HasWeChatCodeUrl())
    {
        ShowQRCode(); 

        // 播放提示音
        //ResetDecoder();
        //PlaySound(Lang::Sounds::P3_SUCCESS);
        ESP_LOGI(TAG, "QR码显示完成");
    }
    else
    {
        ESP_LOGE(TAG, "没有QR码可显示");
        display->SetChatMessage("system", "二维码显示失败");
    }
}

void Application::WaitForDeviceRegistration()
{
    const int MAX_WAIT_ITERATIONS = 60; // 最多等待60次，每次5秒，总共5分钟
    int wait_count = 0;

    auto display = Board::GetInstance().GetDisplay();

    while (true)
    {
        //wait forever
        ESP_LOGI(TAG, "等待设备注册完成");
        vTaskDelay(pdMS_TO_TICKS(10000));
        
    }

    if (wait_count >= MAX_WAIT_ITERATIONS)
    {
        ESP_LOGW(TAG, "⚠️  设备注册等待超时，%d分钟后自动退出", MAX_WAIT_ITERATIONS * 5 / 60);
        display->SetStatus("注册超时，请重启设备");
    }
=======
    });
>>>>>>> topd-debug
}
