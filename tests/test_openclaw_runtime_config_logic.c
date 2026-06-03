#include "openclaw_runtime_config_logic.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static void test_runtime_value_overrides_build_default(void)
{
    assert(openclaw_config_pick_value("nvs-value", "build-value")[0] == 'n');
    assert(openclaw_config_pick_value("", "build-value")[0] == 'b');
    assert(openclaw_config_pick_value(NULL, "build-value")[0] == 'b');
    assert(openclaw_config_pick_value(NULL, NULL)[0] == '\0');
}

static void test_pair_is_configured_only_when_both_values_exist(void)
{
    assert(openclaw_config_pair_is_set("a", "b") == true);
    assert(openclaw_config_pair_is_set("", "b") == false);
    assert(openclaw_config_pair_is_set("a", "") == false);
    assert(openclaw_config_pair_is_set(NULL, "b") == false);
}

static void test_blank_posted_secret_preserves_existing_value(void)
{
    assert(openclaw_config_should_store_posted_value("new-secret") == true);
    assert(openclaw_config_should_store_posted_value("") == false);
    assert(openclaw_config_should_store_posted_value(NULL) == false);
}

static void test_runtime_ws_config_overrides_build_defaults_per_field(void)
{
    openclaw_ws_config_view_t resolved = openclaw_config_resolve_ws_config(
        (openclaw_ws_config_view_t){
            .url = "ws://runtime.local:8000/xiaozhi/v1/",
            .token = "",
            .device_id = NULL,
            .client_id = "runtime-client",
        },
        (openclaw_ws_config_view_t){
            .url = "ws://build.local:8000/xiaozhi/v1/",
            .token = "build-token",
            .device_id = "build-device",
            .client_id = "build-client",
        });

    assert(strcmp(resolved.url, "ws://runtime.local:8000/xiaozhi/v1/") == 0);
    assert(strcmp(resolved.token, "build-token") == 0);
    assert(strcmp(resolved.device_id, "build-device") == 0);
    assert(strcmp(resolved.client_id, "runtime-client") == 0);
}

int main(void)
{
    test_runtime_value_overrides_build_default();
    test_pair_is_configured_only_when_both_values_exist();
    test_blank_posted_secret_preserves_existing_value();
    test_runtime_ws_config_overrides_build_defaults_per_field();
    return 0;
}
