#include "xiaoclaw_ws_client.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "openclaw_runtime_config.h"

#include <esp_log.h>
#include <esp_system.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#define TAG "XiaoClawWS"

namespace {

const char* ConfigSource(bool runtime_set, const std::string& resolved_value) {
    if (runtime_set) {
        return "runtime";
    }
    return resolved_value.empty() ? "empty" : "build";
}

const char* ListenModeToString(ListeningMode mode) {
    switch (mode) {
        case kListeningModeRealtime:
            return "realtime";
        case kListeningModeAutoStop:
            return "auto";
        case kListeningModeManualStop:
        default:
            return "manual";
    }
}

} // namespace

XiaoClawWsClient::XiaoClawWsClient() : connected_(false) {
}

XiaoClawWsClient::~XiaoClawWsClient() {
    Stop();
}

const char* XiaoClawWsClient::MaskSecret(const std::string& value) {
    static std::string masked;
    if (value.empty()) {
        masked = "EMPTY";
    } else if (value.size() <= 8) {
        masked = "SET";
    } else {
        masked = value.substr(0, 3) + "***" + value.substr(value.size() - 3);
    }
    return masked.c_str();
}

bool XiaoClawWsClient::Start() {
    auto config = OpenClawRuntimeConfig::GetXiaoClawWsConfig();
    std::string ws_url = config.url;
    std::string device_token = config.token;
    device_id_ = config.device_id;
    client_id_ = config.client_id;

    if (device_token.empty()) {
        char runtime_token[32];
        snprintf(runtime_token, sizeof(runtime_token), "runtime-%08" PRIx32, esp_random());
        device_token = runtime_token;
        ESP_LOGW(TAG, "device_token is EMPTY, using runtime token for local auth-disabled validation");
    }

    ESP_LOGI(TAG,
             "ws config url=%s token=%s device_id=%s client_id=%s url_source=%s token_source=%s device_id_source=%s client_id_source=%s",
             ws_url.c_str(),
             MaskSecret(device_token),
             device_id_.c_str(),
             client_id_.c_str(),
             ConfigSource(config.runtime_url, config.url),
             ConfigSource(config.runtime_token, config.token),
             ConfigSource(config.runtime_device_id, config.device_id),
             ConfigSource(config.runtime_client_id, config.client_id));

    if (ws_url.empty()) {
        ESP_LOGE(TAG, "ws_url is EMPTY");
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = std::make_unique<WebSocket>(network, 1);
    if (!websocket_) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    std::string auth_token = "Bearer " + device_token;
    websocket_->SetHeader("Authorization", auth_token.c_str());
    websocket_->SetHeader("device-id", device_id_.c_str());
    websocket_->SetHeader("client-id", client_id_.c_str());

    websocket_->OnConnected([this]() {
        ESP_LOGI(TAG, "ws connected");
        connected_ = true;
        if (on_connected_) {
            on_connected_();
        }
        SendHelloJson();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "ws disconnected");
        connected_ = false;
        if (on_disconnected_) {
            on_disconnected_();
        }
    });

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        HandleWsData(data, len, binary);
    });

    ESP_LOGI(TAG, "websocket connecting token=%s", MaskSecret(device_token));
    if (!websocket_->Connect(ws_url.c_str())) {
        ESP_LOGE(TAG, "websocket connect failed");
        websocket_.reset();
        return false;
    }

    return true;
}

void XiaoClawWsClient::Stop() {
    if (websocket_) {
        websocket_->Close();
        websocket_.reset();
        connected_ = false;
    }
}

bool XiaoClawWsClient::IsConnected() const {
    return websocket_ && websocket_->IsConnected();
}

