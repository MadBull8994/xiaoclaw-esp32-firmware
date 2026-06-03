#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "openclaw_runtime_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <utility>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

// Local NVS key defines (formerly from mimi_config.h / llm_proxy.h)
#define MIMI_NVS_LLM                    "llm_config"
#define MIMI_NVS_KEY_SSID               "ssid"
#define MIMI_NVS_KEY_API_KEY            "api_key"
#define MIMI_NVS_KEY_MODEL              "model"
#define MIMI_NVS_KEY_PROVIDER           "provider"
#define MIMI_NVS_KEY_ANTHROPIC_API_URL  "anthropic_api_url"
#define MIMI_NVS_KEY_OPENAI_API_URL     "openai_api_url"

#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY             ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER      "openai"
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL               "deepseek-chat"
#endif
#ifndef MIMI_SECRET_OPENAI_API_URL
#define MIMI_SECRET_OPENAI_API_URL      "https://api.deepseek.com/v1/chat/completions"
#endif
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;
static constexpr int OPENCLAW_CONFIG_PORT = 8080;

namespace {

std::string HtmlEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            int high = HexValue(value[i + 1]);
            int low = HexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                decoded.push_back(value[i]);
            }
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::string FormValue(const std::string& body, const char* key) {
    size_t pos = 0;
    const std::string needle = std::string(key) + "=";
    while (pos < body.size()) {
        size_t end = body.find('&', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        if (body.compare(pos, needle.size(), needle) == 0) {
            return UrlDecode(body.substr(pos + needle.size(), end - pos - needle.size()));
        }
        pos = end + 1;
    }
    return {};
}

std::string GetLlmSetting(const char* key, const char* fallback) {
    Settings settings(MIMI_NVS_LLM, false);
    return settings.GetString(key, fallback ? fallback : "");
}

bool HasLlmApiKey() {
    Settings settings(MIMI_NVS_LLM, false);
    return !settings.GetString(MIMI_NVS_KEY_API_KEY).empty() || MIMI_SECRET_API_KEY[0] != '\0';
}

std::string BuildOpenClawConfigPage(bool saved) {
    auto asr = OpenClawRuntimeConfig::GetBaiduAsrConfig();
    auto tts = OpenClawRuntimeConfig::GetBaiduTtsConfig();
    auto ws = OpenClawRuntimeConfig::GetXiaoClawWsConfig();
    std::string provider = GetLlmSetting(MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER);
    std::string model = GetLlmSetting(MIMI_NVS_KEY_MODEL, MIMI_SECRET_MODEL);
    std::string openai_url = GetLlmSetting(MIMI_NVS_KEY_OPENAI_API_URL, MIMI_SECRET_OPENAI_API_URL);

    auto& ssid_manager = SsidManager::GetInstance();
    std::string current_ssid;
    if (!ssid_manager.GetSsidList().empty()) {
        current_ssid = ssid_manager.GetSsidList()[0].ssid;
    }

    std::string html;
    html.reserve(8192);
    html += "<!doctype html><html><head><meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<title>OpenClaw API Setup</title><style>";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:24px;max-width:720px}";
    html += "label{display:block;margin:14px 0 6px;font-weight:600}input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}";
    html += "fieldset{border:1px solid #ddd;margin:18px 0;padding:16px}legend{font-weight:700}";
    html += ".ok{color:#087d32}.warn{color:#a15c00}.hint{color:#666;font-size:14px}.btn{margin-top:18px;background:#111;color:white;border:0;padding:12px 16px;font-size:16px}";
    html += "</style></head><body><h1>OpenClaw API Setup</h1>";
    if (saved) {
        html += "<p class=\"ok\">Saved. Blank secret fields preserved previous values.</p>";
    }
    html += "<p class=\"hint\">Secret values are stored on the device and are never shown again.</p>";
    html += "<form method=\"post\" action=\"/save\">";
    html += "<fieldset><legend>WiFi</legend>";
    html += "<p>Status: <b class=\"";
    html += current_ssid.empty() ? "warn\">NOT CONNECTED" : "ok\">CONNECTED";
    html += "</b></p>";
    html += "<label>SSID</label><input name=\"wifi_ssid\" value=\"" + HtmlEscape(current_ssid) + "\" placeholder=\"WiFi network name\">";
    html += "<label>Password</label><input name=\"wifi_password\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current password\">";
    html += "</fieldset>";
    html += "<fieldset><legend>LLM</legend>";
    html += "<p>Status: <b class=\"";
    html += HasLlmApiKey() ? "ok\">SET" : "warn\">EMPTY";
    html += "</b></p>";
    html += "<label>Provider</label><select name=\"llm_provider\">";
    html += "<option value=\"openai\"";
    html += provider == "openai" ? " selected" : "";
    html += ">OpenAI-compatible</option><option value=\"anthropic\"";
    html += provider == "anthropic" ? " selected" : "";
    html += ">Anthropic</option></select>";
    html += "<label>Model</label><input name=\"llm_model\" value=\"" + HtmlEscape(model) + "\">";
    html += "<label>OpenAI-compatible API URL</label><input name=\"llm_openai_url\" value=\"" + HtmlEscape(openai_url) + "\">";
    html += "<label>API Key</label><input name=\"llm_api_key\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current key\">";
    html += "</fieldset>";
    html += "<fieldset><legend>XiaoClaw WebSocket</legend>";
    html += "<p>Status: <b class=\"";
    html += OpenClawRuntimeConfig::IsXiaoClawWsConfigured() ? "ok\">SET" : "warn\">EMPTY";
    html += "</b></p>";
    html += "<p class=\"hint\">Used by the thin-client XiaoClaw backend link. URL should look like ws://server-ip:8000/xiaozhi/v1/.</p>";
    html += "<label>WebSocket URL</label><input name=\"ws_url\" value=\"" + HtmlEscape(ws.url) + "\" placeholder=\"ws://192.168.x.x:8000/xiaozhi/v1/\">";
    html += "<label>Token</label><input name=\"ws_token\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current token\">";
    html += "<label>Device ID</label><input name=\"ws_device_id\" value=\"" + HtmlEscape(ws.device_id) + "\">";
    html += "<label>Client ID</label><input name=\"ws_client_id\" value=\"" + HtmlEscape(ws.client_id) + "\">";
    html += "</fieldset>";
    html += "<fieldset><legend>Baidu realtime ASR</legend>";
    html += "<p>Status: <b class=\"";
    html += OpenClawRuntimeConfig::IsBaiduAsrConfigured() ? "ok\">SET" : "warn\">EMPTY";
    html += "</b></p>";
    html += "<label>AppID</label><input name=\"asr_app_id\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current AppID\">";
    html += "<label>AppKey</label><input name=\"asr_app_key\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current AppKey\">";
    html += "<label>CUID</label><input name=\"asr_cuid\" value=\"" + HtmlEscape(asr.cuid) + "\">";
    html += "</fieldset>";
    html += "<fieldset><legend>Baidu TTS</legend>";
    html += "<p>Status: <b class=\"";
    html += OpenClawRuntimeConfig::IsBaiduTtsConfigured() ? "ok\">SET" : "warn\">EMPTY";
    html += "</b></p>";
    html += "<label>API Key</label><input name=\"tts_api_key\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current key\">";
    html += "<label>Secret Key</label><input name=\"tts_secret_key\" type=\"password\" autocomplete=\"off\" placeholder=\"Leave blank to keep current secret\">";
    html += "<label>CUID</label><input name=\"tts_cuid\" value=\"" + HtmlEscape(tts.cuid) + "\">";
    html += "</fieldset>";
    html += "<button class=\"btn\" type=\"submit\">Save</button></form></body></html>";
    return html;
}

