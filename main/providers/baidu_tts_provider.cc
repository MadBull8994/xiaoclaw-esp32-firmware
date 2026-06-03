#include "baidu_tts_provider.h"

#include "openclaw_runtime_config.h"

#include <cstring>
#include <string>
#include <vector>

#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef CONFIG_OPENCLAW_BAIDU_TTS_API_KEY
#define CONFIG_OPENCLAW_BAIDU_TTS_API_KEY ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_TTS_SECRET_KEY
#define CONFIG_OPENCLAW_BAIDU_TTS_SECRET_KEY ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_TTS_CUID
#define CONFIG_OPENCLAW_BAIDU_TTS_CUID "openclaw-esp32s3"
#endif

namespace {

constexpr const char* TAG = "BaiduTtsProvider";
constexpr const char* kTokenUrl = "https://aip.baidubce.com/oauth/2.0/token";
constexpr const char* kAudioUrl = "https://tsn.baidu.com/text2audio";

struct TextCapture {
    char* buf = nullptr;
    size_t size = 0;
    size_t used = 0;
};

struct AudioCapture {
    uint8_t* buf = nullptr;
    size_t size = 0;
    size_t used = 0;
    bool overflow = false;
    char content_type[96] = {0};
};

esp_err_t TextEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    auto* capture = static_cast<TextCapture*>(evt->user_data);
    if (!capture->buf || capture->size == 0 || capture->used + 1 >= capture->size) {
        return ESP_OK;
    }

    size_t room = capture->size - capture->used - 1;
    size_t copy_len = static_cast<size_t>(evt->data_len) < room ? static_cast<size_t>(evt->data_len) : room;
    std::memcpy(capture->buf + capture->used, evt->data, copy_len);
    capture->used += copy_len;
    capture->buf[capture->used] = '\0';
    return ESP_OK;
}

esp_err_t AudioEventHandler(esp_http_client_event_t* evt) {
    if (!evt->user_data) {
        return ESP_OK;
    }

    auto* capture = static_cast<AudioCapture*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy(capture->content_type, evt->header_value, sizeof(capture->content_type));
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t remaining = capture->size - capture->used;
        size_t copy_len = static_cast<size_t>(evt->data_len) < remaining
                          ? static_cast<size_t>(evt->data_len) : remaining;
        if (copy_len > 0) {
            std::memcpy(capture->buf + capture->used, evt->data, copy_len);
            capture->used += copy_len;
        }
        if (static_cast<size_t>(evt->data_len) > remaining) {
            capture->overflow = true;
        }
    }
    return ESP_OK;
}

std::string UrlEncode(const std::string& value) {
    std::string encoded;
    encoded.resize(value.size() * 3 + 1);
    int len = baidu_tts_url_encode(encoded.data(), encoded.size(), value.c_str());
    if (len < 0) {
        return {};
    }
    encoded.resize(len);
    return encoded;
}

baidu_tts_error_t TransportError(esp_err_t ret, int sock_errno, int elapsed_ms, int timeout_ms) {
    if (ret == ESP_OK && sock_errno == 0) {
        return BAIDU_TTS_ERROR_NONE;
    }
    if (ret == ESP_ERR_HTTP_CONNECT || ret == ESP_ERR_HTTP_FETCH_HEADER ||
        ret == ESP_ERR_HTTP_INVALID_TRANSPORT || ret == ESP_ERR_HTTP_CONNECTING) {
        return BAIDU_TTS_ERROR_TRANSPORT;
    }
    if (elapsed_ms >= timeout_ms || sock_errno == ETIMEDOUT) {
        return BAIDU_TTS_ERROR_TIMEOUT;
    }
    return BAIDU_TTS_ERROR_TRANSPORT;
}

