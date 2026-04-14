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
#include "audio_debugger.h"
#include "settings.h"
#include "core_monitor.h"

#include <cstring>
#include <cmath>
#include <algorithm>

#define TAG "Application"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#else
#include "no_audio_processor.h"
#endif
#include "afe_wake_word.h"




#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include "led/circular_strip.h"
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
    "low_battery",
    "fatal_error",
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
   wake_word_ = std::make_unique<AfeWakeWord>();
   micro_wake_word_ = std::make_unique<MicroWakeWordDetect>();

   
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
    
    esp_timer_create_args_t microwakeword_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->micro_wake_word_->StartDetection();
            if(!app->protocol_->IsAudioChannelOpened()) app->wake_word_->StartDetection();
           
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "microwakeword",
        .skip_unhandled_events = true
    };
    esp_timer_create(&microwakeword_timer_args, &microwakeword_timer_handle_);

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
            micro_wake_word_->Stop();
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
            Reboot();
            return;
        }

        // No new version, mark the current version as valid
        ota_.MarkCurrentVersionValid();
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_.HasActivationCode()) {
            ShowActivationCode();
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
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

void Application::RequestLowBatteryHalt() {
    Schedule([this]() {
        if (device_state_ == kDeviceStateLowBattery) {
            return;
        }
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        if (device_state_ == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
        }
        ResetDecoder();
        SetDeviceState(kDeviceStateLowBattery);
        PlaySound(Lang::Sounds::P3_BATTERYLOW);
    });
}

void Application::ClearLowBatteryHalt() {
    Schedule([this]() {
        if (device_state_ == kDeviceStateLowBattery) {
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    //ESP_LOGI(TAG,"进入PlaySound");
    auto codec =  Board::GetInstance().GetAudioCodec();
    //ESP_LOGI(TAG,"当前OUTPUT_ENABLE:%d",codec->output_enabled());
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ESP_LOGI(TAG,"PLAY SOUND查看队列是否清空 已获取锁");
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
        //ESP_LOGI(TAG,"解码队列已清空，已获取锁");
    }
    ESP_LOGI(TAG,"PLAY SOUND离开条件变量作用域，释放锁");
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
        ESP_LOGI(TAG,"PlaySound已获取锁");
        audio_decode_queue_.emplace_back(std::move(packet));
    }
   
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateLowBattery) {
        return;
    }
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
            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            ESP_LOGE(TAG,"ToggleChatState abort");
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateLowBattery) {
        return;
    }
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
            ESP_LOGE(TAG,"Start Listing Abort");
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
    auto display = board.GetDisplay();

    /* Setup the audio codec */
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
    codec->Start();  // audio codec enable

#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop(); //循环处理输入输出数据，输入I2S读取送入wake_word、audio proc，输出 opus解码播放
        vTaskDelete(NULL);  //贴近codec这里
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
    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    CheckNewVersion();

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
        if (device_state_ == kDeviceStateLowBattery) {
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        //ESP_LOGI(TAG,"Audio comming");
        std::lock_guard<std::mutex> lock(mutex_);
        //ESP_LOGI(TAG,"Audio comming,IncomingAudio 已获取锁");
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
            if (device_state_ == kDeviceStateLowBattery) {
                return;
            }
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        //ESP_LOGW(TAG,"IncomingJson:%s",cJSON_Print(root));
        // Parse JSON data  
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                ESP_LOGI(TAG, "tts start");
                Schedule([this]() {
                    if (device_state_ == kDeviceStateLowBattery) {
                        return;
                    }
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "tts stop");
                Schedule([this]() {
                    if (device_state_ == kDeviceStateLowBattery) {
                        return;
                    }
                    background_task_->WaitForCompletion();
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            ESP_LOGI(TAG, "tts stop,set idel state");
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            ESP_LOGI(TAG, "tts stop,set listening state");
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
            if (device_state_ == kDeviceStateLowBattery) {
                return;
            }
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
            
        } else if (strcmp(type->valuestring, "Hei") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            auto state = cJSON_GetObjectItem(root, "state");
            auto timestamp = cJSON_GetObjectItem(root, "timestamp");
            if (cJSON_IsString(session_id) && cJSON_IsString(state) && cJSON_IsNumber(timestamp)) {
                ESP_LOGW(TAG,"receive message: session_id %s",cJSON_GetStringValue(session_id));
                 // 关键修复：提前提取「值」（拷贝到局部变量），而非捕获指针
                std::string session_id_str = cJSON_GetStringValue(session_id); // 拷贝 session_id 字符串
                int64_t timestamp_val = static_cast<int64_t>(timestamp->valuedouble); // 拷贝 timestamp 数值
                Schedule([this,session_id_str,timestamp_val]() {
                    if (device_state_ == kDeviceStateLowBattery) {
                        return;
                    }
                    std::string json_str = "{"
                    "\"session_id\":\"" + session_id_str + "\","  // 复用输入的session_id
                    "\"type\":\"Hei\"," + // 固定type为"Hei"
                    "\"timestamp\":" + std::to_string(timestamp_val) + ",";
                    
                    if (!protocol_->IsAudioChannelOpened()) {
                        SetDeviceState(kDeviceStateConnecting);
                        if (!protocol_->OpenAudioChannel()) {
                            json_str += "\"state\":\"error\",";
                            json_str += "\"describe\":\"Failed to open audio channel\"";
                            json_str += "}";
                            protocol_->SendText(json_str);
                            return;
                        } else {
                            json_str += "\"state\":\"success\",";
                            json_str += "\"describe\":\"Successfully opened the audio channel\"";
                            json_str += "}";
                            protocol_->SendStartListening(listening_mode_);  //模拟对话流程
                            protocol_->SendEmptyPacket();
                            SetDeviceState(kDeviceStateSpeaking);
                            protocol_->SendText(json_str); 
                            return;
                        }
                    } 
                    json_str += "\"state\":\"success\",";
                    json_str += "\"describe\":\"Successfully opened the audio channel\"";
                    json_str += "}";
                    SetDeviceState(kDeviceStateSpeaking);
                    protocol_->SendText(json_str);
            });
        } else {
                ESP_LOGW(TAG, "Cjson format error");
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    audio_debugger_ = std::make_unique<AudioDebugger>();
    audio_processor_->Initialize(codec);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            //ESP_LOGI(TAG,"音频录入，OnOutput已获取锁");
            if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                ESP_LOGW(TAG, "Too many audio packets in queue, drop the newest packet");
                return;
            }
        }
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
                //ESP_LOGI(TAG," opus_encoder已获取锁");
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
                auto led2 = Board::GetInstance().GetLed2();
                led2->OnStateChanged();
            });
        }
    });
    wake_word_->Initialize(codec);
    micro_wake_word_->OnWakeWordDetected([this](const std::string& wake_word){
        Schedule([this, &wake_word]() {
            if (device_state_ == kDeviceStateLowBattery) {
                return;
            }
            wake_word_->StopDetection();
            if (!protocol_) {
                return;
            }
            if (device_state_ == kDeviceStateIdle) {
                if (!protocol_->IsAudioChannelOpened()) {
                    SetDeviceState(kDeviceStateConnecting);
                    if (!protocol_->OpenAudioChannel()) {
                        micro_wake_word_->StartDetection();
                        wake_word_->StartDetection();
                        return;
                    }
                }
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_POPUP);
                vTaskDelay(pdMS_TO_TICKS(60));
                SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    }
    );
    micro_wake_word_->InitializeWakeWordDetect();
    // micro_wake_word_->StartDetection();
    // wake_word_->StartDetection();
    esp_timer_start_once(microwakeword_timer_handle_,1000*1000*3);
    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    

    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    SetDeviceState(kDeviceStateIdle);
    // Print heap stats
    SystemInfo::PrintHeapStats();
    //start_core1_monitor();
    // Enter the main event loop
    MainEventLoop(); //经过AFE处理后的音频发送任务和主要调度任务
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 3 == 0) {
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
        //ESP_LOGI(TAG,"主要调度器事件获取锁");
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT | SEND_AUDIO_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SEND_AUDIO_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            //ESP_LOGI(TAG,"音频发送事件已获取锁");
            auto packets = std::move(audio_send_queue_);
            lock.unlock();
            for (auto& packet : packets) {
                if (!protocol_->SendAudio(packet)) {
                    break;
                }
                //ESP_LOGI(TAG,"音频发送成功");
            }
        }

        if (bits & SCHEDULE_EVENT) {   //设备状态切换
            std::unique_lock<std::mutex> lock(mutex_);
            ESP_LOGI(TAG,"主循环调度器事件已获取锁");
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
            ///ESP_LOGI(TAG,"输出使能");
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        //ESP_LOGW(TAG, "OnAudioOutput skipped because busy_decoding_audio_ == true");
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    //ESP_LOGI(TAG,"AudioOutput锁已获取");
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateLowBattery) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }
    //ESP_LOGI(TAG,"解码队列退包");
    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();
    //ESP_LOGI(TAG,"AudioOutput已释放锁，并通知条件变量");
    // Synchronize the sample rate and frame duration
    SetDecodeSampleRate(packet.sample_rate, packet.frame_duration);
    //ESP_LOGI(TAG,"SetDecodeSampleRate");
    busy_decoding_audio_ = true;    
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        //ESP_LOGI(TAG,"正在执行背景任务");
        busy_decoding_audio_ = false;
        if (aborted_) {
            ESP_LOGI(TAG,"aborted!");
            return;
        }
        //ESP_LOGW(TAG,"DecodeTask: before Decode");
        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            ESP_LOGI(TAG,"解码OPUS失败");
            return;
        }
        
        // 计算音频能量并更新LED灯条
        if(kDeviceStateSpeaking == this->GetDeviceState()){
            float audio_level = CalculateAudioRMS(pcm);
            UpdateLedWithAudioLevel(audio_level);
        }
        
        // Resample if the sample rate is different
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        //ESP_LOGW(TAG,"DecodeTask: after Decode, before OutputData");
        codec->OutputData(pcm);
        //ESP_LOGW(TAG,"DecodeTask: after OutputData");
