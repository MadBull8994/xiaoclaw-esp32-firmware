#pragma once

#include <functional>
#include <string>
#include <vector>

#include "baidu_tts_logic.h"

struct BaiduTtsResult {
    baidu_tts_error_t error = BAIDU_TTS_ERROR_NONE;
    int http_status = 0;
    size_t audio_bytes = 0;
    std::string content_type;
};

using TtsChunkCallback = std::function<bool(std::vector<int16_t>&& pcm_chunk)>;

class BaiduTtsProvider {
public:
    static bool IsConfigured();
    static BaiduTtsResult SynthesizePcm(const std::string& text, std::vector<int16_t>& pcm);
    static BaiduTtsResult SynthesizeStream(const std::string& text, TtsChunkCallback on_chunk);
};
