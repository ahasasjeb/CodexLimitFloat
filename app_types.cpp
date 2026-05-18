#include "app_types.h"

namespace codexlimit
{

namespace
{
constexpr const wchar_t *kStatusLoading = L"正在读取 Codex 限额...";
constexpr const wchar_t *kStatusRefreshing = L"正在刷新...";
constexpr const wchar_t *kStatusUpdated = L"已更新";
constexpr const wchar_t *kStatusNoWindow = L"usage 返回中没有限额窗口";
constexpr const wchar_t *kStatusReadFailed = L"读取失败：请确认已用 ChatGPT/Codex backend 登录";
constexpr const wchar_t *kStatusRelayAuthRemoved = L"警告：24小时内向 OpenAI 请求失败 2 次，已判定无效并删除转发密钥，请重新启动程序";
constexpr const wchar_t *kStatusThreadFailed = L"刷新线程启动失败";
} // namespace

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
    case StatusCode::RelayAuthRemoved:
        return kStatusRelayAuthRemoved;
    case StatusCode::ThreadFailed:
        return kStatusThreadFailed;
    default:
        return kStatusLoading;
    }
}

} // namespace codexlimit
