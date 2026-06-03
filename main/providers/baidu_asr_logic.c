#include "baidu_asr_logic.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

int baidu_asr_build_start_frame(char *dst, size_t dst_size,
                                const char *appid, const char *appkey,
                                int dev_pid, const char *cuid)
{
    if (!dst || dst_size == 0 || !appid || !appkey || !cuid ||
        appid[0] == '\0' || appkey[0] == '\0' || cuid[0] == '\0') {
        return -1;
    }

    int len = snprintf(dst, dst_size,
                       "{\"type\":\"START\",\"data\":{"
                       "\"appid\":%s,"
                       "\"appkey\":\"%s\","
                       "\"dev_pid\":%d,"
                       "\"cuid\":\"%s\","
                       "\"format\":\"pcm\","
                       "\"sample\":16000"
                       "}}",
                       appid, appkey, dev_pid, cuid);
    if (len < 0 || len >= (int)dst_size) {
        return -1;
    }
    return len;
}

const char *baidu_asr_finish_frame(void)
{
    return "{\"type\":\"FINISH\"}";
}

baidu_asr_error_t baidu_asr_response_error(const char *response_json)
{
    if (!response_json || response_json[0] == '\0') {
        return BAIDU_ASR_ERROR_NONE;
    }
    if (strstr(response_json, "\"err_no\":0") ||
        strstr(response_json, "\"err_no\": 0") ||
        strstr(response_json, "\"type\":\"HEARTBEAT\"")) {
        return BAIDU_ASR_ERROR_NONE;
    }
    if (strstr(response_json, "authentication") ||
        strstr(response_json, "Authentication") ||
        strstr(response_json, "\"err_no\":-3004") ||
        strstr(response_json, "\"err_no\": -3004")) {
        return BAIDU_ASR_ERROR_AUTH;
    }
    if (strstr(response_json, "\"err_no\"")) {
        return BAIDU_ASR_ERROR_SERVICE;
    }
    return BAIDU_ASR_ERROR_NONE;
}

bool baidu_asr_is_final_text(const char *response_json)
{
    if (!response_json || response_json[0] == '\0') {
        return false;
    }
    return strstr(response_json, "\"FIN_TEXT\"") != NULL ||
           strstr(response_json, "\"type\":\"FIN_TEXT\"") != NULL ||
           strstr(response_json, "\"type\": \"FIN_TEXT\"") != NULL;
}

int baidu_asr_extract_result_text(const char *response_json, char *dst, size_t dst_size)
{
    if (!response_json || !dst || dst_size == 0) {
        return -1;
    }
    dst[0] = '\0';

    const char *key = strstr(response_json, "\"result\"");
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
            switch (*p) {
            case 'n':
                dst[out++] = '\n';
                break;
            case 'r':
                dst[out++] = '\r';
                break;
            case 't':
                dst[out++] = '\t';
                break;
            case '"':
            case '\\':
            case '/':
                dst[out++] = *p;
                break;
            default:
                dst[out++] = *p;
                break;
            }
            p++;
            continue;
        }
        dst[out++] = *p++;
    }
    dst[out] = '\0';
    return out > 0 ? (int)out : -1;
}

const char *baidu_asr_error_label(baidu_asr_error_t error)
{
    switch (error) {
    case BAIDU_ASR_ERROR_NONE:
        return "NONE";
    case BAIDU_ASR_ERROR_NO_WIFI:
        return "NO WIFI";
    case BAIDU_ASR_ERROR_TIME_UNSYNCED:
        return "NO TIME";
    case BAIDU_ASR_ERROR_CONFIG:
        return "CONFIG";
    case BAIDU_ASR_ERROR_CONNECT:
        return "CONNECT";
    case BAIDU_ASR_ERROR_AUTH:
        return "AUTH";
    case BAIDU_ASR_ERROR_TIMEOUT:
        return "TIMEOUT";
    case BAIDU_ASR_ERROR_SERVICE:
        return "SERVICE";
    case BAIDU_ASR_ERROR_TRANSPORT:
    default:
        return "TRANSPORT";
    }
}