#ifdef CONFIG_USE_SERVER_AEC
        std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_queue_.push_back(packet.timestamp);
#endif
        last_output_time_ = std::chrono::steady_clock::now();
    });
}
void PrintAllChannels(const std::vector<int16_t>& raw_data) {
    if (raw_data.size() < 4) return;
    ESP_LOGI("ChannelDebug", "第1帧数据：");
    ESP_LOGI("ChannelDebug", "通道1(MIC1)：%d", raw_data[0]);
    ESP_LOGI("ChannelDebug", "通道3(DAC)：%d", raw_data[1]);
    ESP_LOGI("ChannelDebug", "通道2(MIC2)：%d", raw_data[2]);
    ESP_LOGI("ChannelDebug", "通道4(空)：%d", raw_data[3]);
}
// 从4通道TDM数据中提取麦克风1（通道1）的纯数据
std::vector<int16_t> ExtractMic1Data(const std::vector<int16_t>& raw_data) {
    std::vector<int16_t> mic1_data;
    mic1_data.reserve(raw_data.size() / 4); // 预分配空间（4通道→1通道，数据量减为1/4）

    // TDM数据格式：每4个int16_t对应1帧（MIC1、MIC3、MIC2、MIC4）
    // 遍历原始数据，只取每4个值中的第1个（MIC1）
    for (size_t i = 0; i < raw_data.size(); i += 4) {
        mic1_data.push_back(raw_data[i]);
    }

    ESP_LOGD("AudioExtract", "提取MIC1数据：原始%d个采样点 → MIC1%d个采样点",
             raw_data.size(), mic1_data.size());
    return mic1_data;
}
void Application::OnAudioInput() {
    if (wake_word_->IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_->GetFeedSize();  //512*2 = 1024个采样点 1个采样点2byte
        if (samples > 0) {
            //ESP_LOGI(TAG,"samples :%d",samples);
            if (ReadAudio(data, 16000, samples)) {
                wake_word_->Feed(data);
                return;
            }
        }
 
    }
  
    if (audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();//由于初始化时是MR，那feedsize应该是512*2
        
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                audio_processor_->Feed(data);
                return;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(OPUS_FRAME_DURATION_MS / 2));
}
 //这里的samples只是数据大小，比如单通道，那samples=1就是读一个通道，4通道的也只是读一个，要读一帧samples=4
