#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <wincred.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "credui.lib")
#pragma comment(lib, "shcore.lib")

namespace {

constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshIntervalMs = 60 * 1000;
constexpr UINT kInitialRefreshDelayMs = 200;
constexpr int kHttpTimeoutMs = 3000;
constexpr UINT kRefreshDoneMessage = WM_APP + 1;
constexpr int kCompactScalePercent = 76;

struct WindowLimit {
    bool present = false;
    int used_percent = 0;
    int window_seconds = 0;
    long long reset_at = 0;
};

enum class StatusCode : std::uint8_t {
    Loading,
    Refreshing,
    Updated,
    NoWindow,
    ReadFailed,
    ThreadFailed,
};

constexpr const wchar_t* kStatusLoading = L"正在读取 Codex 限额...";
constexpr const wchar_t* kStatusRefreshing = L"正在刷新...";
constexpr const wchar_t* kStatusUpdated = L"已更新";
constexpr const wchar_t* kStatusNoWindow = L"usage 返回中没有限额窗口";
constexpr const wchar_t* kStatusReadFailed = L"读取失败：请确认已用 ChatGPT/Codex backend 登录";
constexpr const wchar_t* kStatusThreadFailed = L"刷新线程启动失败";

const wchar_t* StatusText(StatusCode code) {
    switch (code) {
    case StatusCode::Loading:
        return kStatusLoading;
    case StatusCode::Refreshing:
        return kStatusRefreshing;
    case StatusCode::Updated:
        return kStatusUpdated;
    case StatusCode::NoWindow:
        return kStatusNoWindow;
    case StatusCode::ReadFailed:
        return kStatusReadFailed;
    case StatusCode::ThreadFailed:
        return kStatusThreadFailed;
    default:
        return kStatusLoading;
    }
}

struct UsageState {
    WindowLimit primary;
    WindowLimit secondary;
    StatusCode status = StatusCode::Loading;
    bool ok = false;
};

enum class AuthStore {
    None,
    Env,
    File,
    Keyring,
};

struct AuthTokens {
    AuthStore store = AuthStore::None;
    std::wstring id_token;
    std::wstring access_token;
    std::wstring refresh_token;
    std::wstring account_id;
};

struct AppConfig {
    std::wstring codex_home;
    std::wstring base_url = L"https://chatgpt.com/backend-api/";
    std::wstring credentials_store = L"auto";
};

HWND g_hwnd = nullptr;
HFONT g_title_font = nullptr;
HFONT g_value_font = nullptr;
HFONT g_small_font = nullptr;
UsageState g_state;
CRITICAL_SECTION g_state_lock;
bool g_refresh_running = false;
bool g_shutting_down = false;
HANDLE g_refresh_thread = nullptr;
HANDLE g_single_instance_mutex = nullptr;
int g_ui_scale = 100;
int g_resolution_scale = 100;
UINT g_dpi = 96;

struct RefreshResult {
    UsageState state;
};

int S(int value) {
    return MulDiv(value, g_ui_scale, 100);
}

int DpiScale(int value) {
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

void UpdateEffectiveScale() {
    g_ui_scale = std::max(45, MulDiv(DpiScale(g_resolution_scale), kCompactScalePercent, 100));
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring GetEnvVar(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return L"";
    std::wstring value(size - 1, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    return value;
}

std::wstring JoinPath(std::wstring base, std::wstring_view leaf) {
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base.push_back(L'\\');
    base.append(leaf);
    return base;
}

std::optional<std::string> ReadFileUtf8(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) return std::nullopt;
    data.resize(read);
    return data;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == data.size();
}

std::string JsonStringValue(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return "";
    std::string out;
    bool escape = false;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (escape) {
            switch (c) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(c); break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

std::optional<long long> JsonNumberValue(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    bool neg = pos < json.size() && json[pos] == '-';
    if (neg) ++pos;
    long long value = 0;
    bool any = false;
    while (pos < json.size() && isdigit(static_cast<unsigned char>(json[pos]))) {
        any = true;
        value = value * 10 + (json[pos++] - '0');
    }
    if (!any) return std::nullopt;
    return neg ? -value : value;
}

std::string ObjectForKey(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return "";
    pos = json.find('{', pos + needle.size());
    if (pos == std::string_view::npos) return "";
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return std::string(json.substr(pos, i - pos + 1));
    }
    return "";
}

std::wstring GetCodexHome() {
    std::wstring home = GetEnvVar(L"CODEX_HOME");
    if (!home.empty()) return home;
    std::wstring user = GetEnvVar(L"USERPROFILE");
    return JoinPath(user, L".codex");
}

std::wstring TomlStringValue(std::string_view toml, std::string_view key) {
    std::string needle = std::string(key);
    size_t pos = 0;
    while ((pos = toml.find(needle, pos)) != std::string_view::npos) {
        bool line_start = pos == 0 || toml[pos - 1] == '\n' || toml[pos - 1] == '\r';
        if (line_start) {
            size_t eq = toml.find('=', pos + needle.size());
            size_t quote = eq == std::string_view::npos ? eq : toml.find('"', eq + 1);
            size_t end = quote == std::string_view::npos ? quote : toml.find('"', quote + 1);
            if (end != std::string_view::npos) return Utf8ToWide(toml.substr(quote + 1, end - quote - 1));
        }
        pos += needle.size();
    }
    return L"";
}

AppConfig LoadConfig() {
    AppConfig cfg;
    cfg.codex_home = GetCodexHome();
    if (auto data = ReadFileUtf8(JoinPath(cfg.codex_home, L"config.toml"))) {
        std::wstring base = TomlStringValue(*data, "chatgpt_base_url");
        std::wstring store = TomlStringValue(*data, "cli_auth_credentials_store");
        if (!base.empty()) cfg.base_url = base;
        if (!store.empty()) cfg.credentials_store = store;
    }
    return cfg;
}

std::optional<AuthTokens> ReadAuthJson(const AppConfig& cfg) {
    auto data = ReadFileUtf8(JoinPath(cfg.codex_home, L"auth.json"));
    if (!data) return std::nullopt;
    if (JsonStringValue(*data, "auth_mode") != "chatgpt") return std::nullopt;
    AuthTokens tokens;
    tokens.store = AuthStore::File;
    tokens.id_token = Utf8ToWide(JsonStringValue(*data, "id_token"));
    tokens.access_token = Utf8ToWide(JsonStringValue(*data, "access_token"));
    tokens.refresh_token = Utf8ToWide(JsonStringValue(*data, "refresh_token"));
    tokens.account_id = Utf8ToWide(JsonStringValue(*data, "account_id"));
    if (tokens.access_token.empty()) return std::nullopt;
    return tokens;
}

std::optional<AuthTokens> ReadAuthFromCredentialManager() {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(L"Codex Auth", CRED_TYPE_GENERIC, 0, &cred)) return std::nullopt;
    std::string blob(reinterpret_cast<const char*>(cred->CredentialBlob),
                     reinterpret_cast<const char*>(cred->CredentialBlob) + cred->CredentialBlobSize);
    CredFree(cred);
    if (blob.empty()) return std::nullopt;
    if (JsonStringValue(blob, "auth_mode") != "chatgpt") return std::nullopt;
    AuthTokens tokens;
    tokens.store = AuthStore::Keyring;
    tokens.id_token = Utf8ToWide(JsonStringValue(blob, "id_token"));
    tokens.access_token = Utf8ToWide(JsonStringValue(blob, "access_token"));
    tokens.refresh_token = Utf8ToWide(JsonStringValue(blob, "refresh_token"));
    tokens.account_id = Utf8ToWide(JsonStringValue(blob, "account_id"));
    if (tokens.access_token.empty()) return std::nullopt;
    return tokens;
}

std::optional<AuthTokens> LoadAuth(const AppConfig& cfg) {
    std::wstring env_token = GetEnvVar(L"CODEX_ACCESS_TOKEN");
    if (!env_token.empty()) {
        AuthTokens tokens;
        tokens.store = AuthStore::Env;
        tokens.access_token = env_token;
        return tokens;
    }
    if (cfg.credentials_store == L"keyring" || cfg.credentials_store == L"auto") {
        if (auto tokens = ReadAuthFromCredentialManager()) return tokens;
    }
    if (cfg.credentials_store == L"file" || cfg.credentials_store == L"auto") {
        if (auto tokens = ReadAuthJson(cfg)) return tokens;
    }
    return std::nullopt;
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

bool ReplaceJsonStringValue(std::string& json, std::string_view key, std::string_view value) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    size_t begin = json.find('"', pos + 1);
    if (begin == std::string::npos) return false;

    bool escape = false;
    for (size_t end = begin + 1; end < json.size(); ++end) {
        char c = json[end];
        if (escape) {
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            json.replace(begin + 1, end - begin - 1, JsonEscape(value));
            return true;
        }
    }
    return false;
}

std::string AuthJson(const AuthTokens& tokens) {
    std::string id = WideToUtf8(tokens.id_token);
    std::string access = WideToUtf8(tokens.access_token);
    std::string refresh = WideToUtf8(tokens.refresh_token);
    std::string account = WideToUtf8(tokens.account_id);
    return "{\n"
           "  \"auth_mode\": \"chatgpt\",\n"
           "  \"tokens\": {\n"
           "    \"id_token\": \"" + JsonEscape(id) + "\",\n"
           "    \"access_token\": \"" + JsonEscape(access) + "\",\n"
           "    \"refresh_token\": \"" + JsonEscape(refresh) + "\",\n"
           "    \"account_id\": \"" + JsonEscape(account) + "\"\n"
           "  }\n"
           "}\n";
}

void SaveAuth(const AppConfig& cfg, const AuthTokens& tokens) {
    if (tokens.store == AuthStore::File || cfg.credentials_store == L"file") {
        std::wstring path = JoinPath(cfg.codex_home, L"auth.json");
        std::string json = ReadFileUtf8(path).value_or(AuthJson(tokens));
        bool changed = false;
        changed |= ReplaceJsonStringValue(json, "id_token", WideToUtf8(tokens.id_token));
        changed |= ReplaceJsonStringValue(json, "access_token", WideToUtf8(tokens.access_token));
        changed |= ReplaceJsonStringValue(json, "refresh_token", WideToUtf8(tokens.refresh_token));
        changed |= ReplaceJsonStringValue(json, "account_id", WideToUtf8(tokens.account_id));
        WriteFileUtf8(path, changed ? json : AuthJson(tokens));
        return;
    }
    if (tokens.store == AuthStore::Keyring || cfg.credentials_store == L"keyring") {
        std::string blob = AuthJson(tokens);
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = const_cast<LPWSTR>(L"Codex Auth");
        cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        CredWriteW(&cred, 0);
    }
}

struct ParsedUrl {
    std::wstring scheme;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
    bool secure = true;
};

std::optional<ParsedUrl> ParseUrl(const std::wstring& url) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t scheme[16]{};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszScheme = scheme;
    parts.dwSchemeLength = ARRAYSIZE(scheme);
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return std::nullopt;
    ParsedUrl out;
    out.scheme.assign(parts.lpszScheme, parts.dwSchemeLength);
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.port = parts.nPort;
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (out.path.empty()) out.path = L"/";
    return out;
}

std::wstring BuildUsageUrl(const AppConfig& cfg) {
    std::wstring base = cfg.base_url;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    std::wstring lower = base;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (lower.find(L"/backend-api") != std::wstring::npos) return base + L"/wham/usage";
    return base + L"/api/codex/usage";
}

struct HttpResponse {
    DWORD status = 0;
    std::string body;
};

std::optional<HttpResponse> HttpRequest(const std::wstring& method, const std::wstring& url, const std::wstring& headers, const std::string& body = {}) {
    auto parsed = ParseUrl(url);
    if (!parsed) return std::nullopt;
    HINTERNET session = WinHttpOpen(L"CodexLimitFloat/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);
    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    DWORD flags = parsed->secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    BOOL sent = WinHttpSendRequest(request,
                                   headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()),
                                   0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size, nullptr);
    HttpResponse response;
    response.status = status;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        if (response.body.size() + available > 2 * 1024 * 1024) break;
        size_t old = response.body.size();
        response.body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, response.body.data() + old, available, &read)) break;
        response.body.resize(old + read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

bool RefreshAuth(const AppConfig& cfg, AuthTokens& tokens) {
    if (tokens.refresh_token.empty()) return false;
    std::string refresh = WideToUtf8(tokens.refresh_token);
    std::string body = "{\"client_id\":\"app_EMoamEEZ73f0CkXaXp7hrann\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"" +
                       JsonEscape(refresh) + "\"}";
    std::wstring headers = L"Content-Type: application/json\r\nUser-Agent: CodexLimitFloat/1.0\r\n";
    auto response = HttpRequest(L"POST", L"https://auth.openai.com/oauth/token", headers, body);
    if (!response || response->status < 200 || response->status >= 300) return false;
    std::wstring new_access = Utf8ToWide(JsonStringValue(response->body, "access_token"));
    if (new_access.empty()) return false;
    std::wstring new_id = Utf8ToWide(JsonStringValue(response->body, "id_token"));
    std::wstring new_refresh = Utf8ToWide(JsonStringValue(response->body, "refresh_token"));
    tokens.access_token = new_access;
    if (!new_id.empty()) tokens.id_token = new_id;
    if (!new_refresh.empty()) tokens.refresh_token = new_refresh;
    SaveAuth(cfg, tokens);
    return true;
}

void FillLimit(WindowLimit& out, const std::string& obj) {
    if (obj.empty()) return;
    out.present = true;
    out.used_percent = static_cast<int>(JsonNumberValue(obj, "used_percent").value_or(0));
    out.window_seconds = static_cast<int>(JsonNumberValue(obj, "limit_window_seconds").value_or(0));
    out.reset_at = JsonNumberValue(obj, "reset_at").value_or(0);
}

std::optional<UsageState> FetchUsage() {
    AppConfig cfg = LoadConfig();
    auto auth = LoadAuth(cfg);
    if (!auth || auth->access_token.empty()) return std::nullopt;

    std::wstring headers = L"Authorization: Bearer " + auth->access_token + L"\r\n";
    if (!auth->account_id.empty()) headers += L"ChatGPT-Account-ID: " + auth->account_id + L"\r\n";
    headers += L"User-Agent: CodexLimitFloat/1.0\r\n";

    auto response = HttpRequest(L"GET", BuildUsageUrl(cfg), headers);
    if (!response) return std::nullopt;
    if ((response->status == 401 || response->status == 403) && RefreshAuth(cfg, *auth)) {
        headers = L"Authorization: Bearer " + auth->access_token + L"\r\n";
        if (!auth->account_id.empty()) headers += L"ChatGPT-Account-ID: " + auth->account_id + L"\r\n";
        headers += L"User-Agent: CodexLimitFloat/1.0\r\n";
        response = HttpRequest(L"GET", BuildUsageUrl(cfg), headers);
    }
    if (!response || response->status < 200 || response->status >= 300) return std::nullopt;

    UsageState state;
    std::string rate_limit = ObjectForKey(response->body, "rate_limit");
    FillLimit(state.primary, ObjectForKey(rate_limit, "primary_window"));
    FillLimit(state.secondary, ObjectForKey(rate_limit, "secondary_window"));
    state.ok = state.primary.present || state.secondary.present;
    state.status = state.ok ? StatusCode::Updated : StatusCode::NoWindow;
    return state;
}

DWORD WINAPI RefreshThread(LPVOID) {
    RefreshResult* message = new RefreshResult();
    auto result = FetchUsage();
    if (result) {
        message->state = *result;
    } else {
        message->state.ok = false;
        message->state.status = StatusCode::ReadFailed;
    }
    EnterCriticalSection(&g_state_lock);
    bool can_post = !g_shutting_down && g_hwnd != nullptr && IsWindow(g_hwnd);
    LeaveCriticalSection(&g_state_lock);
    if (!can_post || !PostMessageW(g_hwnd, kRefreshDoneMessage, 0, reinterpret_cast<LPARAM>(message))) {
        delete message;
        EnterCriticalSection(&g_state_lock);
        g_refresh_running = false;
        LeaveCriticalSection(&g_state_lock);
    }
    return 0;
}

void StartRefresh() {
    EnterCriticalSection(&g_state_lock);
    if (g_refresh_running || g_shutting_down) {
        LeaveCriticalSection(&g_state_lock);
        return;
    }
    g_refresh_running = true;
    g_state.status = StatusCode::Refreshing;
    LeaveCriticalSection(&g_state_lock);
    InvalidateRect(g_hwnd, nullptr, TRUE);
    HANDLE thread = CreateThread(nullptr, 0, RefreshThread, nullptr, 0, nullptr);
    EnterCriticalSection(&g_state_lock);
    if (thread) {
        if (g_refresh_thread) CloseHandle(g_refresh_thread);
        g_refresh_thread = thread;
    } else {
        g_refresh_running = false;
        g_state.status = StatusCode::ThreadFailed;
    }
    LeaveCriticalSection(&g_state_lock);
    if (!thread) InvalidateRect(g_hwnd, nullptr, TRUE);
}

void ResetRefreshTimer(HWND hwnd) {
    KillTimer(hwnd, kRefreshTimer);
    SetTimer(hwnd, kRefreshTimer, kRefreshIntervalMs, nullptr);
}

void TriggerManualRefresh(HWND hwnd) {
    StartRefresh();
    ResetRefreshTimer(hwnd);
}

const wchar_t* FormatWindowLabel(const WindowLimit& limit, const wchar_t* fallback_title, wchar_t* buffer, size_t buffer_len) {
    if (!limit.present) return fallback_title;
    int seconds = limit.window_seconds;
    if (seconds >= 3600 && seconds % 3600 == 0) {
        swprintf_s(buffer, buffer_len, L"%d 小时使用限额", seconds / 3600);
        return buffer;
    }
    if (seconds >= 60) {
        swprintf_s(buffer, buffer_len, L"%d 分钟使用限额", seconds / 60);
        return buffer;
    }
    return L"使用限额";
}

const wchar_t* FormatResetTime(long long unix_seconds, wchar_t* buffer, size_t buffer_len) {
    if (unix_seconds <= 0) return L"--";
    FILETIME ft{};
    ULONGLONG ticks = (static_cast<ULONGLONG>(unix_seconds) + 11644473600ULL) * 10000000ULL;
    ft.dwLowDateTime = static_cast<DWORD>(ticks);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    FILETIME local_ft{};
    SYSTEMTIME st{}, now{};
    FileTimeToLocalFileTime(&ft, &local_ft);
    FileTimeToSystemTime(&local_ft, &st);
    GetLocalTime(&now);
    if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay) {
        swprintf_s(buffer, buffer_len, L"%02d:%02d", st.wHour, st.wMinute);
    } else {
        swprintf_s(buffer, buffer_len, L"%d年%d月%d日 %d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
    return buffer;
}

void RoundRectPath(HDC dc, RECT r, int radius) {
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(radius), S(radius));
}

void FillRoundRect(HDC dc, const RECT& rect, COLORREF fill_color, COLORREF border_color, int radius) {
    HBRUSH brush = CreateSolidBrush(fill_color);
    HPEN pen = CreatePen(PS_SOLID, 1, border_color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRectPath(dc, rect, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawProgressBar(HDC dc, RECT bar, int remaining) {
    FillRoundRect(dc, bar, RGB(231, 233, 238), RGB(231, 233, 238), 6);
    RECT fill = bar;
    fill.right = fill.left + MulDiv(fill.right - fill.left, std::clamp(remaining, 0, 100), 100);
    if (fill.right > fill.left) {
        FillRoundRect(dc, fill, RGB(34, 197, 94), RGB(34, 197, 94), 6);
    }
}

void DrawLimitRow(HDC dc, const RECT& rect, const WindowLimit& limit, const wchar_t* fallback_title) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(106, 119, 137));
    SelectObject(dc, g_title_font);
    wchar_t title_buf[32]{};
    const wchar_t* title = FormatWindowLabel(limit, fallback_title, title_buf, ARRAYSIZE(title_buf));
    RECT title_rect{rect.left, rect.top, rect.right - S(82), rect.top + S(18)};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    SelectObject(dc, g_small_font);
    wchar_t reset_buf[64]{};
    const wchar_t* reset = FormatResetTime(limit.present ? limit.reset_at : 0, reset_buf, ARRAYSIZE(reset_buf));
    RECT reset_rect{rect.right - S(108), rect.top, rect.right, rect.top + S(18)};
    DrawTextW(dc, reset, -1, &reset_rect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    SetTextColor(dc, RGB(0, 0, 0));
    SelectObject(dc, g_value_font);
    int remaining = limit.present ? std::clamp(100 - limit.used_percent, 0, 100) : 0;
    wchar_t value_buf[8]{};
    const wchar_t* value = limit.present
                               ? (swprintf_s(value_buf, ARRAYSIZE(value_buf), L"%d%%", remaining), value_buf)
                               : L"--";
    RECT value_rect{rect.left, rect.top + S(21), rect.left + S(82), rect.top + S(53)};
    DrawTextW(dc, value, -1, &value_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(dc, RGB(78, 88, 104));
    SelectObject(dc, g_title_font);
    RECT remain_rect{rect.left + S(84), rect.top + S(28), rect.left + S(124), rect.top + S(48)};
    DrawTextW(dc, L"剩余", -1, &remain_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT bar{rect.left + S(126), rect.top + S(36), rect.right, rect.top + S(44)};
    DrawProgressBar(dc, bar, remaining);
}

RECT RefreshButtonRect(const RECT& client) {
    return RECT{client.right - S(56), S(8), client.right - S(14), S(30)};
}

bool PtInRectInclusive(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x <= rect.right && point.y >= rect.top && point.y <= rect.bottom;
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    HBRUSH bg = CreateSolidBrush(RGB(245, 246, 248));
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    UsageState snapshot;
    EnterCriticalSection(&g_state_lock);
    snapshot = g_state;
    LeaveCriticalSection(&g_state_lock);

    RECT panel{0, 0, client.right, client.bottom};
    FillRoundRect(dc, panel, RGB(255, 255, 255), RGB(224, 228, 235), 18);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(23, 28, 38));
    SelectObject(dc, g_title_font);
    RECT header{S(14), S(8), client.right - S(90), S(28)};
    DrawTextW(dc, L"Codex 限额", -1, &header, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT refresh = RefreshButtonRect(client);
    FillRoundRect(dc, refresh, RGB(248, 250, 252), RGB(224, 228, 235), 10);
    SetTextColor(dc, RGB(78, 88, 104));
    SelectObject(dc, g_small_font);
    DrawTextW(dc, L"刷新", -1, &refresh, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    HBRUSH dot = CreateSolidBrush(snapshot.ok ? RGB(34, 197, 94) : RGB(239, 68, 68));
    HGDIOBJ old_brush = SelectObject(dc, dot);
    HPEN dot_pen = CreatePen(PS_SOLID, 1, snapshot.ok ? RGB(34, 197, 94) : RGB(239, 68, 68));
    HGDIOBJ old_pen = SelectObject(dc, dot_pen);
    Ellipse(dc, client.right - S(70), S(15), client.right - S(62), S(23));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(dot_pen);
    DeleteObject(dot);

    HPEN divider = CreatePen(PS_SOLID, 1, RGB(238, 240, 244));
    old_pen = SelectObject(dc, divider);
    MoveToEx(dc, S(14), S(32), nullptr);
    LineTo(dc, client.right - S(14), S(32));
    MoveToEx(dc, S(14), S(88), nullptr);
    LineTo(dc, client.right - S(14), S(88));
    SelectObject(dc, old_pen);
    DeleteObject(divider);

    RECT primary{S(14), S(39), client.right - S(14), S(82)};
    RECT secondary{S(14), S(96), client.right - S(14), S(139)};
    DrawLimitRow(dc, primary, snapshot.primary, L"短窗口使用限额");
    DrawLimitRow(dc, secondary, snapshot.secondary, L"长窗口使用限额");

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, snapshot.ok ? RGB(106, 119, 137) : RGB(170, 70, 70));
    SelectObject(dc, g_small_font);
    RECT status{S(14), client.bottom - S(20), client.right - S(14), client.bottom - S(4)};
    DrawTextW(dc, StatusText(snapshot.status), -1, &status, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    EndPaint(hwnd, &ps);
}

HFONT MakeFont(int px, int weight) {
    int font_px = std::max(S(px), px >= 20 ? 11 : 7);
    return CreateFontW(-font_px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
}

void DestroyFonts() {
    if (g_title_font) {
        DeleteObject(g_title_font);
        g_title_font = nullptr;
    }
    if (g_value_font) {
        DeleteObject(g_value_font);
        g_value_font = nullptr;
    }
    if (g_small_font) {
        DeleteObject(g_small_font);
        g_small_font = nullptr;
    }
}

void CreateFonts() {
    DestroyFonts();
    g_title_font = MakeFont(12, FW_SEMIBOLD);
    g_value_font = MakeFont(23, FW_BOLD);
    g_small_font = MakeFont(10, FW_NORMAL);
}

void ApplyWindowShape(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    HRGN region = CreateRoundRectRgn(0, 0, rect.right + 1, rect.bottom + 1, S(18), S(18));
    SetWindowRgn(hwnd, region, TRUE);
}

LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowRefuseStartupNotification(HINSTANCE instance, int width, int height) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = NotifyWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"CodexLimitNotifyWindow";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!hwnd) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 2;
    nid.hIcon = LoadIconW(nullptr, IDI_WARNING);
    wcscpy_s(nid.szTip, L"Codex 限额");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"Codex 限额未启动");
        swprintf_s(nid.szInfo, L"当前分辨率为 %d x %d，小于等于 800 x 600 的屏幕空间不足。", width, height);
        nid.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        Sleep(3500);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    DestroyWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case kRefreshDoneMessage: {
        RefreshResult* result = reinterpret_cast<RefreshResult*>(lparam);
        EnterCriticalSection(&g_state_lock);
        if (result) g_state = result->state;
        g_refresh_running = false;
        if (g_refresh_thread) {
            CloseHandle(g_refresh_thread);
            g_refresh_thread = nullptr;
        }
        LeaveCriticalSection(&g_state_lock);
        delete result;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_CREATE:
        CreateFonts();
        ApplyWindowShape(hwnd);
        SetTimer(hwnd, kRefreshTimer, kInitialRefreshDelayMs, nullptr);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wparam);
        UpdateEffectiveScale();
        CreateFonts();
        RECT* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyWindowShape(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE:
        ApplyWindowShape(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == kRefreshTimer) {
            KillTimer(hwnd, kRefreshTimer);
            StartRefresh();
            SetTimer(hwnd, kRefreshTimer, kRefreshIntervalMs, nullptr);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (PtInRectInclusive(RefreshButtonRect(client), pt)) {
            TriggerManualRefresh(hwnd);
            return 0;
        }
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_DESTROY: {
        EnterCriticalSection(&g_state_lock);
        g_shutting_down = true;
        HANDLE thread = g_refresh_thread;
        g_refresh_thread = nullptr;
        LeaveCriticalSection(&g_state_lock);
        if (thread) {
            WaitForSingleObject(thread, kHttpTimeoutMs + 1000);
            CloseHandle(thread);
        }
        MSG pending{};
        while (PeekMessageW(&pending, hwnd, kRefreshDoneMessage, kRefreshDoneMessage, PM_REMOVE)) {
            delete reinterpret_cast<RefreshResult*>(pending.lParam);
        }
        DestroyFonts();
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    g_dpi = GetDpiForSystem();

    g_single_instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\CodexLimitFloatSingleInstance");
    if (!g_single_instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_single_instance_mutex) CloseHandle(g_single_instance_mutex);
        return 0;
    }

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int work_width = work_area.right - work_area.left;
    int work_height = work_area.bottom - work_area.top;
    if (work_width <= 800 || work_height <= 600) {
        ShowRefuseStartupNotification(instance, work_width, work_height);
        CloseHandle(g_single_instance_mutex);
        return 0;
    }
    int reference_width = std::min(work_width, MulDiv(work_height, 16, 9));
    g_resolution_scale = std::clamp(MulDiv(reference_width, 100, 1366), 85, 125);
    UpdateEffectiveScale();

    InitializeCriticalSection(&g_state_lock);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"CodexLimitFloatWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    int width = S(340);
    int height = S(164);
    int x = work_area.right - width - S(24);
    int y = work_area.top + S(96);
    if (y + height > work_area.bottom - S(12)) y = work_area.bottom - height - S(12);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Codex 限额",
                             WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteCriticalSection(&g_state_lock);
    CloseHandle(g_single_instance_mutex);
    return 0;
}
