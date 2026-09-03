#include "application.h"
#include "board.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "audio_debugger.h"
#include "settings.h"
#include "passthrough_audio_processor.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <cstdint>

#include <esp_log.h>
#include <cJSON.h>
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
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 7);

#if CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    audio_processor_ = std::make_unique<PassthroughAudioProcessor>();

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
    int retry_delay = 10;

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        ESP_LOGI(TAG, "%s", Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota_.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota_.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;
            continue;
        }
        retry_count = 0;
        retry_delay = 10;

        if (ota_.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING);
            vTaskDelay(pdMS_TO_TICKS(1000));
            SetDeviceState(kDeviceStateUpgrading);

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
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

            ota_.StartUpgrade([](int progress, size_t speed) {
                ESP_LOGI(TAG, "OTA %d%% %uKB/s", progress, speed / 1024);
            });

            ESP_LOGE(TAG, "%s", Lang::Strings::UPGRADE_FAILED);
            vTaskDelay(pdMS_TO_TICKS(3000));
            Reboot();
            return;
        }

        ota_.MarkCurrentVersionValid();
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            break;
        }

        ESP_LOGI(TAG, "%s", Lang::Strings::ACTIVATION);
        if (ota_.HasActivationCode()) {
            ShowActivationCode();
        }

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
    ESP_LOGW(TAG, "Activation: %s code=%s", message.c_str(), code.c_str());
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    (void)sound;
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
}

void Application::DismissAlert() {
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        if (always_conversation_mode_) {
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            EnterConversationMode();
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        if (always_conversation_mode_) {
            return;
        }
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        if (always_conversation_mode_) {
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            EnterConversationMode();
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeRealtime);
        });
    }
}

