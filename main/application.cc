#include "application.h"
#include "board.h"
#include "display.h"
#include "display/emote_display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "xiaoclaw_ws_client.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "esp_netif_sntp.h"

#include <cstring>
#include <new>
#include <vector>
#include <algorithm>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    esp_timer_create_args_t recog_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->Schedule([app]() {
                auto state = app->GetDeviceState();
                if (state == kDeviceStateRecognizing) {
                    ESP_LOGW(TAG, "recognizing timeout -> idle");
                    if (app->xiaoclaw_ws_client_) {
                        app->xiaoclaw_ws_client_->SetUploadingEnabled(false);
                    }
                    app->SetDeviceState(kDeviceStateIdle);
                }
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "recog_timeout",
        .skip_unhandled_events = true
    };
    esp_timer_create(&recog_timer_args, &xiaoclaw_recognizing_timeout_timer_);

    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->Schedule([app]() {
                app->TryXiaoClawReconnect();
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "xc_reconnect",
        .skip_unhandled_events = true
    };
    esp_timer_create(&reconnect_timer_args, &xiaoclaw_reconnect_timer_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (xiaoclaw_recognizing_timeout_timer_ != nullptr) {
        esp_timer_stop(xiaoclaw_recognizing_timeout_timer_);
        esp_timer_delete(xiaoclaw_recognizing_timeout_timer_);
    }
    if (xiaoclaw_reconnect_timer_ != nullptr) {
        esp_timer_stop(xiaoclaw_reconnect_timer_);
        esp_timer_delete(xiaoclaw_reconnect_timer_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_pcm_output = nullptr;
    callbacks.on_opus_frame = [this](const uint8_t* data, size_t len) {
        this->OnOpusFrameFromAudio(data, len);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);

        if (new_state == kDeviceStateRecognizing) {
            StartRecognizingTimeout();
        } else if (new_state == kDeviceStateThinking ||
                   new_state == kDeviceStateSynthesizing ||
                   new_state == kDeviceStateSpeaking ||
                   new_state == kDeviceStateIdle ||
                   new_state == kDeviceStateError ||
                   new_state == kDeviceStateReconnecting) {
            CancelRecognizingTimeout();
        }
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        Schedule([this, event, data]() {
            auto display = Board::GetInstance().GetDisplay();

            switch (event) {
                case NetworkEvent::Scanning:
                    display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                    xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                    break;
                case NetworkEvent::Connecting: {
                    if (data.empty()) {
                        // Cellular network - registering without carrier info yet
                        display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                    } else {
                        // WiFi or cellular with carrier info
                        std::string msg = Lang::Strings::CONNECT_TO;
                        msg += data;
                        msg += "...";
                        display->ShowNotification(msg.c_str(), 30000);
                    }
                    break;
                }
                case NetworkEvent::Connected: {
                    std::string msg = Lang::Strings::CONNECTED_TO;
                    msg += data;
                    display->ShowNotification(msg.c_str(), 30000);
                    xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                    break;
                }
                case NetworkEvent::Disconnected:
                    xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                    break;
                case NetworkEvent::WifiConfigModeEnter:
                    // WiFi config mode enter is handled by WifiBoard internally
                    break;
                case NetworkEvent::WifiConfigModeExit:
                    // WiFi config mode exit is handled by WifiBoard internally
                    break;
                // Cellular modem specific events
                case NetworkEvent::ModemDetecting:
                    display->SetStatus(Lang::Strings::DETECTING_MODULE);
                    break;
                case NetworkEvent::ModemErrorNoSim:
                    Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                    break;
                case NetworkEvent::ModemErrorRegDenied:
                    Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                    break;
                case NetworkEvent::ModemErrorInitFailed:
                    Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                    break;
                case NetworkEvent::ModemErrorTimeout:
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                    break;
            }
        });
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }

            if (clock_ticks_ % 30 == 0 && xiaoclaw_ws_client_ && xiaoclaw_ws_client_->IsConnected()) {
                xiaoclaw_ws_client_->SendPingJson();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");

    /* Start SNTP time sync — required for HTTPS certificate validation.
       System time is 1970-01-01 after cold boot; without sync, TLS handshake
       to DeepSeek/LLM APIs fails because all certificates appear expired. */
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);
    ESP_LOGI(TAG, "SNTP time sync started");

    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 6, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });

    InitializeXiaoClawWsClient();
}

