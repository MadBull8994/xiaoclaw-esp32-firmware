#include "baidu_asr_provider.h"

#include "openclaw_runtime_config.h"
#include "openclaw_runtime_config_logic.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_APP_ID
#define CONFIG_OPENCLAW_BAIDU_ASR_APP_ID ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_APP_KEY
#define CONFIG_OPENCLAW_BAIDU_ASR_APP_KEY ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_CUID
#define CONFIG_OPENCLAW_BAIDU_ASR_CUID "openclaw-esp32s3"
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_DEV_PID
#define CONFIG_OPENCLAW_BAIDU_ASR_DEV_PID BAIDU_ASR_DEV_PID
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_MAX_MS
#define CONFIG_OPENCLAW_BAIDU_ASR_MAX_MS 12000
#endif

namespace {

constexpr const char* TAG = "BaiduAsrProvider";
constexpr const char* kAsrHost = "vop.baidu.com";
constexpr const char* kAsrPath = "/realtime_asr";
constexpr int kAsrPort = 443;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kWriteTimeoutMs = 5000;
constexpr int kFinalTimeoutMs = 12000;
constexpr int kQueueDepth = 10;

#define WS_TEXT_FIN    ((ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN))
#define WS_BINARY_FIN  ((ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN))

baidu_asr_error_t ReadStartResponse(esp_transport_handle_t ws) {
    char buf[384];
    int waited_ms = 0;
    while (waited_ms < 500) {
        int n = esp_transport_read(ws, buf, sizeof(buf) - 1, 250);
        waited_ms += 250;
        if (n == 0) {
            continue;
        }
        if (n < 0) {
            ESP_LOGW(TAG, "ASR start read failed errno=%d", errno);
            return BAIDU_ASR_ERROR_TRANSPORT;
        }

        if (esp_transport_ws_get_read_opcode(ws) != WS_TRANSPORT_OPCODES_TEXT) {
            continue;
        }

        buf[n] = '\0';
        ESP_LOGI(TAG, "ASR start response: %s", buf);
        return baidu_asr_response_error(buf);
    }
    return BAIDU_ASR_ERROR_NONE;
}

baidu_asr_error_t ReadFinalResponse(esp_transport_handle_t ws, std::string& text) {
    char buf[512];
    char final_text[256];
    int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(kFinalTimeoutMs) * 1000;
    text.clear();

    while (esp_timer_get_time() < deadline_us) {
        int n = esp_transport_read(ws, buf, sizeof(buf) - 1, 1000);
        if (n == 0) {
            continue;
        }
        if (n < 0) {
            int waited_ms = kFinalTimeoutMs -
                static_cast<int>((deadline_us - esp_timer_get_time()) / 1000);
            ESP_LOGW(TAG, "ASR final read failed n=%d errno=%d waited=%dms",
                     n, errno, waited_ms);
            return BAIDU_ASR_ERROR_TRANSPORT;
        }
        if (esp_transport_ws_get_read_opcode(ws) != WS_TRANSPORT_OPCODES_TEXT) {
            continue;
        }

        buf[n] = '\0';
        ESP_LOGI(TAG, "ASR response: %s", buf);
        baidu_asr_error_t error = baidu_asr_response_error(buf);
        if (error != BAIDU_ASR_ERROR_NONE) {
            return error;
        }
        if (baidu_asr_is_final_text(buf)) {
            if (baidu_asr_extract_result_text(buf, final_text, sizeof(final_text)) > 0) {
                text = final_text;
                return BAIDU_ASR_ERROR_NONE;
            }
            return BAIDU_ASR_ERROR_SERVICE;
        }
    }

    return BAIDU_ASR_ERROR_TIMEOUT;
}

bool SendAll(esp_transport_handle_t ws, ws_transport_opcodes_t opcode, void* data,
             size_t len, const char* label) {
    int written = esp_transport_ws_send_raw(ws, opcode, static_cast<char*>(data),
                                            len, kWriteTimeoutMs);
    if (written != static_cast<int>(len)) {
        ESP_LOGW(TAG, "ASR %s write failed: requested=%u written=%d errno=%d",
                 label, static_cast<unsigned>(len), written, errno);
        return false;
    }
    return true;
}

} // namespace

BaiduAsrProvider::~BaiduAsrProvider() {
    Abort();
}

bool BaiduAsrProvider::IsConfigured() {
    return OpenClawRuntimeConfig::IsBaiduAsrConfigured();
}