bool Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->input_enabled()) {
        return false;
    }

    if (codec->input_sample_rate() != sample_rate) {
        //ESP_LOGI(TAG,"samples diff %d ->%d",codec->input_sample_rate(),sample_rate);
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
            } //默认偶数为refer
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
    //PrintAllChannels(data);
    // 音频调试：发送原始音频数据
    // if (audio_debugger_) {
    //     audio_debugger_->Feed(data);
    // }
    
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
    ESP_LOGW(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
   
    auto led2 = board.GetLed2();
    led2->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_processor_->Stop();
            if(!micro_wake_word_->IsRunning()){
                micro_wake_word_->StartDetection();
                wake_word_->StartDetection();
            }
            board.StartRfidScan();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            timestamp_queue_.clear();
            board.StopRfidScan();
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

                if(micro_wake_word_->IsRunning()){
                    wake_word_->StopDetection();
                    micro_wake_word_->Stop();
                }
            }
            board.StopRfidScan();
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            //由于鼎乐并不支持实时模式，暂时屏蔽这条判断
            if (listening_mode_ != kListeningModeRealtime) {
                ESP_LOGI(TAG,"非实时模式");
                audio_processor_->Stop();
                // Only AFE wake word can be detected in speaking mode 
                if(!micro_wake_word_->IsRunning()){
                    micro_wake_word_->StartDetection();
                    wake_word_->StartDetection();
                }
            }
            ResetDecoder();
            board.StopRfidScan();
            break;
        case kDeviceStateLowBattery:
            ESP_LOGW(TAG,"low bat state");
            display->SetStatus(Lang::Strings::BATTERY_NEED_CHARGE);
            display->SetEmotion("sad");
            audio_processor_->Stop();
            if (micro_wake_word_->IsRunning()) {
                micro_wake_word_->Stop();
            }
            wake_word_->StopDetection();
            board.StopRfidScan();
            board.StopMotorWork();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    ESP_LOGI(TAG,"RESET DECODER 已获取锁");
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

