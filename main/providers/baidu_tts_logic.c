#include "baidu_tts_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char hex_digit(unsigned value)
{
    return (char)(value < 10 ? ('0' + value) : ('A' + value - 10));
}

int baidu_tts_url_encode(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src) {
        return -1;
    }
    dst[0] = '\0';

    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        bool safe = isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (safe) {
            if (out + 2 > dst_size) {
                return -1;
            }
            dst[out++] = (char)*p;
        } else {
            if (out + 4 > dst_size) {
                return -1;
            }
            dst[out++] = '%';
            dst[out++] = hex_digit((*p >> 4) & 0x0F);
            dst[out++] = hex_digit(*p & 0x0F);
        }
    }
    dst[out] = '\0';
    return (int)out;
}

int baidu_tts_extract_access_token(const char *response_json, char *dst, size_t dst_size)
{
    if (!response_json || !dst || dst_size == 0) {
        return -1;
    }
    dst[0] = '\0';

    const char *key = strstr(response_json, "\"access_token\"");
    if (!key) {
        return -1;
    }
    const char *colon = strchr(key, ':');
    if (!colon) {
        return -1;
    }
    const char *p = colon + 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;

    size_t out = 0;
    while (*p && *p != '"' && out + 1 < dst_size) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        dst[out++] = *p++;
    }
    dst[out] = '\0';
    return out > 0 ? (int)out : -1;
}

bool baidu_tts_content_is_pcm(const char *content_type)
{
    if (!content_type) {
        return false;
    }
    return strstr(content_type, "audio/") != NULL &&
           strstr(content_type, "codec=pcm") != NULL;
}

const char *baidu_tts_error_label(baidu_tts_error_t error)
{
    switch (error) {
    case BAIDU_TTS_ERROR_NONE:
        return "NONE";
    case BAIDU_TTS_ERROR_NO_WIFI:
        return "NO WIFI";
    case BAIDU_TTS_ERROR_TIME_UNSYNCED:
        return "NO TIME";
    case BAIDU_TTS_ERROR_CONFIG:
        return "CONFIG";
    case BAIDU_TTS_ERROR_AUTH:
        return "AUTH";
    case BAIDU_TTS_ERROR_TIMEOUT:
        return "TIMEOUT";
    case BAIDU_TTS_ERROR_HTTP_STATUS:
        return "HTTP";
    case BAIDU_TTS_ERROR_BAD_RESPONSE:
        return "RESP";
    case BAIDU_TTS_ERROR_AUDIO:
        return "AUDIO";
    case BAIDU_TTS_ERROR_PLAYBACK:
        return "PLAYBACK";
    case BAIDU_TTS_ERROR_TRANSPORT:
    default:
        return "TRANSPORT";
    }
}