bool BaiduAsrProvider::Start(ResultCallback callback, int dev_pid) {
    if (running_.load()) {
        ESP_LOGW(TAG, "ASR session already running");
        return false;
    }
    auto config = OpenClawRuntimeConfig::GetBaiduAsrConfig();
    if (!openclaw_config_pair_is_set(config.app_id.c_str(), config.app_key.c_str())) {
        ESP_LOGW(TAG, "ASR provider not configured: appid=%s appkey=%s",
                 config.app_id.empty() ? "EMPTY" : "SET",
                 config.app_key.empty() ? "EMPTY" : "SET");
        return false;
    }

    queue_ = xQueueCreate(kQueueDepth, sizeof(PcmFrame*));
    if (!queue_) {
        ESP_LOGE(TAG, "Failed to create ASR PCM queue");
        return false;
    }

    callback_ = std::move(callback);
    dev_pid_ = dev_pid;
    app_id_ = std::move(config.app_id);
    app_key_ = std::move(config.app_key);
    cuid_ = std::move(config.cuid);
    finish_requested_.store(false);
    abort_requested_.store(false);
    running_.store(true);

    if (!task_stack_) {
        task_stack_ = heap_caps_malloc(kTaskStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!task_stack_) {
            ESP_LOGE(TAG, "Failed to allocate ASR task stack from PSRAM");
            running_.store(false);
            vQueueDelete(queue_);
            queue_ = nullptr;
            task_handle_ = nullptr;
            return false;
        }
    }

    task_handle_ = xTaskCreateStaticPinnedToCore(TaskEntry, "baidu_asr",
                                                kTaskStackBytes, this, 9,
                                                static_cast<StackType_t*>(task_stack_),
                                                &task_tcb_, tskNO_AFFINITY);
    if (!task_handle_) {
        running_.store(false);
        vQueueDelete(queue_);
        queue_ = nullptr;
        ESP_LOGE(TAG, "Failed to create ASR task");
        return false;
    }

    ESP_LOGI(TAG, "ASR task created with stack=%u bytes (psram)",
             static_cast<unsigned>(kTaskStackBytes));
    return true;
}

