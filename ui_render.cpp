#define NOMINMAX

#include "ui_render.h"

#include <algorithm>
#include <string>

namespace codexlimit
{

    namespace
    {

        int S(int value, int ui_scale)
        {
            return Scale(value, ui_scale);
        }

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
            if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay)
            {
                swprintf_s(buffer, buffer_len, L"%02d:%02d", st.wHour, st.wMinute);
            }
            else
            {
                swprintf_s(buffer, buffer_len, L"%d年%d月%d日 %d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            }
            return buffer;
        }

        void RoundRectPath(HDC dc, RECT r, int radius, int ui_scale)
        {
            RoundRect(dc, r.left, r.top, r.right, r.bottom, S(radius, ui_scale), S(radius, ui_scale));
        }

        void FillSolidRect(HDC dc, const RECT &rect, COLORREF color)
        {
            SetDCBrushColor(dc, color);
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }

        void FillRoundRect(HDC dc, const RECT &rect, COLORREF fill_color, COLORREF border_color, int radius, int ui_scale)
        {
            HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
            HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
            SetDCBrushColor(dc, fill_color);
            SetDCPenColor(dc, border_color);
            RoundRectPath(dc, rect, radius, ui_scale);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
        }

        void DrawProgressBar(HDC dc, RECT bar, int remaining, int ui_scale)
        {
            Palette p = DefaultPalette();
            FillRoundRect(dc, bar, p.bar, p.bar, 6, ui_scale);
            RECT fill = bar;
            fill.right = fill.left + MulDiv(fill.right - fill.left, std::clamp(remaining, 0, 100), 100);
            if (fill.right > fill.left)
            {
                FillRoundRect(dc, fill, p.ok, p.ok, 6, ui_scale);
            }
        }

        void DrawLimitRow(HDC dc, const RECT &rect, const WindowLimit &limit, const wchar_t *fallback_title, const UiFonts &fonts, int ui_scale)
        {
            Palette p = DefaultPalette();
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, p.muted);
            SelectObject(dc, fonts.title);

            wchar_t title_buf[32]{};
            const wchar_t *title = FormatWindowLabel(limit, fallback_title, title_buf, ARRAYSIZE(title_buf));
            RECT title_rect{rect.left, rect.top, rect.right - S(82, ui_scale), rect.top + S(18, ui_scale)};
            DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            SelectObject(dc, fonts.small_font);
            wchar_t reset_buf[64]{};
            const wchar_t *reset = FormatResetTime(limit.present ? limit.reset_at : 0, reset_buf, ARRAYSIZE(reset_buf));
            RECT reset_rect{rect.right - S(108, ui_scale), rect.top, rect.right, rect.top + S(18, ui_scale)};
            DrawTextW(dc, reset, -1, &reset_rect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            SetTextColor(dc, p.text);
            SelectObject(dc, fonts.value);
            int remaining = limit.present ? std::clamp(100 - limit.used_percent, 0, 100) : 0;
            wchar_t value_buf[8]{};
            const wchar_t *value = limit.present
                                       ? (swprintf_s(value_buf, ARRAYSIZE(value_buf), L"%d%%", remaining), value_buf)
                                       : L"--";
            RECT value_rect{rect.left, rect.top + S(21, ui_scale), rect.left + S(82, ui_scale), rect.top + S(53, ui_scale)};
            DrawTextW(dc, value, -1, &value_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            SetTextColor(dc, p.soft_text);
            SelectObject(dc, fonts.title);
            RECT remain_rect{rect.left + S(84, ui_scale), rect.top + S(28, ui_scale), rect.left + S(124, ui_scale), rect.top + S(48, ui_scale)};
            DrawTextW(dc, L"剩余", -1, &remain_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            RECT bar{rect.left + S(126, ui_scale), rect.top + S(36, ui_scale), rect.right, rect.top + S(44, ui_scale)};
            DrawProgressBar(dc, bar, remaining, ui_scale);
        }

        HFONT MakeFont(int px, int weight, int ui_scale)
        {
            int font_px = std::max(S(px, ui_scale), px >= 20 ? 11 : 7);
            return CreateFontW(-font_px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        }

    } // namespace

    Palette DefaultPalette()
    {
        return Palette{RGB(245, 246, 248), RGB(255, 255, 255), RGB(224, 228, 235), RGB(23, 28, 38), RGB(0, 0, 0), RGB(106, 119, 137), RGB(78, 88, 104), RGB(248, 250, 252), RGB(238, 240, 244), RGB(231, 233, 238), RGB(34, 197, 94), RGB(239, 68, 68), RGB(37, 99, 235)};
    }

    int Scale(int value, int ui_scale)
    {
        return MulDiv(value, ui_scale, 100);
    }

    int DpiScaleValue(int value, UINT dpi)
    {
        return MulDiv(value, static_cast<int>(dpi), 96);
    }

    int ComputeEffectiveScale(int resolution_scale, UINT dpi, int compact_scale_percent)
    {
        return std::max(45, MulDiv(DpiScaleValue(resolution_scale, dpi), compact_scale_percent, 100));
    }

    void CreateUiFonts(UiFonts &fonts, int ui_scale)
    {
        DestroyUiFonts(fonts);
        fonts.title = MakeFont(12, FW_SEMIBOLD, ui_scale);
        fonts.value = MakeFont(23, FW_BOLD, ui_scale);
        fonts.small_font = MakeFont(10, FW_NORMAL, ui_scale);
    }

    void DestroyUiFonts(UiFonts &fonts)
    {
        if (fonts.title)
        {
            DeleteObject(fonts.title);
            fonts.title = nullptr;
        }
        if (fonts.value)
        {
            DeleteObject(fonts.value);
            fonts.value = nullptr;
        }
        if (fonts.small_font)
        {
            DeleteObject(fonts.small_font);
            fonts.small_font = nullptr;
        }
    }

    void ApplyWindowShape(HWND hwnd, int ui_scale, ShapeCache &shape)
    {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        int radius = S(18, ui_scale);
        if (width <= 0 || height <= 0)
            return;
        if (width == shape.width && height == shape.height && radius == shape.radius)
            return;

        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
        if (!region)
            return;
        if (SetWindowRgn(hwnd, region, TRUE))
        {
            shape.width = width;
            shape.height = height;
            shape.radius = radius;
        }
        else
        {
            DeleteObject(region);
        }
    }

    RECT RefreshButtonRect(const RECT &client, int ui_scale)
    {
        return RECT{client.right - S(56, ui_scale), S(8, ui_scale), client.right - S(14, ui_scale), S(30, ui_scale)};
    }

    bool PtInRectInclusive(const RECT &rect, POINT point)
    {
        return point.x >= rect.left && point.x <= rect.right && point.y >= rect.top && point.y <= rect.bottom;
    }

    void PaintUsageWindow(HWND hwnd, const UsageState &snapshot, const UiFonts &fonts, int ui_scale)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        Palette p = DefaultPalette();

        FillSolidRect(dc, client, p.bg);

        RECT panel{0, 0, client.right, client.bottom};
        FillRoundRect(dc, panel, p.panel, p.border, 18, ui_scale);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, p.title);
        SelectObject(dc, fonts.title);
        std::wstring header_title = L"Codex 限额";
        if (!snapshot.plan_display.empty())
        {
            header_title += L" (";
            header_title += snapshot.plan_display;
            header_title += L")";
        }
        RECT header{S(14, ui_scale), S(8, ui_scale), client.right - S(90, ui_scale), S(28, ui_scale)};
        DrawTextW(dc, header_title.c_str(), -1, &header, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT refresh = RefreshButtonRect(client, ui_scale);
        FillRoundRect(dc, refresh, p.button, p.border, 10, ui_scale);
        SetTextColor(dc, p.soft_text);
        SelectObject(dc, fonts.small_font);
        DrawTextW(dc, L"刷新", -1, &refresh, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        COLORREF dot_color = snapshot.ok ? p.ok : p.bad;
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, dot_color);
        SetDCPenColor(dc, dot_color);
        Ellipse(dc, client.right - S(70, ui_scale), S(15, ui_scale), client.right - S(62, ui_scale), S(23, ui_scale));
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);

        old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, p.divider);
        MoveToEx(dc, S(14, ui_scale), S(32, ui_scale), nullptr);
        LineTo(dc, client.right - S(14, ui_scale), S(32, ui_scale));
        MoveToEx(dc, S(14, ui_scale), S(88, ui_scale), nullptr);
        LineTo(dc, client.right - S(14, ui_scale), S(88, ui_scale));
        SelectObject(dc, old_pen);

        RECT primary{S(14, ui_scale), S(39, ui_scale), client.right - S(14, ui_scale), S(82, ui_scale)};
        RECT secondary{S(14, ui_scale), S(96, ui_scale), client.right - S(14, ui_scale), S(139, ui_scale)};
        DrawLimitRow(dc, primary, snapshot.primary, L"短窗口使用限额", fonts, ui_scale);
        DrawLimitRow(dc, secondary, snapshot.secondary, L"长窗口使用限额", fonts, ui_scale);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, snapshot.ok ? p.muted : p.bad);
        SelectObject(dc, fonts.small_font);
        RECT status{S(14, ui_scale), client.bottom - S(20, ui_scale), client.right - S(14, ui_scale), client.bottom - S(4, ui_scale)};
        DrawTextW(dc, StatusText(snapshot.status), -1, &status, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        EndPaint(hwnd, &ps);
    }

} // namespace codexlimit