baidu_tts_error_t FetchToken(char* token, size_t token_size, int* http_status) {
    if (token && token_size > 0) {
        token[0] = '\0';
    }
    if (http_status) {
        *http_status = 0;
    }
    auto config = OpenClawRuntimeConfig::GetBaiduTtsConfig();
    if (!token || token_size == 0 || config.api_key.empty() || config.secret_key.empty()) {
        return BAIDU_TTS_ERROR_CONFIG;
    }

    std::string encoded_key = UrlEncode(config.api_key);
    std::string encoded_secret = UrlEncode(config.secret_key);
    if (encoded_key.empty() || encoded_secret.empty()) {
        return BAIDU_TTS_ERROR_CONFIG;
    }

    std::string url = std::string(kTokenUrl) +
        "?grant_type=client_credentials&client_id=" + encoded_key +
        "&client_secret=" + encoded_secret;

    char response_body[768] = {0};
    TextCapture response = {
        .buf = response_body,
        .size = sizeof(response_body),
    };

    constexpr int timeout_ms = 10000;
    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.method = HTTP_METHOD_GET;
    http_config.timeout_ms = timeout_ms;
    http_config.event_handler = TextEventHandler;
    http_config.user_data = &response;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        return BAIDU_TTS_ERROR_TRANSPORT;
    }

    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = esp_http_client_perform(client);
    int elapsed_ms = static_cast<int>((esp_timer_get_time() - started_us) / 1000);
    int status = esp_http_client_get_status_code(client);
    int sock_errno = esp_http_client_get_errno(client);
    if (http_status) {
        *http_status = status;
    }

    baidu_tts_error_t error = TransportError(ret, sock_errno, elapsed_ms, timeout_ms);
    if (error == BAIDU_TTS_ERROR_NONE) {
        if (status == 401 || status == 403) {
            error = BAIDU_TTS_ERROR_AUTH;
        } else if (status < 200 || status >= 300) {
            error = BAIDU_TTS_ERROR_HTTP_STATUS;
        } else if (baidu_tts_extract_access_token(response_body, token, token_size) < 0) {
            error = BAIDU_TTS_ERROR_BAD_RESPONSE;
        }
    }

    if (error == BAIDU_TTS_ERROR_NONE) {
        ESP_LOGI(TAG, "TTS token OK: HTTP %d token=SET", status);
    } else {
        ESP_LOGW(TAG, "TTS token failed: class=%s ret=%s errno=%d elapsed=%dms HTTP=%d body=%.*s",
                 baidu_tts_error_label(error), esp_err_to_name(ret), sock_errno,
                 elapsed_ms, status, 160, response_body);
    }

    esp_http_client_cleanup(client);
    return error;
}