esp_err_t OpenClawConfigGetHandler(httpd_req_t* req) {
    bool saved = false;
    char query[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        saved = strstr(query, "saved=1") != nullptr;
    }
    std::string html = BuildOpenClawConfigPage(saved);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

esp_err_t OpenClawConfigPostHandler(httpd_req_t* req) {
    std::string body;
    body.resize(req->content_len);
    size_t received = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    std::string llm_api_key = FormValue(body, "llm_api_key");
    std::string llm_provider = FormValue(body, "llm_provider");
    std::string llm_model = FormValue(body, "llm_model");
    std::string llm_openai_url = FormValue(body, "llm_openai_url");
    (void)llm_api_key;
    (void)llm_provider;
    (void)llm_model;
    (void)llm_openai_url;

    OpenClawRuntimeConfig::SaveBaiduAsrConfig(FormValue(body, "asr_app_id"),
                                              FormValue(body, "asr_app_key"),
                                              FormValue(body, "asr_cuid"));
    OpenClawRuntimeConfig::SaveBaiduTtsConfig(FormValue(body, "tts_api_key"),
                                              FormValue(body, "tts_secret_key"),
                                              FormValue(body, "tts_cuid"));
    OpenClawRuntimeConfig::SaveXiaoClawWsConfig(FormValue(body, "ws_url"),
                                                FormValue(body, "ws_token"),
                                                FormValue(body, "ws_device_id"),
                                                FormValue(body, "ws_client_id"));

    std::string wifi_ssid = FormValue(body, "wifi_ssid");
    std::string wifi_password = FormValue(body, "wifi_password");
    bool restart_wifi_after_response = false;
    if (!wifi_ssid.empty()) {
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.AddSsid(wifi_ssid, wifi_password);
        ESP_LOGI(TAG, "WiFi credentials saved: ssid=%s", wifi_ssid.c_str());
        restart_wifi_after_response = true;
    }

    ESP_LOGI(TAG, "OpenClaw API config saved: llm=%s ws=%s asr=%s tts=%s",
             HasLlmApiKey() ? "SET" : "EMPTY",
             OpenClawRuntimeConfig::IsXiaoClawWsConfigured() ? "SET" : "EMPTY",
             OpenClawRuntimeConfig::IsBaiduAsrConfigured() ? "SET" : "EMPTY",
             OpenClawRuntimeConfig::IsBaiduTtsConfigured() ? "SET" : "EMPTY");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved=1");
    esp_err_t resp_err = httpd_resp_send(req, nullptr, 0);

    if (resp_err == ESP_OK && restart_wifi_after_response) {
        // Exit config mode on the main task after the HTTP response is flushed.
        // The existing ConfigModeExit -> TryWifiConnect path will start STA mode
        // with the newly saved credentials.
        Application::GetInstance().Schedule([]() {
            ESP_LOGI(TAG, "Apply saved WiFi credentials: exit config mode");
            WifiManager::GetInstance().StopConfigAp();
        });
    }

    return resp_err;
}

} // namespace

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    StopOpenClawConfigServer();
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            StartOpenClawConfigServer();
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            StopOpenClawConfigServer();
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();
    StartOpenClawConfigServer();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();
        hint += "\nOpenClaw API: http://192.168.4.1:";
        hint += std::to_string(OPENCLAW_CONFIG_PORT);

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

