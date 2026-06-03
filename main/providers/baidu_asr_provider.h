#pragma once

#include "baidu_asr_logic.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct BaiduAsrResult {
    baidu_asr_error_t error = BAIDU_ASR_ERROR_NONE;
    int websocket_status = 0;
    int audio_bytes = 0;
    std::string text;
};

class BaiduAsrProvider {
public:
    using ResultCallback = std::function<void(const BaiduAsrResult&)>;

    BaiduAsrProvider() = default;
    ~BaiduAsrProvider();

    static bool IsConfigured();

    bool Start(ResultCallback callback, int dev_pid = BAIDU_ASR_DEV_PID);
    void FeedPcm(const int16_t* samples, size_t sample_count);
    void Finish();
    void Abort();
    bool IsRunning() const { return running_.load(); }

private:
    static constexpr uint32_t kTaskStackBytes = 12 * 1024;

    struct PcmFrame {
        size_t samples = 0;
        int16_t data[BAIDU_ASR_FRAME_BYTES / sizeof(int16_t)];
    };

    static void TaskEntry(void* arg);
    void TaskMain();
    void CleanupQueue();

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    void* task_stack_ = nullptr;
    StaticTask_t task_tcb_ = {};
    ResultCallback callback_;
    std::string app_id_;
    std::string app_key_;
    std::string cuid_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finish_requested_{false};
    std::atomic<bool> abort_requested_{false};
    int dev_pid_ = BAIDU_ASR_DEV_PID;
};