void Application::InitializeXiaoClawWsClient() {
    xiaoclaw_ws_client_ = std::make_unique<XiaoClawWsClient>();

    xiaoclaw_ws_client_->OnStateChange([this](const char* state) {
        ESP_LOGI(TAG, "XiaoClaw server state=%s", state);
        Display* display = Board::GetInstance().GetDisplay();
        display->SetStatus(state);

        if (strcmp(state, "idle") == 0) {
            SetDeviceState(kDeviceStateIdle);
        } else if (strcmp(state, "wakeup_detected") == 0) {
            {
                auto current = GetDeviceState();
                if (current == kDeviceStateIdle || current == kDeviceStateConnecting) {
                    SetDeviceState(kDeviceStateWakeupDetected);
                } else {
                    ESP_LOGI(TAG, "Ignore regressive remote state=wakeup_detected, current=%s",
                             DeviceStateMachine::GetStateName(current));
                }
            }
        } else if (strcmp(state, "listening") == 0) {
            SetDeviceState(kDeviceStateListening);
        } else if (strcmp(state, "uploading_audio") == 0) {
            SetDeviceState(kDeviceStateUploadingAudio);
        } else if (strcmp(state, "recognizing") == 0) {
            SetDeviceState(kDeviceStateRecognizing);
        } else if (strcmp(state, "thinking") == 0) {
            SetDeviceState(kDeviceStateThinking);
        } else if (strcmp(state, "synthesizing") == 0) {
            SetDeviceState(kDeviceStateSynthesizing);
        } else if (strcmp(state, "speaking") == 0) {
            SetDeviceState(kDeviceStateSpeaking);
        } else if (strcmp(state, "error") == 0) {
            SetDeviceState(kDeviceStateError);
        } else if (strcmp(state, "reconnecting") == 0) {
            SetDeviceState(kDeviceStateReconnecting);
        }
    });

    xiaoclaw_ws_client_->OnConnected([this]() {
        ESP_LOGI(TAG, "XiaoClaw WS connected");
        xiaoclaw_reconnect_delay_ms_ = 2000;
        if (xiaoclaw_reconnect_timer_ != nullptr) {
            esp_timer_stop(xiaoclaw_reconnect_timer_);
        }

        Display* display = Board::GetInstance().GetDisplay();
        display->SetStatus("idle");

        auto state = GetDeviceState();
        if (state == kDeviceStateReconnecting || state == kDeviceStateError) {
            SetDeviceState(kDeviceStateIdle);
        }
    });

    xiaoclaw_ws_client_->OnDisconnected([this]() {
        if (xiaoclaw_reconnect_in_progress_) {
            return;
        }

        auto state = GetDeviceState();
        int64_t uptime_ms = esp_timer_get_time() / 1000;
        ESP_LOGW(TAG,
                 "XiaoClaw WS disconnected uptime_ms=%lld state_before=%s",
                 uptime_ms,
                 DeviceStateMachine::GetStateName(state));

        if (xiaoclaw_ws_client_) {
            xiaoclaw_ws_client_->SetUploadingEnabled(false);
        }
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        SetXiaoClawBootListening(false);

        Display* display = Board::GetInstance().GetDisplay();
        display->SetStatus("reconnecting");
        SetDeviceState(kDeviceStateReconnecting);
        StartXiaoClawReconnectTimer();
    });

    xiaoclaw_ws_client_->OnTtsStart([this]() {
        // Reset decoder and queues before the first TTS binary frame arrives.
        // Doing this in kDeviceStateSpeaking races with the first frame and can
        // drop the opening audio of short replies.
        audio_service_.ResetDecoder();
        xiaoclaw_ws_client_->ResetTtsStats();
        SetDeviceState(kDeviceStateSynthesizing);
    });

    xiaoclaw_ws_client_->OnTtsEnd([this]() {
        auto s = GetDeviceState();
        if (s == kDeviceStateSpeaking || s == kDeviceStateSynthesizing) {
            ESP_LOGI(TAG, "XiaoClaw tts_end -> idle");
            SetDeviceState(kDeviceStateIdle);
        }
    });

    xiaoclaw_ws_client_->OnError([this]() {
        ESP_LOGW(TAG, "XiaoClaw server error -> error");
        Display* display = Board::GetInstance().GetDisplay();
        display->SetStatus("error");
        SetDeviceState(kDeviceStateError);
    });

    xiaoclaw_ws_client_->SetOpusFrameCallback([this](const uint8_t* data, size_t len) {
        this->OnOpusFrameFromAudio(data, len);
    });

    xiaoclaw_ws_client_->SetTtsBinaryCallback([this](const uint8_t* data, size_t len) {
        this->OnTtsBinaryFrame(data, len);
    });

    if (!xiaoclaw_ws_client_->Start()) {
        ESP_LOGE(TAG, "XiaoClaw WS start failed");
        Display* display = Board::GetInstance().GetDisplay();
        display->SetStatus("reconnecting");
        SetDeviceState(kDeviceStateReconnecting);
        xiaoclaw_ws_client_.reset();
        xiaoclaw_reconnect_delay_ms_ = std::min(xiaoclaw_reconnect_delay_ms_ * 2, 30000);
        StartXiaoClawReconnectTimer();
    }
}

