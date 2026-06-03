#include "openclaw_runtime_config.h"

#include "openclaw_runtime_config_logic.h"
#include "settings.h"

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_APP_ID
#define CONFIG_OPENCLAW_BAIDU_ASR_APP_ID ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_APP_KEY
#define CONFIG_OPENCLAW_BAIDU_ASR_APP_KEY ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_ASR_CUID
#define CONFIG_OPENCLAW_BAIDU_ASR_CUID "openclaw-esp32s3"
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_TTS_API_KEY
#define CONFIG_OPENCLAW_BAIDU_TTS_API_KEY ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_TTS_SECRET_KEY
#define CONFIG_OPENCLAW_BAIDU_TTS_SECRET_KEY ""
#endif

#ifndef CONFIG_OPENCLAW_BAIDU_TTS_CUID
#define CONFIG_OPENCLAW_BAIDU_TTS_CUID "openclaw-esp32s3"
#endif

#ifndef CONFIG_XIAOCLAW_WS_URL
#define CONFIG_XIAOCLAW_WS_URL ""
#endif

#ifndef CONFIG_XIAOCLAW_DEVICE_TOKEN
#define CONFIG_XIAOCLAW_DEVICE_TOKEN ""
#endif

#ifndef CONFIG_XIAOCLAW_DEVICE_ID
#define CONFIG_XIAOCLAW_DEVICE_ID "esp32s3-xiaoclaw"
#endif

#ifndef CONFIG_XIAOCLAW_CLIENT_ID
#define CONFIG_XIAOCLAW_CLIENT_ID "esp32s3-client"
#endif

namespace {

constexpr const char* kNamespace = "openclaw_cfg";
constexpr const char* kAsrAppId = "asr_app_id";
constexpr const char* kAsrAppKey = "asr_app_key";
constexpr const char* kAsrCuid = "asr_cuid";
constexpr const char* kTtsApiKey = "tts_api_key";
constexpr const char* kTtsSecretKey = "tts_secret_key";
constexpr const char* kTtsCuid = "tts_cuid";
constexpr const char* kWebsocketNamespace = "websocket";
constexpr const char* kWsUrl = "url";
constexpr const char* kWsToken = "token";
constexpr const char* kWsDeviceId = "device_id";
constexpr const char* kWsClientId = "client_id";

std::string PickString(const std::string& runtime_value, const char* build_value)
{
    return openclaw_config_pick_value(runtime_value.c_str(), build_value);
}

void StoreIfPosted(Settings& settings, const char* key, const std::string& value)
{
    if (openclaw_config_should_store_posted_value(value.c_str())) {
        settings.SetString(key, value);
    }
}

bool HasPostedValue(const std::string& value)
{
    return openclaw_config_should_store_posted_value(value.c_str());
}

} // namespace

namespace OpenClawRuntimeConfig {

OpenClawAsrConfig GetBaiduAsrConfig()
{
    Settings settings(kNamespace, false);
    OpenClawAsrConfig config;
    config.app_id = PickString(settings.GetString(kAsrAppId), CONFIG_OPENCLAW_BAIDU_ASR_APP_ID);
    config.app_key = PickString(settings.GetString(kAsrAppKey), CONFIG_OPENCLAW_BAIDU_ASR_APP_KEY);
    config.cuid = PickString(settings.GetString(kAsrCuid), CONFIG_OPENCLAW_BAIDU_ASR_CUID);
    return config;
}

OpenClawTtsConfig GetBaiduTtsConfig()
{
    Settings settings(kNamespace, false);
    OpenClawTtsConfig config;
    config.api_key = PickString(settings.GetString(kTtsApiKey), CONFIG_OPENCLAW_BAIDU_TTS_API_KEY);
    config.secret_key = PickString(settings.GetString(kTtsSecretKey), CONFIG_OPENCLAW_BAIDU_TTS_SECRET_KEY);
    config.cuid = PickString(settings.GetString(kTtsCuid), CONFIG_OPENCLAW_BAIDU_TTS_CUID);
    return config;
}

OpenClawXiaoClawWsConfig GetXiaoClawWsConfig()
{
    Settings settings(kWebsocketNamespace, false);
    std::string runtime_url = settings.GetString(kWsUrl);
    std::string runtime_token = settings.GetString(kWsToken);
    std::string runtime_device_id = settings.GetString(kWsDeviceId);
    std::string runtime_client_id = settings.GetString(kWsClientId);
    openclaw_ws_config_view_t resolved = openclaw_config_resolve_ws_config(
        (openclaw_ws_config_view_t){
            .url = runtime_url.c_str(),
            .token = runtime_token.c_str(),
            .device_id = runtime_device_id.c_str(),
            .client_id = runtime_client_id.c_str(),
        },
        (openclaw_ws_config_view_t){
            .url = CONFIG_XIAOCLAW_WS_URL,
            .token = CONFIG_XIAOCLAW_DEVICE_TOKEN,
            .device_id = CONFIG_XIAOCLAW_DEVICE_ID,
            .client_id = CONFIG_XIAOCLAW_CLIENT_ID,
        });

    OpenClawXiaoClawWsConfig config;
    config.url = resolved.url;
    config.token = resolved.token;
    config.device_id = resolved.device_id;
    config.client_id = resolved.client_id;
    config.runtime_url = HasPostedValue(runtime_url);
    config.runtime_token = HasPostedValue(runtime_token);
    config.runtime_device_id = HasPostedValue(runtime_device_id);
    config.runtime_client_id = HasPostedValue(runtime_client_id);
    return config;
}

bool IsBaiduAsrConfigured()
{
    auto config = GetBaiduAsrConfig();
    return openclaw_config_pair_is_set(config.app_id.c_str(), config.app_key.c_str());
}

bool IsBaiduTtsConfigured()
{
    auto config = GetBaiduTtsConfig();
    return openclaw_config_pair_is_set(config.api_key.c_str(), config.secret_key.c_str());
}

bool IsXiaoClawWsConfigured()
{
    auto config = GetXiaoClawWsConfig();
    return !config.url.empty();
}

void SaveBaiduAsrConfig(const std::string& app_id,
                        const std::string& app_key,
                        const std::string& cuid)
{
    Settings settings(kNamespace, true);
    StoreIfPosted(settings, kAsrAppId, app_id);
    StoreIfPosted(settings, kAsrAppKey, app_key);
    StoreIfPosted(settings, kAsrCuid, cuid);
}

void SaveBaiduTtsConfig(const std::string& api_key,
                        const std::string& secret_key,
                        const std::string& cuid)
{
    Settings settings(kNamespace, true);
    StoreIfPosted(settings, kTtsApiKey, api_key);
    StoreIfPosted(settings, kTtsSecretKey, secret_key);
    StoreIfPosted(settings, kTtsCuid, cuid);
}

void SaveXiaoClawWsConfig(const std::string& url,
                          const std::string& token,
                          const std::string& device_id,
                          const std::string& client_id)
{
    Settings settings(kWebsocketNamespace, true);
    StoreIfPosted(settings, kWsUrl, url);
    StoreIfPosted(settings, kWsToken, token);
    StoreIfPosted(settings, kWsDeviceId, device_id);
    StoreIfPosted(settings, kWsClientId, client_id);
}

} // namespace OpenClawRuntimeConfig
