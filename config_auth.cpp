#define NOMINMAX

#include "config_auth.h"

#include "file_utils.h"
#include "http_client.h"
#include "json_utils.h"
#include "text_utils.h"

#include <windows.h>
#include <wincred.h>

#include <string>
#include <string_view>

namespace codexlimit
{

    namespace
    {

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

    } // namespace

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

    void SaveAuth(const AppConfig &cfg, const AuthTokens &tokens)
    {
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

} // namespace codexlimit