void Application::StartXiaoClawReconnectTimer() {
    if (xiaoclaw_reconnect_timer_ == nullptr) {
        return;
    }

    esp_timer_stop(xiaoclaw_reconnect_timer_);
    ESP_LOGW(TAG, "XiaoClaw reconnect scheduled in %d ms", xiaoclaw_reconnect_delay_ms_);
    esp_timer_start_once(xiaoclaw_reconnect_timer_, xiaoclaw_reconnect_delay_ms_ * 1000);
}

void Application::TryXiaoClawReconnect() {
    if (!xiaoclaw_ws_client_) {
        InitializeXiaoClawWsClient();
        return;
    }

    if (xiaoclaw_ws_client_->IsConnected()) {
        xiaoclaw_reconnect_delay_ms_ = 2000;
        return;
    }

    ESP_LOGW(TAG, "Trying XiaoClaw WS reconnect");

    xiaoclaw_reconnect_in_progress_ = true;
    xiaoclaw_ws_client_->Stop();
    xiaoclaw_reconnect_in_progress_ = false;

    bool ok = xiaoclaw_ws_client_->Start();
    if (!ok) {
        xiaoclaw_reconnect_delay_ms_ = std::min(xiaoclaw_reconnect_delay_ms_ * 2, 30000);
        StartXiaoClawReconnectTimer();
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();
    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        ESP_LOGI(TAG, "Activation paused for WiFi configuration");
        return;
    }

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
#ifdef CONFIG_XIAOZHI_SKIP_OTA_VERSION_CHECK
    ESP_LOGW(TAG, "OTA version check skipped by config");
    return;
#endif
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateWifiConfiguring) {
                    ESP_LOGI(TAG, "Abort version check for WiFi configuration");
                    return;
                }
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts_response") == 0) {
            ESP_LOGW(TAG, "Deprecated tts_response ignored; XiaoClaw requires JSON control plus binary Opus TTS");
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
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
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else if (strcmp(type->valuestring, "llm") == 0) {
        } else if (strcmp(type->valuestring, "tts") == 0) {
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

bool Application::IsXiaoClawWsReady() const {
    return xiaoclaw_ws_client_ && xiaoclaw_ws_client_->IsConnected();
}

bool Application::XiaoClawSendListenStart(ListeningMode mode) {
    if (IsXiaoClawWsReady()) {
        return xiaoclaw_ws_client_->SendListenStartJson(mode);
    }
    return false;
}

bool Application::XiaoClawSendListenStop() {
    if (IsXiaoClawWsReady()) {
        return xiaoclaw_ws_client_->SendListenStopJson();
    }
    return false;
}

bool Application::XiaoClawSendWakeWordDetected(const std::string& wake_word) {
    if (IsXiaoClawWsReady()) {
        return xiaoclaw_ws_client_->SendWakeWordDetectedJson(wake_word);
    }
    return false;
}

bool Application::XiaoClawBeginListening(ListeningMode mode) {
    if (!XiaoClawSendListenStart(mode)) {
        ESP_LOGE(TAG, "XiaoClaw listen start failed mode=%d", static_cast<int>(mode));
        return false;
    }
    if (!XiaoClawStartAudioUpload()) {
        ESP_LOGE(TAG, "XiaoClaw audio upload start failed mode=%d", static_cast<int>(mode));
        XiaoClawSendListenStop();
        return false;
    }
    return true;
}

bool Application::XiaoClawStartAudioUpload() {
    if (!IsXiaoClawWsReady()) {
        ESP_LOGW(TAG, "XiaoClaw audio upload not started: ws not connected");
        return false;
    }

    xiaoclaw_ws_client_->ResetUploadStats();
    xiaoclaw_ws_client_->SetUploadingEnabled(true);

    audio_service_.EnableWakeWordDetection(false);
    audio_service_.EnableVoiceProcessing(true);

    ESP_LOGI(TAG, "XiaoClaw audio upload started");
    return true;
}

void Application::XiaoClawStopAudioUpload() {
    if (xiaoclaw_ws_client_) {
        xiaoclaw_ws_client_->SetUploadingEnabled(false);
    }

    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);

    ESP_LOGI(TAG, "XiaoClaw audio upload stopped");
}

