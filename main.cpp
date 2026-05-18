#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <wincred.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include "startup_animation.h"
#include "resource.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "credui.lib")
#pragma comment(lib, "shcore.lib")

namespace
{

    // 定时器 ID，用于周期性刷新限额数据
    constexpr UINT_PTR kRefreshTimer = 1;
    // 自动刷新间隔：每 60 秒刷新一次
    constexpr UINT kRefreshIntervalMs = 60 * 1000;
    // 程序启动后首次刷新的延迟（毫秒）
    constexpr UINT kInitialRefreshDelayMs = 200;
    constexpr UINT kExitCommand = 1001;
    // HTTP 请求超时时间（毫秒）
    constexpr int kHttpTimeoutMs = 3000;
    // 自定义消息：刷新完成通知
    constexpr UINT kRefreshDoneMessage = WM_APP + 1;
    constexpr UINT kTrayIconMessage = WM_APP + 2;
    constexpr UINT kTrayIconId = 1;
    constexpr const wchar_t *kRelayKeyFileName = L"relay_server_key.json";
    // 窗口缩放百分比（紧凑模式）
    constexpr int kCompactScalePercent = 76;

    // 存储单个时间窗口的限额信息
    struct WindowLimit
    {
        bool present = false;   // 该窗口是否存在
        int used_percent = 0;   // 已使用百分比
        int window_seconds = 0; // 窗口时长（秒）
        long long reset_at = 0; // 重置时间戳（Unix 时间）
    };

    // 程序运行状态枚举
    enum class StatusCode : std::uint8_t
    {
        Loading,      // 正在加载
        Refreshing,   // 正在刷新
        Updated,      // 已更新
        NoWindow,     // 没有检测到限额窗口
        ReadFailed,   // 读取失败
        ThreadFailed, // 刷新线程启动失败
    };

    // 各状态对应的中文显示文本
    constexpr const wchar_t *kStatusLoading = L"正在读取 Codex 限额...";
    constexpr const wchar_t *kStatusRefreshing = L"正在刷新...";
    constexpr const wchar_t *kStatusUpdated = L"已更新";
    constexpr const wchar_t *kStatusNoWindow = L"usage 返回中没有限额窗口";
    constexpr const wchar_t *kStatusReadFailed = L"读取失败：请确认已用 ChatGPT/Codex backend 登录";
    constexpr const wchar_t *kStatusThreadFailed = L"刷新线程启动失败";

