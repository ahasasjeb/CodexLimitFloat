#pragma once

#include <cstdint>
#include <string>

namespace codexlimit
{

constexpr int kHttpTimeoutMs = 3000;
constexpr const wchar_t *kRelayKeyFileName = L"relay_server_key.json";

struct WindowLimit
{
    bool present = false;
    int used_percent = 0;
    int window_seconds = 0;
    long long reset_at = 0;
};

enum class StatusCode : std::uint8_t
{
    Loading,
    Refreshing,
    Updated,
    NoWindow,
    ReadFailed,
    RelayAuthRemoved,
    ThreadFailed,
};

const wchar_t *StatusText(StatusCode code);

struct UsageState
{
    WindowLimit primary;
    WindowLimit secondary;
    std::wstring plan_display;
    StatusCode status = StatusCode::Loading;
    bool ok = false;
};

enum class AuthStore
{
    None,
    Env,
    File,
    Keyring,
};

struct AuthTokens
{
    AuthStore store = AuthStore::None;
    std::wstring id_token;
    std::wstring access_token;
    std::wstring refresh_token;
    std::wstring account_id;
};

struct AppConfig
{
    std::wstring codex_home;
    std::wstring base_url = L"https://chatgpt.com/backend-api/";
    std::wstring credentials_store = L"auto";
};

struct HttpResponse
{
    unsigned long status = 0;
    std::string body;
};

} // namespace codexlimit
