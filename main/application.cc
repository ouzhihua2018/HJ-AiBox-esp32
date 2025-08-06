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
            // 升级失败，保持原有提示词，然后检查设备关联状态
            display->SetStatus(Lang::Strings::UPGRADE_FAILED);
            ESP_LOGI(TAG, "Firmware upgrade failed, checking device association status...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            // 升级失败后，检查设备关联状态决定后续行为
            HandleDeviceActivationAndQRCode();
            
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            return;
        }

        // No new version, mark the current version as valid
        ota_.MarkCurrentVersionValid();
        
        // 检查设备关联状态和二维码显示逻辑
        HandleDeviceActivationAndQRCode();
        
        xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
        break;
    }
}

// 注释掉显示激活码填写地址的代码
// void Application::ShowActivationCode() {
//     auto& message = ota_.GetActivationMessage();
//     auto& code = ota_.GetActivationCode();
// 
//     struct digit_sound {
//         char digit;
//         const std::string_view& sound;
//     };
//     static const std::array<digit_sound, 10> digit_sounds{{
//         digit_sound{'0', Lang::Sounds::P3_0},
//         digit_sound{'1', Lang::Sounds::P3_1}, 
//         digit_sound{'2', Lang::Sounds::P3_2},
//         digit_sound{'3', Lang::Sounds::P3_3},
//         digit_sound{'4', Lang::Sounds::P3_4},
//         digit_sound{'5', Lang::Sounds::P3_5},
//         digit_sound{'6', Lang::Sounds::P3_6},
//         digit_sound{'7', Lang::Sounds::P3_7},
//         digit_sound{'8', Lang::Sounds::P3_8},
//         digit_sound{'9', Lang::Sounds::P3_9}
//     }};
// 
//     // This sentence uses 9KB of SRAM, so we need to wait for it to finish
//     Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);
// 
//     for (const auto& digit : code) {
//         auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
//             [digit](const digit_sound& ds) { return ds.digit == digit; });
//         if (it != digit_sounds.end()) {
//             PlaySound(it->sound);
//         }
//     }
// }

void Application::ShowQRCode() {
    auto display = Board::GetInstance().GetDisplay();
    
    ESP_LOGI(TAG, "Starting QR code display process");
    
    // 尝试从OTA下载二维码
    if (ota_.DownloadAndDisplayQRCode()) {
        // 获取下载的图片数据
        const std::string& image_data = ota_.GetQRImageData();
        
        if (!image_data.empty()) {
            // 尝试显示二维码图片
            if (display->ShowQRCodeImage(reinterpret_cast<const uint8_t*>(image_data.data()), image_data.length())) {
                // 二维码显示成功，确保不显示任何表情和状态信息
                ESP_LOGI(TAG, "QR code image displayed successfully, hiding all other UI elements");
                
                // 清除所有状态文字和聊天信息，确保屏幕只显示二维码
                display->SetStatus("");
                display->SetChatMessage("system", "");
                
                // 播放二维码显示成功的提示音
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_SUCCESS);
                ESP_LOGI(TAG, "QR code display completed with audio notification");
                
                return;
            }
        }
    }
    
    // 二维码获取或显示失败
    ESP_LOGE(TAG, "Failed to download or display QR code");
    display->SetChatMessage("system", "QR Code Failed");
    
    // 播放失败提示音，重复10次
    for (int i = 0; i < 10 && device_state_ == kDeviceStateActivating; ++i) {
        Alert(Lang::Strings::ERROR, "二维码获取失败，请检查网络设置", "sad", Lang::Sounds::P3_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(5000)); // 每5秒重复一次
    }
}

void Application::HandleDeviceActivationAndQRCode() {
    auto display = Board::GetInstance().GetDisplay();
    
    // 检查是否有激活码或激活挑战（表示设备未关联）
    if (ota_.HasActivationCode() || ota_.HasActivationChallenge()) {
        ESP_LOGI(TAG, "Device is not associated, showing QR code for activation");
        
        // 设置设备状态为激活中，避免后续被覆盖
        SetDeviceState(kDeviceStateActivating);
        display->SetStatus(Lang::Strings::ACTIVATION);
        
        // 设备未关联，显示二维码
        if (ota_.HasWeChatCodeUrl()) {
            ShowQRCode();
        } else {
            ESP_LOGW(TAG, "No QR code URL available for unassociated device");
            display->SetChatMessage("system", "QR Code Not Available");
        }
        
        // 执行激活流程
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_.Activate();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Device activation successful");
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
    } else {
        ESP_LOGI(TAG, "Device is already associated, proceeding with normal operation");
        // 设备已关联，按原有固件版本正常使用，不显示二维码
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetChatMessage("system", "");
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

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    
    // 检查设备激活状态，决定后续显示逻辑
    if (ota_.HasActivationCode() || ota_.HasActivationChallenge()) {
        // 设备未激活，保持激活状态，不设置为idle（避免显示neutral表情）
        ESP_LOGI(TAG, "Device is not activated, keeping activation state for QR code display");
        // 激活状态已在HandleDeviceActivationAndQRCode中设置
    } else {
        // 设备已激活，可以正常进入idle状态
        ESP_LOGI(TAG, "Device is activated, entering idle state");
        SetDeviceState(kDeviceStateIdle);
        
        if (protocol_started) {
            // 只显示版本号，不显示"版本"字样，覆盖时间显示位置
            std::string message = ota_.GetCurrentVersion();
            // 将版本信息显示在屏幕顶部居中位置
            display->SetStatus(message.c_str());
            // 清除聊天消息区域，确保版本信息在顶部显示
            display->SetChatMessage("system", "");
            // Play the success sound to indicate the device is ready
            ResetDecoder();
            PlaySound(Lang::Sounds::P3_SUCCESS);
        }
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

        // If we have synchronized server time, show time centered if the device is idle
        if (ota_.HasServerTime()) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // Show only time, centered in status bar
                    time_t now = time(NULL);
                    char time_str[32];
                    strftime(time_str, sizeof(time_str), "%H:%M", localtime(&now));
                    
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
        case kDeviceStateActivating:
            // 在激活状态下，不设置表情，保持二维码显示
            // 状态信息已在HandleDeviceActivationAndQRCode中设置
            ESP_LOGI(TAG, "Device in activation state, keeping QR code display");
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
    });
}