void Application::XiaoClawAbortCurrentPlayback() {
    DeviceState state = GetDeviceState();
    bool sent = false;
    if (xiaoclaw_ws_client_ && xiaoclaw_ws_client_->IsConnected()) {
        sent = xiaoclaw_ws_client_->SendAbortMessage();
    }
    audio_service_.ClearPlaybackQueues();
    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "BOOT abort playback state=%s sent_abort=%d",
             DeviceStateMachine::GetStateName(state), sent ? 1 : 0);
}

void Application::OnOpusFrameFromAudio(const uint8_t* data, size_t len) {
    static uint32_t opus_cb_total = 0;
    static uint32_t opus_drop_bad_state = 0;
    static uint32_t opus_drop_no_ws = 0;
    static uint32_t opus_drop_upload_disabled = 0;
    ++opus_cb_total;

    auto state = GetDeviceState();
    if (state != kDeviceStateListening && state != kDeviceStateUploadingAudio) {
        ++opus_drop_bad_state;
        if (opus_drop_bad_state == 1 || (opus_drop_bad_state % 50) == 0) {
            ESP_LOGW(TAG,
                     "opus cb drop[%u] bad_state=%s total_cb=%u",
                     static_cast<unsigned>(opus_drop_bad_state),
                     DeviceStateMachine::GetStateName(state),
                     static_cast<unsigned>(opus_cb_total));
        }
        return;
    }
    if (!data || len == 0 || len > 2048) {
        return;
    }
    if (!xiaoclaw_ws_client_ || !xiaoclaw_ws_client_->IsConnected()) {
        ++opus_drop_no_ws;
        if (opus_drop_no_ws == 1 || (opus_drop_no_ws % 50) == 0) {
            ESP_LOGW(TAG,
                     "opus cb drop[%u] no_ws total_cb=%u",
                     static_cast<unsigned>(opus_drop_no_ws),
                     static_cast<unsigned>(opus_cb_total));
        }
        return;
    }
    if (!xiaoclaw_ws_client_->IsUploadingEnabled()) {
        ++opus_drop_upload_disabled;
        if (opus_drop_upload_disabled == 1 || (opus_drop_upload_disabled % 50) == 0) {
            ESP_LOGW(TAG,
                     "opus cb drop[%u] upload_disabled total_cb=%u",
                     static_cast<unsigned>(opus_drop_upload_disabled),
                     static_cast<unsigned>(opus_cb_total));
        }
        return;
    }

    if ((opus_cb_total % 25) == 1) {
        ESP_LOGI(TAG,
                 "opus cb OK count=%u len=%u upload_en=%d state=%s",
                 static_cast<unsigned>(opus_cb_total),
                 static_cast<unsigned>(len),
                 xiaoclaw_ws_client_->IsUploadingEnabled(),
                 DeviceStateMachine::GetStateName(state));
    }

    if (state == kDeviceStateListening) {
        SetDeviceState(kDeviceStateUploadingAudio);
    }

    xiaoclaw_ws_client_->SendOpusFrame(data, len);
}