baidu_tts_error_t RequestPcm(const char* token, const std::string& text, std::vector<int16_t>& pcm,
                             size_t* audio_bytes, std::string* content_type, int* http_status) {
    if (audio_bytes) {
        *audio_bytes = 0;
    }
    if (content_type) {
        content_type->clear();
    }
    if (http_status) {
        *http_status = 0;
    }
    if (!token || token[0] == '\0' || text.empty()) {
        return BAIDU_TTS_ERROR_CONFIG;
    }

    std::string encoded_text_once = UrlEncode(text);
    std::string encoded_text = UrlEncode(encoded_text_once);
    std::string encoded_token = UrlEncode(token);
    auto config = OpenClawRuntimeConfig::GetBaiduTtsConfig();
    std::string encoded_cuid = UrlEncode(config.cuid);
    if (encoded_text_once.empty() || encoded_text.empty() || encoded_token.empty() || encoded_cuid.empty()) {
        return BAIDU_TTS_ERROR_CONFIG;
    }

    std::string request_body = "tex=" + encoded_text +
        "&tok=" + encoded_token +
        "&cuid=" + encoded_cuid +
        "&ctp=1&lan=zh&spd=5&pit=5&vol=8&per=0&aue=4";

    std::vector<uint8_t> audio(BAIDU_TTS_AUDIO_MAX_BYTES);
    AudioCapture capture = {
        .buf = audio.data(),
        .size = audio.size(),
    };

    constexpr int timeout_ms = 60000;
    esp_http_client_config_t http_config = {};
    http_config.url = kAudioUrl;
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = timeout_ms;
    http_config.event_handler = AudioEventHandler;
    http_config.user_data = &capture;
    http_config.buffer_size = 8192;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        return BAIDU_TTS_ERROR_TRANSPORT;
    }

    esp_http_client_set_header(client, "Accept", "audio/basic,application/json");
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = esp_http_client_open(client, request_body.size());
    if (ret == ESP_OK) {
        int written = esp_http_client_write(client, request_body.data(), request_body.size());
        if (written < 0 || written != static_cast<int>(request_body.size())) {
            ret = ESP_FAIL;
        }
    }

    if (ret == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        ret = ESP_FAIL;
    }

    if (ret == ESP_OK) {
        uint8_t read_buf[1024];
        while (true) {
            int read_len = esp_http_client_read(client, reinterpret_cast<char*>(read_buf), sizeof(read_buf));
            if (read_len < 0) {
                if (capture.used == 0) {
                    ret = ESP_FAIL;
                }
                break;
            }
            if (read_len == 0) {
                break;
            }
            size_t data_len = static_cast<size_t>(read_len);
            if (capture.used + data_len > capture.size) {
                capture.overflow = true;
                data_len = capture.size > capture.used ? capture.size - capture.used : 0;
            }
            if (data_len > 0) {
                std::memcpy(capture.buf + capture.used, read_buf, data_len);
                capture.used += data_len;
            }
            if (capture.overflow) {
                break;
            }
        }
    }
    esp_http_client_close(client);

    int elapsed_ms = static_cast<int>((esp_timer_get_time() - started_us) / 1000);
    int status = esp_http_client_get_status_code(client);
    int sock_errno = esp_http_client_get_errno(client);
    if (http_status) {
        *http_status = status;
    }

    baidu_tts_error_t error = TransportError(ret, sock_errno, elapsed_ms, timeout_ms);
    if (error == BAIDU_TTS_ERROR_NONE) {
        if (status == 401 || status == 403) {
            error = BAIDU_TTS_ERROR_AUTH;
        } else if (status < 200 || status >= 300) {
            error = BAIDU_TTS_ERROR_HTTP_STATUS;
        } else if (capture.overflow) {
            error = BAIDU_TTS_ERROR_AUDIO;
        } else if (!baidu_tts_content_is_pcm(capture.content_type) ||
                   strstr(capture.content_type, "rate=16000") == nullptr ||
                   strstr(capture.content_type, "channel=1") == nullptr) {
            error = BAIDU_TTS_ERROR_BAD_RESPONSE;
        } else if (capture.used < sizeof(int16_t) || (capture.used % sizeof(int16_t)) != 0) {
            error = BAIDU_TTS_ERROR_AUDIO;
        }
    }

    if (audio_bytes) {
        *audio_bytes = capture.used;
    }
    if (content_type) {
        *content_type = capture.content_type;
    }

    int content_len = esp_http_client_get_content_length(client);
    if (error == BAIDU_TTS_ERROR_NONE) {
        pcm.resize(capture.used / sizeof(int16_t));
        std::memcpy(pcm.data(), capture.buf, capture.used);
        ESP_LOGI(TAG, "TTS audio OK: HTTP %d cl=%d recv=%u content=%s",
                 status, content_len, static_cast<unsigned>(capture.used), capture.content_type);
    } else {
        bool body_is_text = capture.used > 0 && !baidu_tts_content_is_pcm(capture.content_type);
        ESP_LOGW(TAG, "TTS audio failed: class=%s ret=%s errno=%d elapsed=%dms HTTP=%d cl=%d recv=%u content=%s body=%.*s",
                 baidu_tts_error_label(error), esp_err_to_name(ret), sock_errno,
                 elapsed_ms, status, content_len, static_cast<unsigned>(capture.used), capture.content_type,
                 body_is_text ? 160 : 0, body_is_text ? reinterpret_cast<const char*>(capture.buf) : "");
    }

    esp_http_client_cleanup(client);
    return error;
}

} // namespace

bool BaiduTtsProvider::IsConfigured() {
    return OpenClawRuntimeConfig::IsBaiduTtsConfigured();
}