void BaiduAsrProvider::FeedPcm(const int16_t* samples, size_t sample_count) {
    if (!running_.load() || finish_requested_.load() || abort_requested_.load() ||
        !queue_ || !samples || sample_count == 0) {
        return;
    }

    size_t offset = 0;
    while (offset < sample_count) {
        auto* frame = static_cast<PcmFrame*>(
            heap_caps_malloc(sizeof(PcmFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!frame) {
            ESP_LOGW(TAG, "ASR PCM frame allocation failed");
            return;
        }

        frame->samples = std::min(sample_count - offset,
                                  sizeof(frame->data) / sizeof(frame->data[0]));
        std::memcpy(frame->data, samples + offset, frame->samples * sizeof(int16_t));
        offset += frame->samples;

        if (xQueueSend(queue_, &frame, 0) != pdTRUE) {
            PcmFrame* old_frame = nullptr;
            if (xQueueReceive(queue_, &old_frame, 0) == pdTRUE && old_frame) {
                heap_caps_free(old_frame);
            }
            if (xQueueSend(queue_, &frame, 0) != pdTRUE) {
                heap_caps_free(frame);
                ESP_LOGW(TAG, "ASR PCM queue full, dropping frame");
                return;
            }
            ESP_LOGW(TAG, "ASR PCM queue full, dropping oldest frame");
        }
    }
}

void BaiduAsrProvider::Finish() {
    if (!running_.load()) {
        return;
    }
    finish_requested_.store(true);
}

void BaiduAsrProvider::Abort() {
    if (!running_.load()) {
        return;
    }
    abort_requested_.store(true);
    finish_requested_.store(true);
}

void BaiduAsrProvider::TaskEntry(void* arg) {
    static_cast<BaiduAsrProvider*>(arg)->TaskMain();
    vTaskDelete(nullptr);
}

void BaiduAsrProvider::TaskMain() {
    BaiduAsrResult result;
    result.error = BAIDU_ASR_ERROR_NONE;
    char ws_path[128] = {};
    char start_json[320] = {};
    int start_len = 0;

    esp_transport_handle_t ssl = esp_transport_ssl_init();
    esp_transport_handle_t ws = ssl ? esp_transport_ws_init(ssl) : nullptr;
    if (!ssl || !ws) {
        result.error = BAIDU_ASR_ERROR_TRANSPORT;
        goto done;
    }

    esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
    snprintf(ws_path, sizeof(ws_path), "%s?sn=openclaw-%lld",
             kAsrPath, static_cast<long long>(esp_timer_get_time()));
    esp_transport_ws_set_path(ws, ws_path);

    ESP_LOGI(TAG, "ASR provider start: appid=%s appkey=%s dev_pid=%d",
             app_id_.empty() ? "EMPTY" : "SET",
             app_key_.empty() ? "EMPTY" : "SET",
             dev_pid_);

    if (esp_transport_connect(ws, kAsrHost, kAsrPort, kConnectTimeoutMs) < 0) {
        result.websocket_status = esp_transport_ws_get_upgrade_request_status(ws);
        ESP_LOGW(TAG, "ASR connect failed: status=%d errno=%d",
                 result.websocket_status, errno);
        result.error = BAIDU_ASR_ERROR_CONNECT;
        goto done;
    }
    result.websocket_status = esp_transport_ws_get_upgrade_request_status(ws);
    ESP_LOGI(TAG, "ASR websocket connected: status=%d", result.websocket_status);

    start_len = baidu_asr_build_start_frame(start_json, sizeof(start_json),
                                            app_id_.c_str(),
                                            app_key_.c_str(),
                                            dev_pid_,
                                            cuid_.c_str());
    if (start_len < 0) {
        result.error = BAIDU_ASR_ERROR_CONFIG;
        goto done;
    }

    if (!SendAll(ws, WS_TEXT_FIN, start_json, start_len, "start")) {
        result.error = BAIDU_ASR_ERROR_TRANSPORT;
        goto done;
    }

    result.error = ReadStartResponse(ws);
    if (result.error != BAIDU_ASR_ERROR_NONE) {
        goto done;
    }

    {
        std::vector<int16_t> chunk;
        chunk.reserve(BAIDU_ASR_FRAME_BYTES / sizeof(int16_t));
        int64_t started_us = esp_timer_get_time();
        while (!abort_requested_.load()) {
            bool timed_out = (esp_timer_get_time() - started_us) / 1000 > CONFIG_OPENCLAW_BAIDU_ASR_MAX_MS;
            if (timed_out) {
                ESP_LOGW(TAG, "ASR max session timeout, finishing");
                finish_requested_.store(true);
            }

            PcmFrame* frame = nullptr;
            if (xQueueReceive(queue_, &frame, pdMS_TO_TICKS(100)) == pdTRUE && frame) {
                chunk.insert(chunk.end(), frame->data, frame->data + frame->samples);
                result.audio_bytes += frame->samples * sizeof(int16_t);
                heap_caps_free(frame);
            }

            while (chunk.size() >= BAIDU_ASR_FRAME_BYTES / sizeof(int16_t)) {
                if (!SendAll(ws, WS_BINARY_FIN, chunk.data(), BAIDU_ASR_FRAME_BYTES, "audio")) {
                    result.error = BAIDU_ASR_ERROR_TRANSPORT;
                    goto done;
                }
                chunk.erase(chunk.begin(), chunk.begin() + BAIDU_ASR_FRAME_BYTES / sizeof(int16_t));
            }

            if (finish_requested_.load()) {
                CleanupQueue();
                break;
            }
        }

        if (abort_requested_.load()) {
            result.error = BAIDU_ASR_ERROR_TRANSPORT;
            goto done;
        }

        if (!chunk.empty()) {
            size_t bytes = chunk.size() * sizeof(int16_t);
            if (!SendAll(ws, WS_BINARY_FIN, chunk.data(), bytes, "final audio")) {
                result.error = BAIDU_ASR_ERROR_TRANSPORT;
                goto done;
            }
        }
    }

    {
        char finish[32];
        strlcpy(finish, baidu_asr_finish_frame(), sizeof(finish));
        if (!SendAll(ws, WS_TEXT_FIN, finish, strlen(finish), "finish")) {
            result.error = BAIDU_ASR_ERROR_TRANSPORT;
            goto done;
        }
    }

    result.error = ReadFinalResponse(ws, result.text);

done:
    if (ws) {
        esp_transport_destroy(ws);
    } else if (ssl) {
        esp_transport_destroy(ssl);
    }

    CleanupQueue();
    running_.store(false);
    finish_requested_.store(false);
    abort_requested_.store(false);
    task_handle_ = nullptr;

    if (callback_) {
        callback_(result);
    }
}

void BaiduAsrProvider::CleanupQueue() {
    if (!queue_) {
        return;
    }

    PcmFrame* frame = nullptr;
    while (xQueueReceive(queue_, &frame, 0) == pdTRUE) {
        heap_caps_free(frame);
    }
    vQueueDelete(queue_);
    queue_ = nullptr;
}
