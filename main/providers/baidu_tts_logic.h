#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAIDU_TTS_TOKEN_MAX_LEN 192
#define BAIDU_TTS_AUDIO_MAX_BYTES (192 * 1024)

typedef enum {
    BAIDU_TTS_ERROR_NONE = 0,
    BAIDU_TTS_ERROR_NO_WIFI,
    BAIDU_TTS_ERROR_TIME_UNSYNCED,
    BAIDU_TTS_ERROR_CONFIG,
    BAIDU_TTS_ERROR_AUTH,
    BAIDU_TTS_ERROR_TIMEOUT,
    BAIDU_TTS_ERROR_HTTP_STATUS,
    BAIDU_TTS_ERROR_BAD_RESPONSE,
    BAIDU_TTS_ERROR_AUDIO,
    BAIDU_TTS_ERROR_PLAYBACK,
    BAIDU_TTS_ERROR_TRANSPORT,
} baidu_tts_error_t;

int baidu_tts_url_encode(char *dst, size_t dst_size, const char *src);
int baidu_tts_extract_access_token(const char *response_json, char *dst, size_t dst_size);
bool baidu_tts_content_is_pcm(const char *content_type);
const char *baidu_tts_error_label(baidu_tts_error_t error);

#ifdef __cplusplus
}
#endif
