#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* url;
    const char* token;
    const char* device_id;
    const char* client_id;
} openclaw_ws_config_view_t;

const char* openclaw_config_pick_value(const char* runtime_value, const char* build_value);
openclaw_ws_config_view_t openclaw_config_resolve_ws_config(
    openclaw_ws_config_view_t runtime_config,
    openclaw_ws_config_view_t build_config);
bool openclaw_config_pair_is_set(const char* first, const char* second);
bool openclaw_config_should_store_posted_value(const char* posted_value);

#ifdef __cplusplus
}
#endif