BaiduTtsResult BaiduTtsProvider::SynthesizePcm(const std::string& text, std::vector<int16_t>& pcm) {
    BaiduTtsResult result;
    pcm.clear();

    ESP_LOGI(TAG, "TTS provider start: provider=baidu format=pcm16-16k key=%s secret=%s text_bytes=%u",
              IsConfigured() ? "SET" : "EMPTY",
              IsConfigured() ? "SET" : "EMPTY",
              static_cast<unsigned>(text.size()));

    char token[BAIDU_TTS_TOKEN_MAX_LEN + 1] = {0};
    result.error = FetchToken(token, sizeof(token), &result.http_status);
    if (result.error != BAIDU_TTS_ERROR_NONE) {
        return result;
    }

    result.error = RequestPcm(token, text, pcm, &result.audio_bytes,
                              &result.content_type, &result.http_status);
    return result;
}

#define WS_TEXT_FIN    ((ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN))
#define WS_BINARY_FIN  ((ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN))

BaiduTtsResult BaiduTtsProvider::SynthesizeStream(const std::string& text,
                                                   TtsChunkCallback on_chunk) {
    BaiduTtsResult result;
    constexpr const char* kHost = "aip.baidubce.com";
    constexpr int kPort = 443;
    constexpr int kConnectTimeoutMs = 10000;
    char ws_path[256] = {};

    ESP_LOGI(TAG, "TTS WS provider start: provider=baidu format=pcm16-16k ws_tok=SET text_bytes=%u",
             static_cast<unsigned>(text.size()));

    char token[BAIDU_TTS_TOKEN_MAX_LEN + 1] = {0};
    result.error = FetchToken(token, sizeof(token), &result.http_status);
    if (result.error != BAIDU_TTS_ERROR_NONE) {
        return result;
    }

    snprintf(ws_path, sizeof(ws_path),
             "/ws/2.0/speech/publiccloudspeech/v1/tts?access_token=%s&per=0",
             token);

    esp_transport_handle_t ssl = esp_transport_ssl_init();
    esp_transport_handle_t ws = ssl ? esp_transport_ws_init(ssl) : nullptr;
    if (!ssl || !ws) {
        ESP_LOGE(TAG, "TTS WS transport init failed");
        result.error = BAIDU_TTS_ERROR_TRANSPORT;
        goto cleanup;
    }
    esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
    esp_transport_ws_set_path(ws, ws_path);

    if (esp_transport_connect(ws, kHost, kPort, kConnectTimeoutMs) < 0) {
        ESP_LOGW(TAG, "TTS WS connect failed: errno=%d", errno);
        result.error = BAIDU_TTS_ERROR_TRANSPORT;
        goto cleanup;
    }
    ESP_LOGI(TAG, "TTS WS connected: status=%d",
             esp_transport_ws_get_upgrade_request_status(ws));

    {
        char start_json[] =
            "{\"type\":\"system.start\",\"payload\":{"
            "\"spd\":5,\"pit\":5,\"vol\":8,\"aue\":3}}";
        int start_len = sizeof(start_json) - 1;
        if (esp_transport_ws_send_raw(ws, WS_TEXT_FIN, start_json, start_len, 5000) !=
            start_len) {
            ESP_LOGW(TAG, "TTS WS system.start send failed");
            result.error = BAIDU_TTS_ERROR_TRANSPORT;
            goto cleanup;
        }
    }

    {
        char buf[256];
        int n = esp_transport_read(ws, buf, sizeof(buf) - 1, 3000);
        if (n <= 0 || esp_transport_ws_get_read_opcode(ws) != WS_TRANSPORT_OPCODES_TEXT) {
            ESP_LOGW(TAG, "TTS WS system.started read failed: n=%d", n);
            result.error = BAIDU_TTS_ERROR_TRANSPORT;
            goto cleanup;
        }
        buf[n] = '\0';
        ESP_LOGI(TAG, "TTS WS system.started: %s", buf);
        if (strstr(buf, "\"code\":-") || strstr(buf, "\"error\"")) {
            ESP_LOGW(TAG, "TTS WS start error: %s", buf);
            result.error = BAIDU_TTS_ERROR_BAD_RESPONSE;
            goto cleanup;
        }
    }

    {
        char text_json[512];
        int text_len = snprintf(text_json, sizeof(text_json),
                                "{\"type\":\"text\",\"payload\":{\"text\":\"%.*s\"}}",
                                static_cast<int>(text.size()), text.c_str());
        if (text_len < 0 || text_len >= static_cast<int>(sizeof(text_json))) {
            result.error = BAIDU_TTS_ERROR_CONFIG;
            goto cleanup;
        }
        if (esp_transport_ws_send_raw(ws, WS_TEXT_FIN, text_json, text_len, 5000) !=
            text_len) {
            ESP_LOGW(TAG, "TTS WS text send failed");
            result.error = BAIDU_TTS_ERROR_TRANSPORT;
            goto cleanup;
        }
        ESP_LOGI(TAG, "TTS WS text JSON: %s", text_json);
        ESP_LOGI(TAG, "TTS WS text sent: %d bytes", static_cast<int>(text.size()));
    }

    {
        char finish_json[] = "{\"type\":\"system.finish\"}";
        int finish_len = sizeof(finish_json) - 1;
        if (esp_transport_ws_send_raw(ws, WS_TEXT_FIN, finish_json, finish_len, 5000) !=
            finish_len) {
            ESP_LOGW(TAG, "TTS WS system.finish send failed");
            result.error = BAIDU_TTS_ERROR_TRANSPORT;
            goto cleanup;
        }
    }

    {
        uint8_t buf[1024];
        int total_bytes = 0;
        int frame_count = 0;
        int audio_frames = 0;
        while (true) {
            int n = esp_transport_read(ws, reinterpret_cast<char*>(buf),
                                       sizeof(buf), 60000);
            if (n < 0) {
                ESP_LOGI(TAG, "TTS WS done: n=%d total=%d frames=%d audio=%d",
                         n, total_bytes, frame_count, audio_frames);
                break;
            }
            if (n == 0) {
                continue;
            }
            total_bytes += n;
            frame_count++;

            ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(ws);
            if (opcode == WS_TRANSPORT_OPCODES_BINARY ||
                opcode == WS_BINARY_FIN) {
                audio_frames++;
                if (n % sizeof(int16_t) == 0 && n > 0) {
                    std::vector<int16_t> pcm(n / sizeof(int16_t));
                    std::memcpy(pcm.data(), buf, n);
                    if (on_chunk && !on_chunk(std::move(pcm))) {
                        ESP_LOGW(TAG, "TTS WS chunk rejected");
                        break;
                    }
                }
            } else if (opcode == WS_TRANSPORT_OPCODES_TEXT ||
                       opcode == WS_TEXT_FIN) {
                char tmp[256];
                size_t copy_n = static_cast<size_t>(n) < sizeof(tmp) - 1
                               ? static_cast<size_t>(n) : sizeof(tmp) - 1;
                std::memcpy(tmp, buf, copy_n);
                tmp[copy_n] = '\0';
                ESP_LOGI(TAG, "TTS WS frame #%d TEXT: %s", frame_count, tmp);

                if (strstr(tmp, "system.finished")) {
                    ESP_LOGI(TAG, "TTS WS system.finished: total=%d audio_frames=%d",
                             total_bytes, audio_frames);
                    break;
                }
                if (strstr(tmp, "system.error") || strstr(tmp, "\"code\":-")) {
                    ESP_LOGW(TAG, "TTS WS server error: %s", tmp);
                    result.error = BAIDU_TTS_ERROR_BAD_RESPONSE;
                    break;
                }
            } else {
                ESP_LOGI(TAG, "TTS WS frame #%d opcode=%d len=%d",
                         frame_count, (int)opcode, n);
            }
        }
        result.audio_bytes = total_bytes;
    }

cleanup:
    if (ws) esp_transport_destroy(ws);
    if (ssl) esp_transport_destroy(ssl);
    return result;
}
