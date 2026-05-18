#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include "app_types.h"
#include "relay_key.h"
#include "startup_animation.h"
#include "ui_render.h"
#include "usage_service.h"
#include "resource.h"

#include <algorithm>
#include <string>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "credui.lib")
#pragma comment(lib, "shcore.lib")

namespace
{

    using namespace codexlimit;

    // 定时器 ID，用于周期性刷新限额数据
    constexpr UINT_PTR kRefreshTimer = 1;
    // 自动刷新间隔：每 60 秒刷新一次
    constexpr UINT kRefreshIntervalMs = 60 * 1000;
    // 程序启动后首次刷新的延迟（毫秒）
    constexpr UINT kInitialRefreshDelayMs = 200;
    constexpr UINT kExitCommand = 1001;
    // 自定义消息：刷新完成通知
    constexpr UINT kRefreshDoneMessage = WM_APP + 1;
    constexpr UINT kTrayIconMessage = WM_APP + 2;
    constexpr UINT kTrayIconId = 1;
    // 窗口缩放百分比（紧凑模式）
    constexpr int kCompactScalePercent = 76;


    // 全局变量：主窗口句柄
    HWND g_hwnd = nullptr;
    // UI 资源
    UiFonts g_fonts;
    // 当前使用状态
    UsageState g_state;
    // 状态访问的临界区锁
    CRITICAL_SECTION g_state_lock;
    // 刷新线程运行标志
    bool g_refresh_running = false;
    // 程序正在关闭标志
    bool g_shutting_down = false;
    bool g_relay_auth_removed_notice_shown = false;
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
    ShapeCache g_shape;

    // 根据 UI 缩放比例调整数值
    // value: 原始值
    // 返回：缩放后的值
    int S(int value)
    {
        return Scale(value, g_ui_scale);
    }

    // 更新有效缩放比例
    // 结合分辨率缩放和紧凑模式比例，计算最终 UI 缩放
    void UpdateEffectiveScale()
    {
        g_ui_scale = ComputeEffectiveScale(g_resolution_scale, g_dpi, kCompactScalePercent);
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

    RECT RefreshButtonRect(const RECT &client)
    {
        return codexlimit::RefreshButtonRect(client, g_ui_scale);
    }

    void CreateFonts()
    {
        CreateUiFonts(g_fonts, g_ui_scale);
    }

    void DestroyFonts()
    {
        DestroyUiFonts(g_fonts);
    }

    void ApplyWindowShape(HWND hwnd)
    {
        codexlimit::ApplyWindowShape(hwnd, g_ui_scale, g_shape);
    }

    // 窗口绘制函数
    void Paint(HWND hwnd)
    {
        UsageState snapshot;
        EnterCriticalSection(&g_state_lock);
        snapshot = g_state;
        LeaveCriticalSection(&g_state_lock);
        PaintUsageWindow(hwnd, snapshot, g_fonts, g_ui_scale);
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
            bool show_relay_auth_removed_notice = false;
            EnterCriticalSection(&g_state_lock);
            if (g_has_pending_state)
            {
                g_state = g_pending_state;
                g_has_pending_state = false;
            }
            if (g_state.status == StatusCode::RelayAuthRemoved && !g_relay_auth_removed_notice_shown)
            {
                g_relay_auth_removed_notice_shown = true;
                show_relay_auth_removed_notice = true;
            }
            g_refresh_running = false;
            if (g_refresh_thread)
            {
                CloseHandle(g_refresh_thread);
                g_refresh_thread = nullptr;
            }
            LeaveCriticalSection(&g_state_lock);
            InvalidateRect(hwnd, nullptr, TRUE);
            if (show_relay_auth_removed_notice)
            {
                MessageBoxW(hwnd,
                            L"24小时内向 OpenAI 请求失败了 2 次，服务端已判定当前转发认证无效。\n\n"
                            L"程序已删除 relay_server_key.json。\n\n"
                            L"请重新填写密钥后重新启动程序。",
                            L"Codex 限额警告",
                            MB_OK | MB_ICONWARNING);
            }
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
    codexlimit::startup_animation::Start(instance, window_rect, DefaultPalette().guide);

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