bool XiaoClawWsClient::SendJson(cJSON* root) {
    if (!websocket_ || !websocket_->IsConnected()) {
        ESP_LOGW(TAG, "ws not connected, json not sent");
        return false;
    }

    char* text = cJSON_PrintUnformatted(root);
    if (!text) {
        return false;
    }

    size_t len = strlen(text);
    if (len > 4096) {
        ESP_LOGE(TAG, "json too large len=%u", static_cast<unsigned>(len));
        cJSON_free(text);
        return false;
    }

    bool ret = websocket_->Send(text);
    ESP_LOGI(TAG, "json sent len=%u", static_cast<unsigned>(len));

    cJSON_free(text);
    return ret;
}

void XiaoClawWsClient::SendHelloJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(root, "client_id", client_id_.c_str());

    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "codec", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration_ms", 20);
    cJSON_AddItemToObject(root, "audio", audio);

    SendJson(root);
    cJSON_Delete(root);
}

bool XiaoClawWsClient::SendListenStartJson(ListeningMode mode) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    cJSON_AddStringToObject(root, "mode", ListenModeToString(mode));
    bool ok = SendJson(root);
    cJSON_Delete(root);
    return ok;
}

bool XiaoClawWsClient::SendListenStopJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "stop");
    bool ok = SendJson(root);
    cJSON_Delete(root);
    return ok;
}

bool XiaoClawWsClient::SendWakeWordDetectedJson(const std::string& wake_word) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "detect");
    cJSON_AddStringToObject(root, "text", wake_word.c_str());
    bool ok = SendJson(root);
    cJSON_Delete(root);
    return ok;
}

bool XiaoClawWsClient::SendPingJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ping");
    bool ok = SendJson(root);
    cJSON_Delete(root);
    return ok;
}

bool XiaoClawWsClient::SendAbortMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "abort");
    bool ok = SendJson(root);
    cJSON_Delete(root);
    return ok;
}

bool XiaoClawWsClient::SendOpusFrame(const uint8_t* data, size_t len) {
    if (!websocket_ || !websocket_->IsConnected()) {
        ESP_LOGW(TAG, "ws not connected, opus frame dropped");
        return false;
    }

    if (!data || len == 0) {
        return false;
    }

    if (len > 2048) {
        ESP_LOGE(TAG, "opus frame too large len=%u", static_cast<unsigned>(len));
        return false;
    }

    bool ret = websocket_->Send(data, len, true);
    if (!ret) {
        ESP_LOGW(TAG, "send opus frame failed len=%u", static_cast<unsigned>(len));
        return false;
    }

    ++uploaded_frame_count_;
    uploaded_bytes_ += len;

    if (uploaded_frame_count_ == 1) {
        ESP_LOGI(TAG, "FIRST opus frame uploaded len=%u", static_cast<unsigned>(len));
    } else if ((uploaded_frame_count_ % 25) == 0) {
        ESP_LOGI(TAG,
                 "opus upload frames=%u bytes=%u last_len=%u",
                 static_cast<unsigned>(uploaded_frame_count_),
                 static_cast<unsigned>(uploaded_bytes_),
                 static_cast<unsigned>(len));
    }

    return true;
}

void XiaoClawWsClient::ResetUploadStats() {
    uploaded_frame_count_ = 0;
    uploaded_bytes_ = 0;
}

void XiaoClawWsClient::ResetTtsStats() {
    tts_binary_frame_count_ = 0;
    tts_binary_bytes_ = 0;
}

void XiaoClawWsClient::HandleWsData(const char* data, size_t len, bool binary) {
    if (!data || len == 0) {
        return;
    }

    if (binary) {
        if (len > 2048) {
            ESP_LOGW(TAG, "server binary frame too large len=%u", static_cast<unsigned>(len));
            return;
        }

        ++tts_binary_frame_count_;
        tts_binary_bytes_ += len;

        if (tts_binary_frame_count_ == 1) {
            ESP_LOGI(TAG, "FIRST tts binary frame len=%u", static_cast<unsigned>(len));
        } else if ((tts_binary_frame_count_ % 25) == 0) {
            ESP_LOGI(TAG,
                     "tts binary frames=%u bytes=%u last_len=%u",
                     static_cast<unsigned>(tts_binary_frame_count_),
                     static_cast<unsigned>(tts_binary_bytes_),
                     static_cast<unsigned>(len));
        }

        if (tts_binary_callback_) {
            tts_binary_callback_(reinterpret_cast<const uint8_t*>(data), len);
        } else {
            ESP_LOGW(TAG, "binary frame dropped, no tts callback len=%u", static_cast<unsigned>(len));
        }
        return;
    }

    HandleJsonMessage(data, len);
}