void Application::StopListening() {
    if (always_conversation_mode_) {
        return;
    }
    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
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

    auto codec = board.GetAudioCodec();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, 1);

    esp_timer_start_periodic(clock_timer_handle_, 1000000);
    board.StartNetwork();

    CheckNewVersion();

    ESP_LOGI(TAG, "%s", Lang::Strings::LOADING_PROTOCOL);

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
        if (always_conversation_mode_) {
            ESP_LOGW(TAG, "Network error in always conversation: %s, will retry", message.c_str());
            Alert(Lang::Strings::ERROR, message.c_str());
            ScheduleEnterConversationMode();
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str());
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
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(!always_conversation_mode_);
        Schedule([this]() {
            if (always_conversation_mode_) {
                ESP_LOGI(TAG, "Audio channel closed, re-enter conversation");
                EnterConversationMode();
            } else {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    protocol_->OnIncomingJson([this](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                ESP_LOGI(TAG, "tts start");
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "tts stop");
                Schedule([this]() {
                    background_task_->WaitForCompletion();
                    if (device_state_ == kDeviceStateSpeaking) {
                        SetDeviceState(kDeviceStateListening);
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                ESP_LOGI(TAG, "emotion: %s", emotion->valuestring);
            }
#if CONFIG_IOT_PROTOCOL_MCP
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
#endif
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() {
                        Reboot();
                    });
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring);
            }
        } else if (strcmp(type->valuestring, "Hei") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            auto state = cJSON_GetObjectItem(root, "state");
            auto timestamp = cJSON_GetObjectItem(root, "timestamp");
            if (cJSON_IsString(session_id) && cJSON_IsString(state) && cJSON_IsNumber(timestamp)) {
                std::string session_id_str = cJSON_GetStringValue(session_id);
                int64_t timestamp_val = static_cast<int64_t>(timestamp->valuedouble);
                Schedule([this, session_id_str, timestamp_val]() {
                    std::string json_str = "{"
                    "\"session_id\":\"" + session_id_str + "\","
                    "\"type\":\"Hei\","
                    "\"timestamp\":" + std::to_string(timestamp_val) + ",";

                    if (!protocol_->IsAudioChannelOpened()) {
                        SetDeviceState(kDeviceStateConnecting);
                        if (!protocol_->OpenAudioChannel()) {
                            json_str += "\"state\":\"error\",";
                            json_str += "\"describe\":\"Failed to open audio channel\"";
                            json_str += "}";
                            protocol_->SendText(json_str);
                            return;
                        }
                        json_str += "\"state\":\"success\",";
                        json_str += "\"describe\":\"Successfully opened the audio channel\"";
                        json_str += "}";
                        protocol_->SendStartListening(listening_mode_);
                        protocol_->SendEmptyPacket();
                        SetDeviceState(kDeviceStateSpeaking);
                        protocol_->SendText(json_str);
                        return;
                    }
                    json_str += "\"state\":\"success\",";
                    json_str += "\"describe\":\"Successfully opened the audio channel\"";
                    json_str += "}";
                    SetDeviceState(kDeviceStateSpeaking);
                    protocol_->SendText(json_str);
                });
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
                    if (timestamp_queue_.size() > 3) {
                        timestamp_queue_.pop_front();
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

    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

    if (protocol_started) {
        ESP_LOGI(TAG, "%s%s", Lang::Strings::VERSION, ota_.GetCurrentVersion().c_str());
        always_conversation_mode_ = true;
        ESP_LOGI(TAG, "Init done, enter always conversation mode");
        EnterConversationMode();
    } else {
        SetDeviceState(kDeviceStateIdle);
    }
    SystemInfo::PrintHeapStats();
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;
    if (clock_ticks_ % 10 == 0) {
        SystemInfo::PrintHeapStats();
    }
}

void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

void Application::MainEventLoop() {
    vTaskPrioritySet(NULL, 3);

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
        aborted_ = false;
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

    SetDecodeSampleRate(packet.sample_rate, packet.frame_duration);
    busy_decoding_audio_ = true;
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }
        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            ESP_LOGI(TAG, "OPUS decode failed");
            return;
        }
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
        auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
        input_resampler_.Process(data.data(), data.size(), resampled.data());
        data = std::move(resampled);
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return false;
        }
    }

    if (audio_debugger_) {
        audio_debugger_->Feed(data);
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

void Application::ScheduleEnterConversationMode() {
    xTaskCreate([](void* arg) {
        auto* app = static_cast<Application*>(arg);
        vTaskDelay(pdMS_TO_TICKS(2000));
        app->Schedule([app]() {
            app->EnterConversationMode();
        });
        vTaskDelete(NULL);
    }, "conv_retry", 3072, this, 1, nullptr);
}

void Application::EnterConversationMode() {
    if (!always_conversation_mode_ || !protocol_) {
        return;
    }
    if (device_state_ == kDeviceStateUpgrading ||
        device_state_ == kDeviceStateWifiConfiguring) {
        return;
    }
    if (entering_conversation_) {
        return;
    }
    entering_conversation_ = true;

    ESP_LOGI(TAG, "Enter conversation mode");
    if (!protocol_->IsAudioChannelOpened()) {
        SetDeviceState(kDeviceStateConnecting);
        if (!protocol_->OpenAudioChannel()) {
            ESP_LOGW(TAG, "OpenAudioChannel failed, retry later");
            entering_conversation_ = false;
            ScheduleEnterConversationMode();
            return;
        }
    }
    SetListeningMode(kListeningModeRealtime);
    entering_conversation_ = false;
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }

    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGW(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    background_task_->WaitForCompletion();

    auto led = Board::GetInstance().GetLed();
    led->OnStateChanged();

    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            audio_processor_->Stop();
            break;
        case kDeviceStateConnecting:
            timestamp_queue_.clear();
            break;
        case kDeviceStateListening:
            if (!audio_processor_->IsRunning()) {
                protocol_->SendStartListening(listening_mode_);
                if (previous_state == kDeviceStateSpeaking) {
                    audio_decode_queue_.clear();
                    audio_decode_cv_.notify_all();
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
                opus_encoder_->ResetState();
                audio_processor_->Start();
            }
            break;
        case kDeviceStateSpeaking:
            // Full duplex: keep uplink running while the AI talks back on the line.
            if (!audio_processor_->IsRunning()) {
                protocol_->SendStartListening(listening_mode_);
                opus_encoder_->ResetState();
                audio_processor_->Start();
            }
            ResetDecoder();
            break;
        default:
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

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}