void WifiBoard::StartOpenClawConfigServer() {
    if (openclaw_config_server_) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OPENCLAW_CONFIG_PORT;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&openclaw_config_server_, &config);
    if (err != ESP_OK) {
        openclaw_config_server_ = nullptr;
        ESP_LOGW(TAG, "Failed to start OpenClaw API config server: %s", esp_err_to_name(err));
        return;
    }

    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = OpenClawConfigGetHandler;
    httpd_register_uri_handler(openclaw_config_server_, &root_uri);

    httpd_uri_t save_uri = {};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = OpenClawConfigPostHandler;
    httpd_register_uri_handler(openclaw_config_server_, &save_uri);

    ESP_LOGI(TAG, "OpenClaw API config server started on port %d", OPENCLAW_CONFIG_PORT);
}

void WifiBoard::StopOpenClawConfigServer() {
    if (!openclaw_config_server_) {
        return;
    }
    httpd_stop(openclaw_config_server_);
    openclaw_config_server_ = nullptr;
    ESP_LOGI(TAG, "OpenClaw API config server stopped");
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
        state == kDeviceStateIdle || state == kDeviceStateAudioTesting ||
        state == kDeviceStateActivating) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
            vTaskDelay(pdMS_TO_TICKS(1500));

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state cannot enter config mode: %d", state);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