float Application::CalculateAudioRMS(const std::vector<int16_t>& audio_data) {
    if (audio_data.empty()) {
        return 0.0f;
    }

    // 计算均方根(RMS)值
    // audio_data中的每个元素是一个16位音频采样点
    // 对于60ms的音频帧，大约有960个采样点(16000Hz采样率 * 0.06s)
    int64_t sum_squares = 0;
    for (const auto& sample : audio_data) {
        sum_squares += static_cast<int64_t>(sample) * sample;
    }

    return std::sqrt(static_cast<float>(sum_squares) / audio_data.size());
}

void Application::UpdateLedWithAudioLevel(float rms_value) {
    // 获取LED设备
    auto& board = Board::GetInstance();
    auto led = static_cast<CircularStrip*>(board.GetLed2()); // 使用第二个LED灯条
    
    if (led == nullptr) {
        return;
    }
    
    // 将RMS值映射到颜色和亮度 (0-100%)
    // 假设最大RMS值为10000 (经验值，可根据实际情况调整)
    float normalized_level = std::min(1.0f, rms_value / 10000.0f);
    
    // 根据音频能量计算RGB颜色，实现更丰富的颜色变化
    // 低能量时显示绿色，中低能量显示青色，中等能量显示蓝色，
    // 中高能量显示紫色，高能量显示红色
    StripColor color;
    if (normalized_level < 0.2f) {
        // 绿色到青色过渡 (0-20%音量)
        float ratio = normalized_level / 0.2f;
        color.red = 0;
        color.green = 255;
        color.blue = static_cast<uint8_t>(255 * ratio);
    } else if (normalized_level < 0.4f) {
        // 青色到蓝色过渡 (20-40%音量)
        float ratio = (normalized_level - 0.2f) / 0.2f;
        color.red = 0;
        color.green = static_cast<uint8_t>(255 * (1 - ratio));
        color.blue = 255;
    } else if (normalized_level < 0.6f) {
        // 蓝色到紫色过渡 (40-60%音量)
        float ratio = (normalized_level - 0.4f) / 0.2f;
        color.red = static_cast<uint8_t>(255 * ratio);
        color.green = 0;
        color.blue = 255;
    } else if (normalized_level < 0.8f) {
        // 紫色到红色过渡 (60-80%音量)
        float ratio = (normalized_level - 0.6f) / 0.2f;
        color.red = 255;
        color.green = 0;
        color.blue = static_cast<uint8_t>(255 * (1 - ratio));
    } else {
        // 红色到白色过渡 (80-100%音量)
        float ratio = (normalized_level - 0.8f) / 0.2f;
        color.red = 255;
        color.green = static_cast<uint8_t>(255 * ratio);
        color.blue = static_cast<uint8_t>(255 * ratio);
    }
    
    // 调整亮度
    color.red = static_cast<uint8_t>(color.red * normalized_level);
    color.green = static_cast<uint8_t>(color.green * normalized_level);
    color.blue = static_cast<uint8_t>(color.blue * normalized_level);
    
    // 点亮所有LED
    led->SetAllColor(color);
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateLowBattery) {
        return;
    }
    if (device_state_ == kDeviceStateIdle) {
        ESP_LOGE(TAG,":WakeWordInvoke");
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

void Application::EmergencyWake()
{   
    if (GetDeviceState() == kDeviceStateLowBattery) {
        return;
    }
    if(ota_.HasServerTime()){ //服务器同步过时间
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t beijing_timestamp_ms  =(int64_t) tv.tv_sec * 1000 + (tv.tv_usec + 500) / 1000;
        // 2. 东八区转 UTC：减去 8 小时（8×3600×1000 = 28800000 毫秒）
        int64_t utc_timestamp_ms = beijing_timestamp_ms - 8 * 3600 * 1000;
        std::string mac_address = SystemInfo::GetMacAddress();
        ESP_LOGI("EmergencyWake", "UTC时间戳(ms)：%" PRId64, utc_timestamp_ms);

        protocol_->SendEmergencyMessage(utc_timestamp_ms,mac_address);
    }
}
void Application::CharacterSwitch(uint8_t* uid,size_t size)
{   
    if (GetDeviceState() == kDeviceStateLowBattery) {
        return;
    }
    char uid_c[size] = {0};
    int ret = sprintf(uid_c,"%X %X %X %X",uid[0],uid[1],uid[2],uid[3]);
    std::string uid_s(uid_c);
    if(ret<0){
        ESP_LOGE(TAG,"角色切换组包失败");
        return ;
    }
    std::string mac_address = SystemInfo::GetMacAddress();
    protocol_->SendRfidMessage(mac_address,uid_s);

}
bool Application::CanEnterSleepMode() {
    if (device_state_ == kDeviceStateLowBattery) {
        return false;
    }
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
        if (device_state_ == kDeviceStateLowBattery) {
            return;
        }
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        if (device_state_ == kDeviceStateLowBattery) {
            return;
        }
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
    });
}
