#pragma once

#include <string>

struct OpenClawAsrConfig {
    std::string app_id;
    std::string app_key;
    std::string cuid;
};

struct OpenClawTtsConfig {
    std::string api_key;
    std::string secret_key;
    std::string cuid;
};

struct OpenClawXiaoClawWsConfig {
    std::string url;
    std::string token;
    std::string device_id;
    std::string client_id;
    bool runtime_url = false;
    bool runtime_token = false;
    bool runtime_device_id = false;
    bool runtime_client_id = false;
};

namespace OpenClawRuntimeConfig {

OpenClawAsrConfig GetBaiduAsrConfig();
OpenClawTtsConfig GetBaiduTtsConfig();
OpenClawXiaoClawWsConfig GetXiaoClawWsConfig();

bool IsBaiduAsrConfigured();
bool IsBaiduTtsConfigured();
bool IsXiaoClawWsConfigured();

void SaveBaiduAsrConfig(const std::string& app_id,
                        const std::string& app_key,
                        const std::string& cuid);
void SaveBaiduTtsConfig(const std::string& api_key,
                        const std::string& secret_key,
                        const std::string& cuid);
void SaveXiaoClawWsConfig(const std::string& url,
                          const std::string& token,
                          const std::string& device_id,
                          const std::string& client_id);

} // namespace OpenClawRuntimeConfig
