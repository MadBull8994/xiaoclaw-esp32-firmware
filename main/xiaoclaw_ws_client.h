#ifndef _XIAOCLAW_WS_CLIENT_H_
#define _XIAOCLAW_WS_CLIENT_H_

#include <string>
#include <cJSON.h>
#include <functional>
#include <memory>

#include "protocol.h"

class WebSocket;

class XiaoClawWsClient {
public:
    using JsonCallback = std::function<void(const cJSON* root)>;
    using StateChangeCallback = std::function<void(const char* state)>;
    using OpusFrameCallback = std::function<void(const uint8_t* data, size_t len)>;

    XiaoClawWsClient();
    ~XiaoClawWsClient();

    bool Start();
    void Stop();
    bool IsConnected() const;
    bool SendJson(cJSON* root);
    void SendHelloJson();
    bool SendListenStartJson(ListeningMode mode = kListeningModeManualStop);
    bool SendListenStopJson();
    bool SendWakeWordDetectedJson(const std::string& wake_word);
    bool SendPingJson();
    bool SendAbortMessage();

    void OnIncomingJson(JsonCallback cb) { on_incoming_json_ = std::move(cb); }
    void OnStateChange(StateChangeCallback cb) { on_state_change_ = std::move(cb); }
    void OnTtsStart(std::function<void()> cb) { on_tts_start_ = std::move(cb); }
    void OnTtsEnd(std::function<void()> cb) { on_tts_end_ = std::move(cb); }
    void OnConnected(std::function<void()> cb) { on_connected_ = std::move(cb); }
    void OnDisconnected(std::function<void()> cb) { on_disconnected_ = std::move(cb); }
    void OnError(std::function<void()> cb) { on_error_ = std::move(cb); }

    void SetOpusFrameCallback(OpusFrameCallback cb) { opus_frame_callback_ = std::move(cb); }
    bool SendOpusFrame(const uint8_t* data, size_t len);
    void ResetUploadStats();
    void ResetTtsStats();
    void SetUploadingEnabled(bool enabled) { uploading_enabled_ = enabled; }
    bool IsUploadingEnabled() const { return uploading_enabled_; }

    using TtsBinaryCallback = std::function<void(const uint8_t* data, size_t len)>;
    void SetTtsBinaryCallback(TtsBinaryCallback cb) { tts_binary_callback_ = std::move(cb); }

    static const char* MaskSecret(const std::string& value);

private:
    void HandleWsData(const char* data, size_t len, bool binary);
    void HandleJsonMessage(const char* data, size_t len);
    void HandleStateMessage(const cJSON* root);
    void HandleSttMessage(const cJSON* root);
    void HandleErrorMessage(const cJSON* root);

    std::unique_ptr<WebSocket> websocket_;
    bool connected_;

    JsonCallback on_incoming_json_;
    StateChangeCallback on_state_change_;
    std::function<void()> on_tts_start_;
    std::function<void()> on_tts_end_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void()> on_error_;

    OpusFrameCallback opus_frame_callback_;
    TtsBinaryCallback tts_binary_callback_;
    uint32_t uploaded_frame_count_ = 0;
    uint32_t uploaded_bytes_ = 0;
    uint32_t tts_binary_frame_count_ = 0;
    uint32_t tts_binary_bytes_ = 0;
    bool uploading_enabled_ = false;
    std::string device_id_;
    std::string client_id_;
};

#endif