void Application::OnTtsBinaryFrame(const uint8_t* data, size_t len) {
    static uint32_t tts_drop_bad_state = 0;
    ++tts_drop_bad_state;

    auto state = GetDeviceState();
    if (state != kDeviceStateSynthesizing && state != kDeviceStateSpeaking) {
        if (tts_drop_bad_state == 1 || (tts_drop_bad_state % 25) == 0) {
            ESP_LOGW(TAG,
                     "tts binary drop[%u] bad_state=%s",
                     static_cast<unsigned>(tts_drop_bad_state),
                     DeviceStateMachine::GetStateName(state));
        }
        return;
    }
    if (!data || len == 0 || len > 2048) {
        return;
    }

    if (state == kDeviceStateSynthesizing) {
        SetDeviceState(kDeviceStateSpeaking);
    }

    audio_service_.PlayTtsBinaryFrame(data, len, 24000, 60);
}

void Application::StartRecognizingTimeout() {
    if (xiaoclaw_recognizing_timeout_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(xiaoclaw_recognizing_timeout_timer_);
    esp_timer_start_once(xiaoclaw_recognizing_timeout_timer_, 10000000);
    ESP_LOGI(TAG, "recognizing timeout started (10s)");
}

void Application::CancelRecognizingTimeout() {
    if (xiaoclaw_recognizing_timeout_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(xiaoclaw_recognizing_timeout_timer_);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (IsXiaoClawWsReady()) {
            SetListeningMode(mode);
            return;
        }
        if (!protocol_) {
            ESP_LOGE(TAG, "Protocol not initialized");
            return;
        }
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        // Send abort to stop server-side TTS without ending dialogue session
        if (xiaoclaw_ws_client_ && xiaoclaw_ws_client_->IsConnected()) {
            xiaoclaw_ws_client_->SendAbortMessage();
        } else {
            AbortSpeaking(kAbortReasonNone);
        }
        // Clear playback queues to stop TTS immediately
        audio_service_.ClearPlaybackQueues();
        SetDeviceState(kDeviceStateIdle);
    } else if (state == kDeviceStateListening) {
        if (IsXiaoClawWsReady()) {
            XiaoClawStopAudioUpload();
            XiaoClawSendListenStop();
            SetDeviceState(kDeviceStateRecognizing);
            return;
        }
        if (protocol_) {
            protocol_->CloseAudioChannel();
        }
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (IsXiaoClawWsReady()) {
        SetListeningMode(mode);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (state == kDeviceStateIdle) {
        if (IsXiaoClawWsReady()) {
            SetListeningMode(kListeningModeManualStop);
            return;
        }
        if (!protocol_) {
            ESP_LOGE(TAG, "Protocol not initialized");
            return;
        }
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (IsXiaoClawWsReady()) {
            XiaoClawStopAudioUpload();
            XiaoClawSendListenStop();
            SetDeviceState(kDeviceStateRecognizing);
            return;
        }
        if (protocol_) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    }
}

void Application::HandleWakeWordDetectedEvent() {
    auto state = GetDeviceState();

    // Cooldown: ignore if we detected a wake word in the last 2 seconds
    // Prevents double-detection from AFE residual buffer.
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_wake_word_time_ms_ < 2000) {
        ESP_LOGI(TAG, "Wake word ignored (cooldown, %lld ms since last)",
                 (long long)(now_ms - last_wake_word_time_ms_));
        return;
    }
    last_wake_word_time_ms_ = now_ms;

    if (!protocol_ && !IsXiaoClawWsReady()) {
        return;
    }

    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: '%s' (state=%s)",
             wake_word.c_str(), DeviceStateMachine::GetStateName(state));

    if (state == kDeviceStateIdle) {
        auto wake_word = audio_service_.GetLastWakeWord();
        if (!IsXiaoClawWsReady()) {
            audio_service_.EncodeWakeWord();
        }

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());
        // Clear playback queues to stop TTS immediately
        audio_service_.ClearPlaybackQueues();

        if (state == kDeviceStateListening) {
            if (IsXiaoClawWsReady()) {
                if (!XiaoClawBeginListening(GetDefaultListeningMode())) {
                    SetDeviceState(kDeviceStateError);
                    return;
                }
            } else {
                protocol_->SendStartListening(GetDefaultListeningMode());
            }
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Stop speaking and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    auto state = GetDeviceState();

    // 允许从 idle / connecting / wakeup_detected 继续
    if (state != kDeviceStateConnecting &&
        state != kDeviceStateIdle &&
        state != kDeviceStateWakeupDetected) {
        return;
    }

    if (IsXiaoClawWsReady()) {
        if (GetDeviceState() == kDeviceStateIdle ||
            GetDeviceState() == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateWakeupDetected);
        }

        ESP_LOGI(TAG, "Wake word detected (XiaoClaw WS): %s", wake_word.c_str());
        // For wake-then-speak UX, do not feed the wake word itself into the
        // next ASR round; wait for the follow-up question after the prompt.
        audio_service_.DiscardPreWakeAudioOnNextFlush();

#if CONFIG_SEND_WAKE_WORD_DATA
        if (!XiaoClawSendWakeWordDetected(wake_word)) {
            audio_service_.EnableWakeWordDetection(true);
            SetDeviceState(kDeviceStateIdle);
            return;
        }
#endif

        play_popup_on_listening_ = true;
        SetListeningMode(GetDefaultListeningMode());
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    // 本地先明确进入 wakeup_detected，保证状态机和 TFT 口径一致
    if (GetDeviceState() == kDeviceStateIdle ||
        GetDeviceState() == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateWakeupDetected);
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());

#if CONFIG_SEND_WAKE_WORD_DATA
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    protocol_->SendWakeWordDetected(wake_word);
#endif

    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateWakeupDetected:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            display->SetStatus("wakeup");
            display->SetEmotion("neutral");
            break;
        case kDeviceStateConnecting:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            if (xiaoclaw_boot_listening_) {
                audio_service_.EnableWakeWordDetection(false);
                if (!audio_service_.IsAudioProcessorRunning()) {
                    audio_service_.EnableVoiceProcessing(true);
                }
                break;
            }

            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                audio_service_.WaitForPlaybackQueueEmpty();
                if (IsXiaoClawWsReady()) {
                    if (!XiaoClawBeginListening(listening_mode_)) {
                        SetDeviceState(kDeviceStateError);
                        break;
                    }
                } else if (protocol_) {
                    protocol_->SendStartListening(listening_mode_);
                    audio_service_.EnableVoiceProcessing(true);
                }
            }

            audio_service_.EnableWakeWordDetection(false);

            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateThinking:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            display->SetStatus(Lang::Strings::THINKING);
            display->SetEmotion("microchip_ai");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateSpeaking:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            break;
        case kDeviceStateError:
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            display->SetStatus("error");
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_ && !IsXiaoClawWsReady()) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (IsXiaoClawWsReady()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonWakeWordDetected);
            // Clear send queue to avoid sending residues to server
            while (audio_service_.PopPacketFromSendQueue());
            // Clear playback queues to stop TTS immediately
            audio_service_.ClearPlaybackQueues();
            // Stop speaking and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (IsXiaoClawWsReady()) {
                XiaoClawStopAudioUpload();
                XiaoClawSendListenStop();
                SetDeviceState(kDeviceStateRecognizing);
            } else if (protocol_) {
                protocol_->CloseAudioChannel();
                SetDeviceState(kDeviceStateIdle);
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
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
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
