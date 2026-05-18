#define NOMINMAX

#include "usage_service.h"

#include "config_auth.h"
#include "http_client.h"
#include "json_utils.h"
#include "relay_key.h"
#include "text_utils.h"

#include <algorithm>

namespace codexlimit
{

    namespace
    {

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

        void FillLimit(WindowLimit &out, std::string_view obj)
        {
            if (obj.empty())
                return;
            out.present = true;
            out.used_percent = static_cast<int>(JsonNumberValue(obj, "used_percent").value_or(0));
            out.window_seconds = static_cast<int>(JsonNumberValue(obj, "limit_window_seconds").value_or(0));
            out.reset_at = JsonNumberValue(obj, "reset_at").value_or(0);
        }

        bool IsRelayAuthRemovedResponse(const std::optional<HttpResponse> &response)
        {
            return response &&
                   response->status == 498 &&
                   JsonStringValue(response->body, "error") == "usage_auth_removed";
        }

        UsageState RelayAuthRemovedState()
        {
            DeleteRelayKeyFiles();
            UsageState state;
            state.ok = false;
            state.status = StatusCode::RelayAuthRemoved;
            return state;
        }

    } // namespace

    std::optional<UsageState> FetchUsage()
    {
        AppConfig cfg = LoadConfig();
        auto auth = LoadAuth(cfg);
        if (!auth || auth->access_token.empty())
            return std::nullopt;

        std::wstring headers = L"Authorization: Bearer " + auth->access_token + L"\r\n";
        if (!auth->account_id.empty())
            headers += L"ChatGPT-Account-ID: " + auth->account_id + L"\r\n";
        headers += L"User-Agent: CodexLimitFloat/1.0\r\n";

        auto response = HttpRequestOfficialFirst(L"GET", BuildUsageUrl(cfg), headers);
        if (!response)
            return std::nullopt;
        if (IsRelayAuthRemovedResponse(response))
            return RelayAuthRemovedState();
        if ((response->status == 401 || response->status == 403) && RefreshAuth(cfg, *auth))
        {
            headers = L"Authorization: Bearer " + auth->access_token + L"\r\n";
            if (!auth->account_id.empty())
                headers += L"ChatGPT-Account-ID: " + auth->account_id + L"\r\n";
            headers += L"User-Agent: CodexLimitFloat/1.0\r\n";
            response = HttpRequestOfficialFirst(L"GET", BuildUsageUrl(cfg), headers);
            if (IsRelayAuthRemovedResponse(response))
                return RelayAuthRemovedState();
        }
        if (!response || response->status < 200 || response->status >= 300)
            return std::nullopt;

        UsageState state;
        state.plan_display = PlanTypeDisplayName(JsonStringValue(response->body, "plan_type"));
        std::string_view rate_limit = ObjectForKey(response->body, "rate_limit");
        FillLimit(state.primary, ObjectForKey(rate_limit, "primary_window"));
        FillLimit(state.secondary, ObjectForKey(rate_limit, "secondary_window"));
        state.ok = state.primary.present || state.secondary.present;
        state.status = state.ok ? StatusCode::Updated : StatusCode::NoWindow;
        return state;
    }

} // namespace codexlimit
