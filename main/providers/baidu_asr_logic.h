#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAIDU_ASR_APP_ID_MAX_LEN  24
#define BAIDU_ASR_APP_KEY_MAX_LEN 96
#define BAIDU_ASR_CUID_MAX_LEN    48
#define BAIDU_ASR_DEV_PID_BASIC   1537
#define BAIDU_ASR_DEV_PID_PUNCT   15372
#define BAIDU_ASR_DEV_PID         BAIDU_ASR_DEV_PID_PUNCT
#define BAIDU_ASR_FRAME_BYTES     5120

typedef enum {
    BAIDU_ASR_ERROR_NONE = 0,
    BAIDU_ASR_ERROR_NO_WIFI,
    BAIDU_ASR_ERROR_TIME_UNSYNCED,
    BAIDU_ASR_ERROR_CONFIG,
    BAIDU_ASR_ERROR_CONNECT,
    BAIDU_ASR_ERROR_AUTH,
    BAIDU_ASR_ERROR_TIMEOUT,
    BAIDU_ASR_ERROR_SERVICE,
    BAIDU_ASR_ERROR_TRANSPORT,
} baidu_asr_error_t;

int baidu_asr_build_start_frame(char *dst, size_t dst_size,
                                const char *appid, const char *appkey,
                                int dev_pid, const char *cuid);
const char *baidu_asr_finish_frame(void);
baidu_asr_error_t baidu_asr_response_error(const char *response_json);
bool baidu_asr_is_final_text(const char *response_json);
int baidu_asr_extract_result_text(const char *response_json, char *dst, size_t dst_size);
const char *baidu_asr_error_label(baidu_asr_error_t error);

#ifdef __cplusplus
}
#endif