void Application::TestQRCodeFunction() {
    ESP_LOGI(TAG, "=== Starting QR Code OTA Interface Test ===");
    
    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus("Testing QR Code");
        display->SetChatMessage("system", "正在测试OTA服务器二维码获取...");
        
        // 测试1: 检查OTA URL配置
        std::string ota_url = ota_.GetCheckVersionUrl();
        ESP_LOGI(TAG, "Step 1: OTA Server URL: %s", ota_url.c_str());
        
        // 测试2: 检查设备JSON payload
        auto& board = Board::GetInstance();
        std::string device_json = board.GetJson();
        ESP_LOGI(TAG, "Step 2: Device JSON payload length: %d bytes", device_json.length());
        ESP_LOGI(TAG, "Device JSON (first 300 chars): %.300s", device_json.c_str());
        
        // 测试3: 调用CheckVersion获取二维码URL
        ESP_LOGI(TAG, "Step 3: Calling CheckVersion to get QR code URL from OTA server");
        display->SetChatMessage("system", "正在连接OTA服务器...");
        
        bool version_check = ota_.CheckVersion();
        ESP_LOGI(TAG, "Step 3 Result: CheckVersion returned %s", version_check ? "SUCCESS" : "FAILED");
        
        if (version_check) {
            // 测试4: 检查是否获取到了QR码URL
            if (ota_.HasWeChatCodeUrl()) {
                std::string qr_url = ota_.GetWeChatCodeUrl();
                ESP_LOGI(TAG, "Step 4: ✅ Got QR code URL: %s", qr_url.c_str());
                display->SetChatMessage("system", "获取到二维码链接，正在下载图片...");
                
                // 测试5: 下载二维码图片
                ESP_LOGI(TAG, "Step 5: Attempting to download QR code image");
                bool download_success = ota_.DownloadAndDisplayQRCode();
                ESP_LOGI(TAG, "Step 5 Result: Download returned %s", download_success ? "SUCCESS" : "FAILED");
                
                if (download_success) {
                    const std::string& image_data = ota_.GetQRImageData();
                    if (!image_data.empty()) {
                        ESP_LOGI(TAG, "Step 6: ✅ QR code image downloaded successfully, size: %d bytes", image_data.length());
                        
                        // 检查PNG头部
                        if (image_data.length() >= 8) {
                            const uint8_t* data = reinterpret_cast<const uint8_t*>(image_data.data());
                            ESP_LOGI(TAG, "PNG Header: %02X %02X %02X %02X %02X %02X %02X %02X", 
                                    data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
                        }
                        
                        Alert("测试成功", "OTA服务器二维码获取正常", "happy", Lang::Sounds::P3_SUCCESS);
                        
                        // 显示获取到的二维码
                        if (display->ShowQRCodeImage(reinterpret_cast<const uint8_t*>(image_data.data()), image_data.length())) {
                            ESP_LOGI(TAG, "Step 7: ✅ QR code displayed successfully");
                        } else {
                            ESP_LOGE(TAG, "Step 7: ❌ QR code display failed");
                        }
                    } else {
                        ESP_LOGW(TAG, "Step 6: ⚠️  Download success but no image data");
                        Alert("测试警告", "下载成功但无图片数据", "neutral", Lang::Sounds::P3_POPUP);
                    }
                } else {
                    ESP_LOGE(TAG, "Step 5: ❌ QR code image download failed");
                    Alert("下载失败", "二维码图片下载失败", "sad", Lang::Sounds::P3_EXCLAMATION);
                }
            } else {
                ESP_LOGW(TAG, "Step 4: ⚠️  No QR code URL found in OTA response");
                ESP_LOGW(TAG, "Server response may be missing 'weChat.codeUrl' field");
                Alert("测试警告", "服务器未返回二维码链接", "neutral", Lang::Sounds::P3_POPUP);
            }
        } else {
            ESP_LOGE(TAG, "Step 3: ❌ CheckVersion failed");
            ESP_LOGE(TAG, "Possible causes:");
            ESP_LOGE(TAG, "  - Network connectivity issue");
            ESP_LOGE(TAG, "  - Server error (HTTP 500 from %s)", ota_url.c_str());
            ESP_LOGE(TAG, "  - Invalid request format");
            ESP_LOGE(TAG, "  - Missing required headers");
            Alert("测试失败", "OTA服务器连接失败", "sad", Lang::Sounds::P3_EXCLAMATION);
        }
        
        ESP_LOGI(TAG, "=== QR Code OTA Interface Test Complete ===");
    });
}

void Application::TestOTARequestFormat() {
    ESP_LOGI(TAG, "=== Testing OTA Request Format ===");
    
    Schedule([this]() {
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
        
        ESP_LOGI(TAG, "=== OTA Request Format Test Complete ===");
    });
}
