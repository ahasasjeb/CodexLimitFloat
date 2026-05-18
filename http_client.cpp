#define NOMINMAX

#include "http_client.h"

#include "file_utils.h"
#include "json_utils.h"
#include "text_utils.h"

#include <algorithm>
#include <optional>
#include <windows.h>
#include <winhttp.h>

namespace codexlimit
{

    namespace
    {

        struct ParsedUrl
        {
            std::wstring scheme;
            std::wstring host;
            INTERNET_PORT port = 0;
            std::wstring path;
            bool secure = true;
        };

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
                        forwarded.status = static_cast<unsigned long>(*status);
                        forwarded.body = JsonStringValue(relay_response.body, "body");
                        return forwarded;
                    }
                }
                return relay_response;
            }
        };

    } // namespace

    std::optional<HttpResponse> HttpRequest(const std::wstring &method, const std::wstring &url, const std::wstring &headers, const std::string &body)
    {
        auto parsed = ParseUrl(url);
        if (!parsed)
            return std::nullopt;
        HINTERNET session = WinHttpOpen(L"CodexLimitFloat/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return std::nullopt;
        WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);
        HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        DWORD flags = parsed->secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), parsed->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
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

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size, nullptr);
        HttpResponse response;
        response.status = status;
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

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return response;
    }

    std::optional<HttpResponse> HttpRequestOfficialFirst(const std::wstring &method, const std::wstring &url, const std::wstring &headers, const std::string &body)
    {
        auto response = HttpRequest(method, url, headers, body);
        if (response)
            return response;

        RelayForwardClient relay;
        return relay.Forward(method, url, headers, body);
    }

} // namespace codexlimit