    // 根据状态码返回对应的显示文本
    const wchar_t *StatusText(StatusCode code)
    {
        switch (code)
        {
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
    // 使用状态结构体，包含短/长两个时间窗口的限额信息
    struct UsageState
    {
        WindowLimit primary;       // 主要限额窗口（短窗口）
        WindowLimit secondary;     // 次要限额窗口（长窗口）
        std::wstring plan_display; // ChatGPT 订阅层级显示名称
        StatusCode status = StatusCode::Loading;
        bool ok = false; // 数据是否有效
    };

    // 凭证存储方式枚举
    enum class AuthStore
    {
        None,    // 无
        Env,     // 环境变量
        File,    // 文件存储
        Keyring, // Windows 凭据管理器
    };

    // 认证令牌结构体
    struct AuthTokens
    {
        AuthStore store = AuthStore::None;
        std::wstring id_token;      // ID 令牌
        std::wstring access_token;  // 访问令牌
        std::wstring refresh_token; // 刷新令牌
        std::wstring account_id;    // 账户 ID
    };

    // 应用程序配置结构体
    struct AppConfig
    {
        std::wstring codex_home;                                     // Codex 配置目录
        std::wstring base_url = L"https://chatgpt.com/backend-api/"; // API 基础 URL
        std::wstring credentials_store = L"auto";                    // 凭证存储方式（auto/keyring/file）
    };

    // 全局变量：主窗口句柄
    HWND g_hwnd = nullptr;
    // 字体句柄
    HFONT g_title_font = nullptr; // 标题字体
    HFONT g_value_font = nullptr; // 数值字体
    HFONT g_small_font = nullptr; // 小字体
    // 当前使用状态
    UsageState g_state;
    // 状态访问的临界区锁
    CRITICAL_SECTION g_state_lock;
    // 刷新线程运行标志
    bool g_refresh_running = false;
    // 程序正在关闭标志
    bool g_shutting_down = false;
    // 刷新线程句柄
    HANDLE g_refresh_thread = nullptr;
    // 单实例互斥体，防止程序多次运行
    HANDLE g_single_instance_mutex = nullptr;
    // 待处理的刷新状态（用于跨线程传递数据）
    UsageState g_pending_state;
    bool g_has_pending_state = false;
    bool g_tray_icon_added = false;
    // UI 缩放相关变量
    int g_ui_scale = 100;         // UI 缩放比例
    int g_resolution_scale = 100; // 分辨率缩放比例
    UINT g_dpi = 96;              // 当前 DPI
    // 窗口形状缓存（用于优化圆角窗口的重绘）
    int g_shape_width = -1;
    int g_shape_height = -1;
    int g_shape_radius = -1;
    struct Palette
    {
        COLORREF bg, panel, border, title, text, muted, soft_text, button, divider, bar, ok, bad, guide;
    };

    Palette P()
    {
        return Palette{RGB(245, 246, 248), RGB(255, 255, 255), RGB(224, 228, 235), RGB(23, 28, 38), RGB(0, 0, 0), RGB(106, 119, 137), RGB(78, 88, 104), RGB(248, 250, 252), RGB(238, 240, 244), RGB(231, 233, 238), RGB(34, 197, 94), RGB(239, 68, 68), RGB(37, 99, 235)};
    }

    // 根据 UI 缩放比例调整数值
    // value: 原始值
    // 返回：缩放后的值
    int S(int value)
    {
        return MulDiv(value, g_ui_scale, 100);
    }

    // 根据 DPI 缩放数值
    int DpiScale(int value)
    {
        return MulDiv(value, static_cast<int>(g_dpi), 96);
    }

    // 更新有效缩放比例
    // 结合分辨率缩放和紧凑模式比例，计算最终 UI 缩放
    void UpdateEffectiveScale()
    {
        g_ui_scale = std::max(45, MulDiv(DpiScale(g_resolution_scale), kCompactScalePercent, 100));
    }

    // 将 UTF-8 字符串转换为宽字符串（UTF-16）
    std::wstring Utf8ToWide(std::string_view text)
    {
        if (text.empty())
            return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
        return out;
    }

    // 将宽字符串（UTF-16）转换为 UTF-8 字符串
    std::string WideToUtf8(std::wstring_view text)
    {
        if (text.empty())
            return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    // 获取环境变量值
    std::wstring GetEnvVar(const wchar_t *name)
    {
        DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size == 0)
            return L"";
        std::wstring value(size - 1, L'\0');
        GetEnvironmentVariableW(name, value.data(), size);
        return value;
    }

    // 连接路径组成部分，添加反斜杠分隔符
    std::wstring JoinPath(std::wstring base, std::wstring_view leaf)
    {
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
            base.push_back(L'\\');
        base.append(leaf);
        return base;
    }

    // 获取当前工作目录，用于读取转发服务器密钥配置
    std::wstring GetCurrentDirectoryPath()
    {
        DWORD size = GetCurrentDirectoryW(0, nullptr);
        if (size == 0)
            return L"";
        std::wstring path(size, L'\0');
        GetCurrentDirectoryW(size, path.data());
        path.resize(size - 1);
        return path;
    }

    // 获取 exe 所在目录；转发服务器密钥文件随程序同级放置
    std::wstring GetExecutableDirectoryPath()
    {
        wchar_t path[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (length == 0 || length >= ARRAYSIZE(path))
            return GetCurrentDirectoryPath();
        std::wstring exe_path(path, length);
        size_t slash = exe_path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return GetCurrentDirectoryPath();
        return exe_path.substr(0, slash);
    }

    // 读取 UTF-8 编码的文件内容
    std::optional<std::string> ReadFileUtf8(const std::wstring &path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return std::nullopt;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart > 8 * 1024 * 1024)
        {
            CloseHandle(file);
            return std::nullopt;
        }
        std::string data(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok)
            return std::nullopt;
        data.resize(read);
        return data;
    }

    // 将内容写入 UTF-8 编码的文件
    bool WriteFileUtf8(const std::wstring &path, const std::string &data)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(file);
        return ok && written == data.size();
    }

    bool EnsureRelayKeyFileAt(const std::wstring &dir)
    {
        if (dir.empty())
            return false;
        std::wstring path = JoinPath(dir, kRelayKeyFileName);
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return true;

        return WriteFileUtf8(path,
                             "{\n"
                             "  \"server_url\": \"https://your-domain.example/api/codex-relay\",\n"
                             "  \"api_key\": \"填入你的转发服务器密钥\",\n"
                             "  \"one_way_key\": \"填入单向加密密钥\"\n"
                             "}\n");
    }

    // 程序启动时生成转发服务器密钥配置模板
    void EnsureRelayKeyFile()
    {
        EnsureRelayKeyFileAt(GetExecutableDirectoryPath());
        EnsureRelayKeyFileAt(GetCurrentDirectoryPath());
    }

    // 在 JSON 字符串中查找指定键的位置
    size_t FindJsonKey(std::string_view json, std::string_view key)
    {
        size_t pos = 0;
        while ((pos = json.find('"', pos)) != std::string_view::npos)
        {
            size_t value = pos + 1;
            if (value + key.size() < json.size() &&
                json.compare(value, key.size(), key) == 0 &&
                json[value + key.size()] == '"')
            {
                return pos;
            }
            pos = value;
        }
        return std::string_view::npos;
    }

    // 从 JSON 中提取字符串值
    std::string JsonStringValue(std::string_view json, std::string_view key)
    {
        size_t pos = FindJsonKey(json, key);
        if (pos == std::string_view::npos)
            return "";
        pos = json.find(':', pos + key.size() + 2);
        if (pos == std::string_view::npos)
            return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string_view::npos)
            return "";
        std::string out;
        bool escape = false;
        for (size_t i = pos + 1; i < json.size(); ++i)
        {
            char c = json[i];
            if (escape)
            {
                switch (c)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(c);
                    break;
                }
                escape = false;
                continue;
            }
            if (c == '\\')
            {
                escape = true;
                continue;
            }
            if (c == '"')
                break;
            out.push_back(c);
        }
        return out;
    }

    // 从 JSON 中提取数值
    std::optional<long long> JsonNumberValue(std::string_view json, std::string_view key)
    {
        size_t pos = FindJsonKey(json, key);
        if (pos == std::string_view::npos)
            return std::nullopt;
        pos = json.find(':', pos + key.size() + 2);
        if (pos == std::string_view::npos)
            return std::nullopt;
        ++pos;
        while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos])))
            ++pos;
        bool neg = pos < json.size() && json[pos] == '-';
        if (neg)
            ++pos;
        long long value = 0;
        bool any = false;
        while (pos < json.size() && isdigit(static_cast<unsigned char>(json[pos])))
        {
            any = true;
            value = value * 10 + (json[pos++] - '0');
        }
        if (!any)
            return std::nullopt;
        return neg ? -value : value;
    }

    // 从 JSON 中提取对象（嵌套的 {...}）
    std::string_view ObjectForKey(std::string_view json, std::string_view key)
    {
        size_t pos = FindJsonKey(json, key);
        if (pos == std::string_view::npos)
            return {};
        pos = json.find('{', pos + key.size() + 2);
        if (pos == std::string_view::npos)
            return {};
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        for (size_t i = pos; i < json.size(); ++i)
        {
            char c = json[i];
            if (in_string)
            {
                if (escape)
                    escape = false;
                else if (c == '\\')
                    escape = true;
                else if (c == '"')
                    in_string = false;
                continue;
            }
            if (c == '"')
                in_string = true;
            else if (c == '{')
                ++depth;
            else if (c == '}' && --depth == 0)
                return json.substr(pos, i - pos + 1);
        }
        return {};
    }

    // 将字符串转为小写，用于订阅层级匹配
    std::string LowerAscii(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    // 按官方 Codex CLI 的语义将原始 plan type 转为 UI 显示名称
    std::wstring PlanTypeDisplayName(std::string_view raw)
    {
        std::string plan = LowerAscii(raw);
        if (plan.empty())
            return L"";
        if (plan == "free")
            return L"Free";
        if (plan == "go")
            return L"Go";
        if (plan == "plus")
            return L"Plus";
        if (plan == "pro")
            return L"Pro";
        if (plan == "prolite")
            return L"Pro Lite";
        if (plan == "team" || plan == "self_serve_business_usage_based")
            return L"Business";
        if (plan == "business" || plan == "enterprise_cbp_usage_based" || plan == "enterprise" || plan == "hc")
            return L"Enterprise";
        if (plan == "education" || plan == "edu")
            return L"Edu";

        return Utf8ToWide(raw);
    }

    // 获取 Codex 配置目录路径
    // 优先使用 CODEX_HOME 环境变量，否则使用 USERPROFILE\.codex
    std::wstring GetCodexHome()
    {
        std::wstring home = GetEnvVar(L"CODEX_HOME");
        if (!home.empty())
            return home;
        std::wstring user = GetEnvVar(L"USERPROFILE");
        return JoinPath(user, L".codex");
    }

    // 从 TOML 格式配置中提取字符串值
    std::wstring TomlStringValue(std::string_view toml, std::string_view key)
    {
        std::string needle = std::string(key);
        size_t pos = 0;
        while ((pos = toml.find(needle, pos)) != std::string_view::npos)
        {
            bool line_start = pos == 0 || toml[pos - 1] == '\n' || toml[pos - 1] == '\r';
            if (line_start)
            {
                size_t eq = toml.find('=', pos + needle.size());
                size_t quote = eq == std::string_view::npos ? eq : toml.find('"', eq + 1);
                size_t end = quote == std::string_view::npos ? quote : toml.find('"', quote + 1);
                if (end != std::string_view::npos)
                    return Utf8ToWide(toml.substr(quote + 1, end - quote - 1));
            }
            pos += needle.size();
        }
        return L"";
    }

    // 加载应用程序配置
    AppConfig LoadConfig()
    {
        AppConfig cfg;
        cfg.codex_home = GetCodexHome();
        if (auto data = ReadFileUtf8(JoinPath(cfg.codex_home, L"config.toml")))
        {
            std::wstring base = TomlStringValue(*data, "chatgpt_base_url");
            std::wstring store = TomlStringValue(*data, "cli_auth_credentials_store");
            if (!base.empty())
                cfg.base_url = base;
            if (!store.empty())
                cfg.credentials_store = store;
        }
        return cfg;
    }

    // 从 auth.json 文件读取认证令牌
    std::optional<AuthTokens> ReadAuthJson(const AppConfig &cfg)
    {
        auto data = ReadFileUtf8(JoinPath(cfg.codex_home, L"auth.json"));
        if (!data)
            return std::nullopt;
        if (JsonStringValue(*data, "auth_mode") != "chatgpt")
            return std::nullopt;
        AuthTokens tokens;
        tokens.store = AuthStore::File;
        tokens.id_token = Utf8ToWide(JsonStringValue(*data, "id_token"));
        tokens.access_token = Utf8ToWide(JsonStringValue(*data, "access_token"));
        tokens.refresh_token = Utf8ToWide(JsonStringValue(*data, "refresh_token"));
        tokens.account_id = Utf8ToWide(JsonStringValue(*data, "account_id"));
        if (tokens.access_token.empty())
            return std::nullopt;
        return tokens;
    }

    // 从 Windows 凭据管理器读取认证令牌
    std::optional<AuthTokens> ReadAuthFromCredentialManager()
    {
        PCREDENTIALW cred = nullptr;
        if (!CredReadW(L"Codex Auth", CRED_TYPE_GENERIC, 0, &cred))
            return std::nullopt;
        std::string blob(reinterpret_cast<const char *>(cred->CredentialBlob),
                         reinterpret_cast<const char *>(cred->CredentialBlob) + cred->CredentialBlobSize);
        CredFree(cred);
        if (blob.empty())
            return std::nullopt;
        if (JsonStringValue(blob, "auth_mode") != "chatgpt")
            return std::nullopt;
        AuthTokens tokens;
        tokens.store = AuthStore::Keyring;
        tokens.id_token = Utf8ToWide(JsonStringValue(blob, "id_token"));
        tokens.access_token = Utf8ToWide(JsonStringValue(blob, "access_token"));
        tokens.refresh_token = Utf8ToWide(JsonStringValue(blob, "refresh_token"));
        tokens.account_id = Utf8ToWide(JsonStringValue(blob, "account_id"));
        if (tokens.access_token.empty())
            return std::nullopt;
        return tokens;
    }

    // 加载认证令牌
    // 优先级：环境变量 > 凭据管理器 > 文件
    std::optional<AuthTokens> LoadAuth(const AppConfig &cfg)
    {
        std::wstring env_token = GetEnvVar(L"CODEX_ACCESS_TOKEN");
        if (!env_token.empty())
        {
            AuthTokens tokens;
            tokens.store = AuthStore::Env;
            tokens.access_token = env_token;
            return tokens;
        }
        if (cfg.credentials_store == L"keyring" || cfg.credentials_store == L"auto")
        {
            if (auto tokens = ReadAuthFromCredentialManager())
                return tokens;
        }
        if (cfg.credentials_store == L"file" || cfg.credentials_store == L"auto")
        {
            if (auto tokens = ReadAuthJson(cfg))
                return tokens;
        }
        return std::nullopt;
    }

    // 对字符串进行 JSON 转义
    std::string JsonEscape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (char c : text)
        {
            switch (c)
            {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
            }
        }
        return out;
    }

    // 替换 JSON 中的字符串值
    bool ReplaceJsonStringValue(std::string &json, std::string_view key, std::string_view value)
    {
        std::string needle = "\"" + std::string(key) + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return false;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return false;
        size_t begin = json.find('"', pos + 1);
        if (begin == std::string::npos)
            return false;

        bool escape = false;
        for (size_t end = begin + 1; end < json.size(); ++end)
        {
            char c = json[end];
            if (escape)
            {
                escape = false;
            }
            else if (c == '\\')
            {
                escape = true;
            }
            else if (c == '"')
            {
                json.replace(begin + 1, end - begin - 1, JsonEscape(value));
                return true;
            }
        }
        return false;
    }

    // 生成认证令牌的 JSON 格式字符串
    std::string AuthJson(const AuthTokens &tokens)
    {
        std::string id = WideToUtf8(tokens.id_token);
        std::string access = WideToUtf8(tokens.access_token);
        std::string refresh = WideToUtf8(tokens.refresh_token);
        std::string account = WideToUtf8(tokens.account_id);
        return "{\n"
               "  \"auth_mode\": \"chatgpt\",\n"
               "  \"tokens\": {\n"
               "    \"id_token\": \"" +
               JsonEscape(id) + "\",\n"
                                "    \"access_token\": \"" +
               JsonEscape(access) + "\",\n"
                                    "    \"refresh_token\": \"" +
               JsonEscape(refresh) + "\",\n"
                                     "    \"account_id\": \"" +
               JsonEscape(account) + "\"\n"
                                     "  }\n"
                                     "}\n";
    }

    // 保存认证令牌到文件或凭据管理器
    void SaveAuth(const AppConfig &cfg, const AuthTokens &tokens)
    {
        // 保存到文件
        if (tokens.store == AuthStore::File || cfg.credentials_store == L"file")
        {
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
        // 保存到 Windows 凭据管理器
        if (tokens.store == AuthStore::Keyring || cfg.credentials_store == L"keyring")
        {
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

    // URL 解析结果结构体
    struct ParsedUrl
    {
        std::wstring scheme;
        std::wstring host;
        INTERNET_PORT port = 0;
        std::wstring path;
        bool secure = true;
    };

    // 解析 URL 字符串
    std::optional<ParsedUrl> ParseUrl(const std::wstring &url)
    {
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
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
            return std::nullopt;
        ParsedUrl out;
        out.scheme.assign(parts.lpszScheme, parts.dwSchemeLength);
        out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
        out.port = parts.nPort;
        out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        if (out.path.empty())
            out.path = L"/";
        return out;
    }

    // 构建使用量查询 URL
    std::wstring BuildUsageUrl(const AppConfig &cfg)
    {
        std::wstring base = cfg.base_url;
        while (!base.empty() && base.back() == L'/')
            base.pop_back();
        std::wstring lower = base;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c)
                       { return static_cast<wchar_t>(towlower(c)); });
        if (lower.find(L"/backend-api") != std::wstring::npos)
            return base + L"/wham/usage";
        return base + L"/api/codex/usage";
    }

    // HTTP 响应结构体
    struct HttpResponse
    {
        DWORD status = 0;
        std::string body;
    };

    // 发送 HTTP 请求
    // method: HTTP 方法（GET/POST）
    // url: 请求 URL
    // headers: 附加请求头
    // body: 请求体内容
    std::optional<HttpResponse> HttpRequest(const std::wstring &method, const std::wstring &url, const std::wstring &headers, const std::string &body = {})
    {
        auto parsed = ParseUrl(url);
        if (!parsed)
            return std::nullopt;
        // 创建 WinHTTP 会话
        HINTERNET session = WinHttpOpen(L"CodexLimitFloat/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return std::nullopt;
        // 设置超时
        WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);
        // 建立连接
        HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        // 打开请求
        DWORD flags = parsed->secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        // 发送请求
        BOOL sent = WinHttpSendRequest(request,
                                       headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                       headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                                       body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()),
                                       static_cast<DWORD>(body.size()),
                                       static_cast<DWORD>(body.size()),
                                       0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        // 读取响应状态码
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size, nullptr);
        HttpResponse response;
        response.status = status;
        // 读取响应体
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                break;
            if (response.body.size() + available > 2 * 1024 * 1024)
                break;
            size_t old = response.body.size();
            response.body.resize(old + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, response.body.data() + old, available, &read))
                break;
            response.body.resize(old + read);
        }
        // 清理资源
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    // 连接负责转发请求的服务器。只有直连官方接口失败时才会使用。
    class RelayForwardClient
    {
    public:
        RelayForwardClient()
        {
            LoadLocalKeyFile();
        }

        bool Enabled() const
        {
            return !server_url_.empty() && !api_key_.empty();
        }

        std::optional<HttpResponse> Forward(const std::wstring &method, const std::wstring &url, const std::wstring &headers, const std::string &body) const
        {
            if (!Enabled())
                return std::nullopt;

            std::string payload = "{\n"
                                  "  \"method\": \"" +
                                  JsonEscape(WideToUtf8(method)) + "\",\n"
                                                                  "  \"url\": \"" +
                                  JsonEscape(WideToUtf8(url)) + "\",\n"
                                                               "  \"headers\": \"" +
                                  JsonEscape(WideToUtf8(headers)) + "\",\n"
                                                                   "  \"body\": \"" +
                                  JsonEscape(body) + "\",\n"
                                                     "  \"one_way_key\": \"" +
                                  JsonEscape(WideToUtf8(one_way_key_)) + "\"\n"
                                                                    "}\n";
            std::wstring relay_headers = L"Content-Type: application/json\r\n"
                                         L"Authorization: Bearer " +
                                         api_key_ + L"\r\n"
                                                    L"X-Codex-Relay-Key: " +
                                         api_key_ + L"\r\n"
                                                    L"X-Codex-One-Way-Key: " +
                                         one_way_key_ + L"\r\n"
                                                    L"User-Agent: CodexLimitFloat/1.0\r\n";

            auto relay_response = HttpRequest(L"POST", server_url_, relay_headers, payload);
            if (!relay_response)
                return std::nullopt;

            return DecodeRelayResponse(*relay_response);
        }

    private:
        std::wstring server_url_;
        std::wstring api_key_;
        std::wstring one_way_key_;

        void LoadLocalKeyFile()
        {
            std::wstring dir = GetExecutableDirectoryPath();
            if (dir.empty())
                return;
            auto data = ReadFileUtf8(JoinPath(dir, kRelayKeyFileName));
            if (!data)
                return;

            std::wstring server_url = Utf8ToWide(JsonStringValue(*data, "server_url"));
            std::wstring api_key = Utf8ToWide(JsonStringValue(*data, "api_key"));
            std::wstring one_way_key = Utf8ToWide(JsonStringValue(*data, "one_way_key"));
            if (server_url.empty() || api_key.empty())
                return;
            if (server_url.find(L"your-domain.example") != std::wstring::npos ||
                api_key.find(L"填入") != std::wstring::npos ||
                api_key.find(L'\r') != std::wstring::npos ||
                api_key.find(L'\n') != std::wstring::npos ||
                one_way_key.find(L'\r') != std::wstring::npos ||
                one_way_key.find(L'\n') != std::wstring::npos)
            {
                return;
            }
            if (one_way_key.find(L"填入") != std::wstring::npos)
                one_way_key.clear();

            server_url_ = server_url;
            api_key_ = api_key;
            one_way_key_ = one_way_key;
        }

        std::optional<HttpResponse> DecodeRelayResponse(const HttpResponse &relay_response) const
        {
            if (FindJsonKey(relay_response.body, "status") != std::string_view::npos &&
                FindJsonKey(relay_response.body, "body") != std::string_view::npos)
            {
                auto status = JsonNumberValue(relay_response.body, "status");
                if (status)
                {
                    HttpResponse forwarded;
                    forwarded.status = static_cast<DWORD>(*status);
                    forwarded.body = JsonStringValue(relay_response.body, "body");
                    return forwarded;
                }
            }
            return relay_response;
        }
    };

    std::optional<HttpResponse> HttpRequestOfficialFirst(const std::wstring &method, const std::wstring &url, const std::wstring &headers, const std::string &body = {})
    {
        auto response = HttpRequest(method, url, headers, body);
        if (response)
            return response;

        RelayForwardClient relay;
        return relay.Forward(method, url, headers, body);
    }

    // 刷新访问令牌
    bool RefreshAuth(const AppConfig &cfg, AuthTokens &tokens)
    {
        if (tokens.refresh_token.empty())
            return false;
        std::string refresh = WideToUtf8(tokens.refresh_token);
        std::string body = "{\"client_id\":\"app_EMoamEEZ73f0CkXaXp7hrann\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"" +
                           JsonEscape(refresh) + "\"}";
        std::wstring headers = L"Content-Type: application/json\r\nUser-Agent: CodexLimitFloat/1.0\r\n";
        auto response = HttpRequestOfficialFirst(L"POST", L"https://auth.openai.com/oauth/token", headers, body);
        if (!response || response->status < 200 || response->status >= 300)
            return false;
        std::wstring new_access = Utf8ToWide(JsonStringValue(response->body, "access_token"));
        if (new_access.empty())
            return false;
        std::wstring new_id = Utf8ToWide(JsonStringValue(response->body, "id_token"));
        std::wstring new_refresh = Utf8ToWide(JsonStringValue(response->body, "refresh_token"));
        tokens.access_token = new_access;
        if (!new_id.empty())
            tokens.id_token = new_id;
        if (!new_refresh.empty())
            tokens.refresh_token = new_refresh;
        SaveAuth(cfg, tokens);
        return true;
    }

    // 解析限额数据到 WindowLimit 结构体
    void FillLimit(WindowLimit &out, std::string_view obj)
    {
        if (obj.empty())
            return;
        out.present = true;
        out.used_percent = static_cast<int>(JsonNumberValue(obj, "used_percent").value_or(0));
        out.window_seconds = static_cast<int>(JsonNumberValue(obj, "limit_window_seconds").value_or(0));
        out.reset_at = JsonNumberValue(obj, "reset_at").value_or(0);
    }

    // 获取使用量状态
    // 发送 HTTP 请求获取 Codex 使用限额信息
    std::optional<UsageState> FetchUsage()
    {
        AppConfig cfg = LoadConfig();
        auto auth = LoadAuth(cfg);
        if (!auth || auth->access_token.empty())
            return std::nullopt;

        // 构建请求头
        std::wstring headers = L"Authorization: Bearer " + auth->access_token + L"\r\n";
        if (!auth->account_id.empty())
            headers += L"ChatGPT-Account-ID: " + auth->account_id + L"\r\n";
        headers += L"User-Agent: CodexLimitFloat/1.0\r\n";

        // 发送请求
        auto response = HttpRequestOfficialFirst(L"GET", BuildUsageUrl(cfg), headers);
        if (!response)
            return std::nullopt;
        // 如果认证失败，尝试刷新令牌后重试
        if ((response->status == 401 || response->status == 403) && RefreshAuth(cfg, *auth))
        {
            headers = L"Authorization: Bearer " + auth->access_token + L"\r\n";
            if (!auth->account_id.empty())
                headers += L"ChatGPT-Account-ID: " + auth->account_id + L"\r\n";
            headers += L"User-Agent: CodexLimitFloat/1.0\r\n";
            response = HttpRequestOfficialFirst(L"GET", BuildUsageUrl(cfg), headers);
        }
        if (!response || response->status < 200 || response->status >= 300)
            return std::nullopt;

        // 解析响应
        UsageState state;
        state.plan_display = PlanTypeDisplayName(JsonStringValue(response->body, "plan_type"));
        std::string_view rate_limit = ObjectForKey(response->body, "rate_limit");
        FillLimit(state.primary, ObjectForKey(rate_limit, "primary_window"));
        FillLimit(state.secondary, ObjectForKey(rate_limit, "secondary_window"));
        state.ok = state.primary.present || state.secondary.present;
        state.status = state.ok ? StatusCode::Updated : StatusCode::NoWindow;
        return state;
    }

    // 刷新线程函数
    // 在后台线程中获取使用量数据，然后通过消息机制通知主窗口
    DWORD WINAPI RefreshThread(LPVOID)
    {
        UsageState state;
        auto result = FetchUsage();
        if (result)
        {
            state = *result;
        }
        else
        {
            state.ok = false;
            state.status = StatusCode::ReadFailed;
        }
        EnterCriticalSection(&g_state_lock);
        bool can_post = !g_shutting_down && g_hwnd != nullptr && IsWindow(g_hwnd);
        if (can_post)
        {
            g_pending_state = state;
            g_has_pending_state = true;
        }
        LeaveCriticalSection(&g_state_lock);
        if (!can_post || !PostMessageW(g_hwnd, kRefreshDoneMessage, 0, 0))
        {
            EnterCriticalSection(&g_state_lock);
            if (can_post)
                g_has_pending_state = false;
            g_refresh_running = false;
            LeaveCriticalSection(&g_state_lock);
        }
        return 0;
    }

    // 启动刷新操作
    void StartRefresh()
    {
        EnterCriticalSection(&g_state_lock);
        if (g_refresh_running || g_shutting_down)
        {
            LeaveCriticalSection(&g_state_lock);
            return;
        }
        g_refresh_running = true;
        g_state.status = StatusCode::Refreshing;
        LeaveCriticalSection(&g_state_lock);
        InvalidateRect(g_hwnd, nullptr, TRUE);
        HANDLE thread = CreateThread(nullptr, 0, RefreshThread, nullptr, 0, nullptr);
        EnterCriticalSection(&g_state_lock);
        if (thread)
        {
            if (g_refresh_thread)
                CloseHandle(g_refresh_thread);
            g_refresh_thread = thread;
        }
        else
        {
            g_refresh_running = false;
            g_state.status = StatusCode::ThreadFailed;
        }
        LeaveCriticalSection(&g_state_lock);
        if (!thread)
            InvalidateRect(g_hwnd, nullptr, TRUE);
    }

    // 重置刷新定时器
    void ResetRefreshTimer(HWND hwnd)
    {
        KillTimer(hwnd, kRefreshTimer);
        SetTimer(hwnd, kRefreshTimer, kRefreshIntervalMs, nullptr);
    }

    // 触发手动刷新
    void TriggerManualRefresh(HWND hwnd)
    {
        StartRefresh();
        ResetRefreshTimer(hwnd);
    }

    // 格式化窗口标签文本
    const wchar_t *FormatWindowLabel(const WindowLimit &limit, const wchar_t *fallback_title, wchar_t *buffer, size_t buffer_len)
    {
        if (!limit.present)
            return fallback_title;
        int seconds = limit.window_seconds;
        if (seconds >= 3600 && seconds % 3600 == 0)
        {
            swprintf_s(buffer, buffer_len, L"%d 小时使用限额", seconds / 3600);
            return buffer;
        }
        if (seconds >= 60)
        {
            swprintf_s(buffer, buffer_len, L"%d 分钟使用限额", seconds / 60);
            return buffer;
        }
        return L"使用限额";
    }

    // 格式化重置时间显示
    const wchar_t *FormatResetTime(long long unix_seconds, wchar_t *buffer, size_t buffer_len)
    {
        if (unix_seconds <= 0)
            return L"--";
        FILETIME ft{};
        ULONGLONG ticks = (static_cast<ULONGLONG>(unix_seconds) + 11644473600ULL) * 10000000ULL;
        ft.dwLowDateTime = static_cast<DWORD>(ticks);
        ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        FILETIME local_ft{};
        SYSTEMTIME st{}, now{};
        FileTimeToLocalFileTime(&ft, &local_ft);
        FileTimeToSystemTime(&local_ft, &st);
        GetLocalTime(&now);
        // 如果是今天，只显示时间
        if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay)
        {
            swprintf_s(buffer, buffer_len, L"%02d:%02d", st.wHour, st.wMinute);
        }
        else
        {
            // 否则显示完整日期时间
            swprintf_s(buffer, buffer_len, L"%d年%d月%d日 %d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        }
        return buffer;
    }

    // 绘制圆角矩形边框路径
    void RoundRectPath(HDC dc, RECT r, int radius)
    {
        RoundRect(dc, r.left, r.top, r.right, r.bottom, S(radius), S(radius));
    }

    // 用纯色填充矩形
    void FillSolidRect(HDC dc, const RECT &rect, COLORREF color)
    {
        SetDCBrushColor(dc, color);
        FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    // 绘制圆角矩形（带填充和边框）
    void FillRoundRect(HDC dc, const RECT &rect, COLORREF fill_color, COLORREF border_color, int radius)
    {
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, fill_color);
        SetDCPenColor(dc, border_color);
        RoundRectPath(dc, rect, radius);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }

    // 绘制进度条
    void DrawProgressBar(HDC dc, RECT bar, int remaining)
    {
        Palette p = P();
        // 绘制背景
        FillRoundRect(dc, bar, p.bar, p.bar, 6);
        // 绘制填充部分
        RECT fill = bar;
        fill.right = fill.left + MulDiv(fill.right - fill.left, std::clamp(remaining, 0, 100), 100);
        if (fill.right > fill.left)
        {
            FillRoundRect(dc, fill, p.ok, p.ok, 6);
        }
    }

    // 绘制单个限额行
    void DrawLimitRow(HDC dc, const RECT &rect, const WindowLimit &limit, const wchar_t *fallback_title)
    {
        Palette p = P();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, p.muted);
        SelectObject(dc, g_title_font);
        // 绘制窗口标签
        wchar_t title_buf[32]{};
        const wchar_t *title = FormatWindowLabel(limit, fallback_title, title_buf, ARRAYSIZE(title_buf));
        RECT title_rect{rect.left, rect.top, rect.right - S(82), rect.top + S(18)};
        DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // 绘制重置时间
        SelectObject(dc, g_small_font);
        wchar_t reset_buf[64]{};
        const wchar_t *reset = FormatResetTime(limit.present ? limit.reset_at : 0, reset_buf, ARRAYSIZE(reset_buf));
        RECT reset_rect{rect.right - S(108), rect.top, rect.right, rect.top + S(18)};
        DrawTextW(dc, reset, -1, &reset_rect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // 绘制剩余百分比数值
        SetTextColor(dc, p.text);
        SelectObject(dc, g_value_font);
        int remaining = limit.present ? std::clamp(100 - limit.used_percent, 0, 100) : 0;
        wchar_t value_buf[8]{};
        const wchar_t *value = limit.present
                                   ? (swprintf_s(value_buf, ARRAYSIZE(value_buf), L"%d%%", remaining), value_buf)
                                   : L"--";
        RECT value_rect{rect.left, rect.top + S(21), rect.left + S(82), rect.top + S(53)};
        DrawTextW(dc, value, -1, &value_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // 绘制"剩余"标签
        SetTextColor(dc, p.soft_text);
        SelectObject(dc, g_title_font);
        RECT remain_rect{rect.left + S(84), rect.top + S(28), rect.left + S(124), rect.top + S(48)};
        DrawTextW(dc, L"剩余", -1, &remain_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // 绘制进度条
        RECT bar{rect.left + S(126), rect.top + S(36), rect.right, rect.top + S(44)};
        DrawProgressBar(dc, bar, remaining);
    }

    // 获取刷新按钮的矩形区域
    RECT RefreshButtonRect(const RECT &client)
    {
        return RECT{client.right - S(56), S(8), client.right - S(14), S(30)};
    }

    // 判断点是否在矩形内（包含边界）
    bool PtInRectInclusive(const RECT &rect, POINT point)
    {
        return point.x >= rect.left && point.x <= rect.right && point.y >= rect.top && point.y <= rect.bottom;
    }

    // 窗口绘制函数
    void Paint(HWND hwnd)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        Palette p = P();

        // 填充背景色
        FillSolidRect(dc, client, p.bg);

        // 获取当前状态快照
        UsageState snapshot;
        EnterCriticalSection(&g_state_lock);
        snapshot = g_state;
        LeaveCriticalSection(&g_state_lock);

        // 绘制主面板（圆角白色背景）
        RECT panel{0, 0, client.right, client.bottom};
        FillRoundRect(dc, panel, p.panel, p.border, 18);

        // 绘制标题
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, p.title);
        SelectObject(dc, g_title_font);
        std::wstring header_title = L"Codex 限额";
        if (!snapshot.plan_display.empty())
        {
            header_title += L" (";
            header_title += snapshot.plan_display;
            header_title += L")";
        }
        RECT header{S(14), S(8), client.right - S(90), S(28)};
        DrawTextW(dc, header_title.c_str(), -1, &header, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        // 绘制刷新按钮
        RECT refresh = RefreshButtonRect(client);
        FillRoundRect(dc, refresh, p.button, p.border, 10);
        SetTextColor(dc, p.soft_text);
        SelectObject(dc, g_small_font);
        DrawTextW(dc, L"刷新", -1, &refresh, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        // 绘制状态指示点（绿色表示正常，红色表示异常）
        COLORREF dot_color = snapshot.ok ? p.ok : p.bad;
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, dot_color);
        SetDCPenColor(dc, dot_color);
        Ellipse(dc, client.right - S(70), S(15), client.right - S(62), S(23));
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);

        // 绘制分隔线
        old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, p.divider);
        MoveToEx(dc, S(14), S(32), nullptr);
        LineTo(dc, client.right - S(14), S(32));
        MoveToEx(dc, S(14), S(88), nullptr);
        LineTo(dc, client.right - S(14), S(88));
        SelectObject(dc, old_pen);

        // 绘制限额行
        RECT primary{S(14), S(39), client.right - S(14), S(82)};
        RECT secondary{S(14), S(96), client.right - S(14), S(139)};
        DrawLimitRow(dc, primary, snapshot.primary, L"短窗口使用限额");
        DrawLimitRow(dc, secondary, snapshot.secondary, L"长窗口使用限额");

        // 绘制状态文本
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, snapshot.ok ? p.muted : p.bad);
        SelectObject(dc, g_small_font);
        RECT status{S(14), client.bottom - S(20), client.right - S(14), client.bottom - S(4)};
        DrawTextW(dc, StatusText(snapshot.status), -1, &status, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        EndPaint(hwnd, &ps);
    }

    // 创建字体
    HFONT MakeFont(int px, int weight)
    {
        int font_px = std::max(S(px), px >= 20 ? 11 : 7);
        return CreateFontW(-font_px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    }

    // 销毁字体资源
    void DestroyFonts()
    {
        if (g_title_font)
        {
            DeleteObject(g_title_font);
            g_title_font = nullptr;
        }
        if (g_value_font)
        {
            DeleteObject(g_value_font);
            g_value_font = nullptr;
        }
        if (g_small_font)
        {
            DeleteObject(g_small_font);
            g_small_font = nullptr;
        }
    }

    // 创建所有字体
    void CreateFonts()
    {
        DestroyFonts();
        g_title_font = MakeFont(12, FW_SEMIBOLD);
        g_value_font = MakeFont(23, FW_BOLD);
        g_small_font = MakeFont(10, FW_NORMAL);
    }

    // 应用窗口形状（圆角）
    void ApplyWindowShape(HWND hwnd)
    {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        int radius = S(18);
        if (width <= 0 || height <= 0)
            return;
        // 如果尺寸没变，跳过
        if (width == g_shape_width && height == g_shape_height && radius == g_shape_radius)
            return;

        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
        if (!region)
            return;
        if (SetWindowRgn(hwnd, region, TRUE))
        {
            g_shape_width = width;
            g_shape_height = height;
            g_shape_radius = radius;
        }
        else
        {
            DeleteObject(region);
        }
    }

    void ShowExitMenu(HWND hwnd, POINT screen)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;
        AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");
        SetForegroundWindow(hwnd);
        UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screen.x, screen.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        if (cmd == kExitCommand)
            DestroyWindow(hwnd);
    }

    HICON LoadAppIcon(HINSTANCE instance, int size)
    {
        return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    }

    void FillTrayIconData(HWND hwnd, NOTIFYICONDATAW &nid)
    {
        nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayIconMessage;
        nid.hIcon = LoadAppIcon(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), GetSystemMetrics(SM_CXSMICON));
        wcscpy_s(nid.szTip, L"Codex 限额");
    }

    void AddTrayIcon(HWND hwnd)
    {
        NOTIFYICONDATAW nid{};
        FillTrayIconData(hwnd, nid);
        g_tray_icon_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (nid.hIcon)
            DestroyIcon(nid.hIcon);
    }

    void DeleteTrayIcon(HWND hwnd)
    {
        if (!g_tray_icon_added)
            return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_tray_icon_added = false;
    }

    // 托盘通知用的消息窗口过程（仅转发默认处理）
    LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // 显示拒绝启动的通知
    void ShowRefuseStartupNotification(HINSTANCE instance, int width, int height)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = NotifyWndProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"CodexLimitNotifyWindow";
        RegisterClassW(&wc);

        // 创建消息窗口
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (!hwnd)
            return;

        // 设置托盘图标
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayIconMessage;
        nid.hIcon = LoadAppIcon(instance, GetSystemMetrics(SM_CXSMICON));
        wcscpy_s(nid.szTip, L"Codex 限额");
        if (Shell_NotifyIconW(NIM_ADD, &nid))
        {
            nid.uFlags = NIF_INFO;
            wcscpy_s(nid.szInfoTitle, L"Codex 限额未启动");
            swprintf_s(nid.szInfo, L"当前分辨率为 %d x %d，小于等于 800 x 600 的屏幕空间不足。", width, height);
            nid.dwInfoFlags = NIIF_WARNING;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            Sleep(3500);
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        if (nid.hIcon)
            DestroyIcon(nid.hIcon);
        DestroyWindow(hwnd);
    }

    // 主窗口消息处理过程
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg)
        {
        case kRefreshDoneMessage:
        {
            // 刷新完成消息：更新状态
            EnterCriticalSection(&g_state_lock);
            if (g_has_pending_state)
            {
                g_state = g_pending_state;
                g_has_pending_state = false;
            }
            g_refresh_running = false;
            if (g_refresh_thread)
            {
                CloseHandle(g_refresh_thread);
                g_refresh_thread = nullptr;
            }
            LeaveCriticalSection(&g_state_lock);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_CREATE:
            // 窗口创建：初始化
            CreateFonts();
            ApplyWindowShape(hwnd);
            AddTrayIcon(hwnd);
            SetTimer(hwnd, kRefreshTimer, kInitialRefreshDelayMs, nullptr);
            return 0;
        case kTrayIconMessage:
            if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU)
            {
                POINT pt{};
                if (LOWORD(lparam) == WM_CONTEXTMENU)
                {
                    pt = {GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
                }
                else
                {
                    GetCursorPos(&pt);
                }
                ShowExitMenu(hwnd, pt);
                return 0;
            }
            return 0;
        case WM_DPICHANGED:
        {
            // DPI 改变：重新计算缩放并调整窗口
            g_dpi = HIWORD(wparam);
            UpdateEffectiveScale();
            CreateFonts();
            RECT *suggested = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyWindowShape(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_SIZE:
            // 窗口大小改变：更新形状
            ApplyWindowShape(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == kExitCommand)
            {
                DestroyWindow(hwnd);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        case WM_TIMER:
            // 定时器：自动刷新
            if (wparam == kRefreshTimer)
            {
                KillTimer(hwnd, kRefreshTimer);
                StartRefresh();
                SetTimer(hwnd, kRefreshTimer, kRefreshIntervalMs, nullptr);
            }
            return 0;
        case WM_LBUTTONDOWN:
        {
            // 鼠标左键按下：检查是否点击刷新按钮或拖动窗口
            RECT client{};
            GetClientRect(hwnd, &client);
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (PtInRectInclusive(RefreshButtonRect(client), pt))
            {
                TriggerManualRefresh(hwnd);
                return 0;
            }
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        case WM_CONTEXTMENU:
        {
            POINT pt{};
            if (GET_X_LPARAM(lparam) == -1 && GET_Y_LPARAM(lparam) == -1)
            {
                RECT rect{};
                GetWindowRect(hwnd, &rect);
                pt = {rect.left + S(16), rect.top + S(16)};
            }
            else
            {
                pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            }
            ShowExitMenu(hwnd, pt);
            return 0;
        }
        case WM_RBUTTONUP:
        {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(hwnd, &pt);
            ShowExitMenu(hwnd, pt);
            return 0;
        }
        case WM_PAINT:
            // 绘制窗口
            Paint(hwnd);
            return 0;
        case WM_DESTROY:
        {
            // 窗口销毁：清理资源
            DeleteTrayIcon(hwnd);
            EnterCriticalSection(&g_state_lock);
            g_shutting_down = true;
            HANDLE thread = g_refresh_thread;
            g_refresh_thread = nullptr;
            g_has_pending_state = false;
            LeaveCriticalSection(&g_state_lock);
            if (thread)
            {
                WaitForSingleObject(thread, kHttpTimeoutMs + 1000);
                CloseHandle(thread);
            }
            // 处理残留消息
            MSG pending{};
            while (PeekMessageW(&pending, hwnd, kRefreshDoneMessage, kRefreshDoneMessage, PM_REMOVE))
            {
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

// 程序入口点
int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE /*previous_instance*/,
    _In_ LPWSTR /*command_line*/,
    _In_ int show)
{
    // 设置 DPI 感知模式
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    g_dpi = GetDpiForSystem();

    EnsureRelayKeyFile();

    // 确保单实例运行
    g_single_instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\CodexLimitFloatSingleInstance");
    if (!g_single_instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (g_single_instance_mutex)
            CloseHandle(g_single_instance_mutex);
        return 0;
    }

    // 检查屏幕分辨率是否足够
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int work_width = work_area.right - work_area.left;
    int work_height = work_area.bottom - work_area.top;
    if (work_width <= 800 || work_height <= 600)
    {
        ShowRefuseStartupNotification(instance, work_width, work_height);
        CloseHandle(g_single_instance_mutex);
        return 0;
    }

    // 计算分辨率缩放比例
    int reference_width = std::min(work_width, MulDiv(work_height, 16, 9));
    g_resolution_scale = std::clamp(MulDiv(reference_width, 100, 1366), 85, 125);
    UpdateEffectiveScale();

    // 初始化临界区
    InitializeCriticalSection(&g_state_lock);

    // 注册窗口类
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"CodexLimitFloatWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadAppIcon(instance, GetSystemMetrics(SM_CXICON));
    RegisterClassW(&wc);

    // 计算窗口位置和大小
    int width = S(340);
    int height = S(164);
    int x = work_area.right - width - S(24);
    int y = work_area.top + S(96);
    if (y + height > work_area.bottom - S(12))
        y = work_area.bottom - height - S(12);

    // 创建浮动窗口
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Codex 限额",
                             WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd)
        return 1;
    HICON big_icon = LoadAppIcon(instance, GetSystemMetrics(SM_CXICON));
    HICON small_icon = LoadAppIcon(instance, GetSystemMetrics(SM_CXSMICON));
    if (big_icon)
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    if (small_icon)
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    RECT window_rect{};
    GetWindowRect(g_hwnd, &window_rect);
    codexlimit::startup_animation::Start(instance, window_rect, P().guide);

    // 消息循环
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理资源
    DeleteCriticalSection(&g_state_lock);
    if (big_icon)
        DestroyIcon(big_icon);
    if (small_icon)
        DestroyIcon(small_icon);
    CloseHandle(g_single_instance_mutex);
    return 0;
}
