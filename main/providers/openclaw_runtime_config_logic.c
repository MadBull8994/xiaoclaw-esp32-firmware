#include "openclaw_runtime_config_logic.h"

const char* openclaw_config_pick_value(const char* runtime_value, const char* build_value)
{
    if (runtime_value && runtime_value[0] != '\0') {
        return runtime_value;
    }
    if (build_value && build_value[0] != '\0') {
        return build_value;
    }
    return "";
}

openclaw_ws_config_view_t openclaw_config_resolve_ws_config(
    openclaw_ws_config_view_t runtime_config,
    openclaw_ws_config_view_t build_config)
{
    openclaw_ws_config_view_t resolved = {
        .url = openclaw_config_pick_value(runtime_config.url, build_config.url),
        .token = openclaw_config_pick_value(runtime_config.token, build_config.token),
        .device_id = openclaw_config_pick_value(runtime_config.device_id, build_config.device_id),
        .client_id = openclaw_config_pick_value(runtime_config.client_id, build_config.client_id),
    };
    return resolved;
}

bool openclaw_config_pair_is_set(const char* first, const char* second)
{
    return first && first[0] != '\0' && second && second[0] != '\0';
}

bool openclaw_config_should_store_posted_value(const char* posted_value)
{
    return posted_value && posted_value[0] != '\0';
}