void XiaoClawWsClient::HandleJsonMessage(const char* data, size_t len) {
    if (!data || len == 0 || len > 4096) {
        ESP_LOGW(TAG, "invalid json len=%u", static_cast<unsigned>(len));
        return;
    }

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "json parse failed");
        return;
    }

    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const std::string msg_type = type->valuestring;

    if (msg_type == "state") {
        HandleStateMessage(root);
    } else if (msg_type == "stt") {
        HandleSttMessage(root);
    } else if (msg_type == "tts") {
        const cJSON* tts_state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(tts_state)) {
            const char* ts = tts_state->valuestring;
            if (strcmp(ts, "start") == 0) {
                ESP_LOGI(TAG, "tts start received");
                if (on_tts_start_) {
                    on_tts_start_();
                }
            } else if (strcmp(ts, "stop") == 0) {
                ESP_LOGI(TAG, "tts stop received");
                if (on_tts_end_) {
                    on_tts_end_();
                }
            } else if (strcmp(ts, "sentence_start") == 0 ||
                       strcmp(ts, "sentence_delta") == 0 ||
                       strcmp(ts, "sentence_end") == 0) {
                ESP_LOGI(TAG, "tts sentence state=%s", ts);
            } else {
                ESP_LOGW(TAG, "tts state=%s", ts);
            }
        }
    } else if (msg_type == "tts_start") {
        ESP_LOGI(TAG, "legacy tts_start received");
        if (on_tts_start_) {
            on_tts_start_();
        }
    } else if (msg_type == "tts_end") {
        ESP_LOGI(TAG, "legacy tts_end received");
        if (on_tts_end_) {
            on_tts_end_();
        }
    } else if (msg_type == "sentence_start" || msg_type == "sentence_delta" || msg_type == "sentence_end") {
        ESP_LOGI(TAG, "sentence msg type=%s", msg_type.c_str());
    } else if (msg_type == "error") {
        HandleErrorMessage(root);
    } else if (msg_type == "hello") {
        ESP_LOGI(TAG, "server hello received");
    } else if (msg_type == "pong") {
        ESP_LOGD(TAG, "server pong received");
    } else {
        if (on_incoming_json_) {
            on_incoming_json_(root);
        }
    }

    cJSON_Delete(root);
}

void XiaoClawWsClient::HandleStateMessage(const cJSON* root) {
    const cJSON* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(state)) {
        return;
    }

    ESP_LOGI(TAG, "server state=%s", state->valuestring);
    if (on_state_change_) {
        on_state_change_(state->valuestring);
    }
}

void XiaoClawWsClient::HandleSttMessage(const cJSON* root) {
    const cJSON* text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text)) {
        return;
    }

    const size_t len = strlen(text->valuestring);
    ESP_LOGI(TAG, "stt received len=%u", static_cast<unsigned>(len));
}

void XiaoClawWsClient::HandleErrorMessage(const cJSON* root) {
    const cJSON* code = cJSON_GetObjectItem(root, "code");
    const cJSON* message = cJSON_GetObjectItem(root, "message");

    ESP_LOGW(TAG,
             "server error code=%s message=%s",
             cJSON_IsString(code) ? code->valuestring : "UNKNOWN",
             cJSON_IsString(message) ? message->valuestring : "");

    if (on_error_) {
        on_error_();
    }
}
