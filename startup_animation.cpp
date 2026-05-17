#define NOMINMAX

#include "startup_animation.h"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#pragma comment(lib, "gdiplus.lib")

namespace codexlimit::startup_animation
{
    namespace
    {
        constexpr UINT_PTR kAnimationTimer = 1;
        constexpr UINT kFrameIntervalMs = 16;
        constexpr std::uint64_t kDurationMs = 620;
        constexpr const wchar_t *kWindowClassName = L"CodexLimitStartupAnimationWindow";

        HWND g_hwnd = nullptr;
        POINT g_start{};
        POINT g_end{};
        COLORREF g_color = RGB(37, 99, 235);
        float g_max_thickness = 2.0f;
        std::uint64_t g_started_at = 0;
        bool g_registered = false;

        ULONG_PTR g_gdiplus_token = 0;
        HDC g_frame_dc = nullptr;
        HBITMAP g_frame_bitmap = nullptr;
        HGDIOBJ g_old_bitmap = nullptr;
        void *g_frame_bits = nullptr;
        SIZE g_frame_size{};

        std::uint64_t NowMs()
        {
            return GetTickCount64();
        }

        float Clamp01(float value)
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        float SmootherStep(float value)
        {
            float t = Clamp01(value);
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        Gdiplus::Color ToGdiColor(BYTE alpha, COLORREF color)
        {
            return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
        }

        void FillTaperedStroke(Gdiplus::Graphics &graphics, float x, float y, float head_thickness, BYTE alpha)
        {
            float dx = x - static_cast<float>(g_start.x);
            float dy = y - static_cast<float>(g_start.y);
            float length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
            float nx = -dy / length;
            float ny = dx / length;
            float tail_half = 0.75f;
            float head_half = head_thickness * 0.5f;

            Gdiplus::PointF points[]{
                {static_cast<Gdiplus::REAL>(g_start.x + nx * tail_half), static_cast<Gdiplus::REAL>(g_start.y + ny * tail_half)},
                {static_cast<Gdiplus::REAL>(x + nx * head_half), static_cast<Gdiplus::REAL>(y + ny * head_half)},
                {static_cast<Gdiplus::REAL>(x - nx * head_half), static_cast<Gdiplus::REAL>(y - ny * head_half)},
                {static_cast<Gdiplus::REAL>(g_start.x - nx * tail_half), static_cast<Gdiplus::REAL>(g_start.y - ny * tail_half)},
            };

            Gdiplus::GraphicsPath path;
            path.AddPolygon(points, ARRAYSIZE(points));
            Gdiplus::SolidBrush brush(ToGdiColor(alpha, g_color));
            graphics.FillPath(&brush, &path);
        }

        void CleanupFrameBuffer()
        {
            if (g_frame_dc && g_old_bitmap)
            {
                SelectObject(g_frame_dc, g_old_bitmap);
                g_old_bitmap = nullptr;
            }
            if (g_frame_bitmap)
            {
                DeleteObject(g_frame_bitmap);
                g_frame_bitmap = nullptr;
            }
            if (g_frame_dc)
            {
                DeleteDC(g_frame_dc);
                g_frame_dc = nullptr;
            }
            g_frame_bits = nullptr;
            g_frame_size = {};
        }

        bool EnsureGdiplus()
        {
            if (g_gdiplus_token != 0)
                return true;

            Gdiplus::GdiplusStartupInput input{};
            return Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr) == Gdiplus::Ok;
        }

        void ShutdownGdiplus()
        {
            if (g_gdiplus_token != 0)
            {
                Gdiplus::GdiplusShutdown(g_gdiplus_token);
                g_gdiplus_token = 0;
            }
        }

        bool EnsureFrameBuffer(HDC screen_dc, int width, int height)
        {
            if (width <= 0 || height <= 0)
                return false;

            if (g_frame_dc && g_frame_bitmap && g_frame_size.cx == width && g_frame_size.cy == height)
                return true;

            CleanupFrameBuffer();

            BITMAPINFO bitmap_info{};
            bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
            bitmap_info.bmiHeader.biWidth = width;
            bitmap_info.bmiHeader.biHeight = -height;
            bitmap_info.bmiHeader.biPlanes = 1;
            bitmap_info.bmiHeader.biBitCount = 32;
            bitmap_info.bmiHeader.biCompression = BI_RGB;

            g_frame_dc = CreateCompatibleDC(screen_dc);
            if (!g_frame_dc)
                return false;

            g_frame_bitmap = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &g_frame_bits, nullptr, 0);
            if (!g_frame_bitmap)
            {
                CleanupFrameBuffer();
                return false;
            }

            g_old_bitmap = SelectObject(g_frame_dc, g_frame_bitmap);
            g_frame_size = {width, height};
            return true;
        }

        void RenderFrame(HWND hwnd)
        {
            RECT window_rect{};
            GetWindowRect(hwnd, &window_rect);
            int width = window_rect.right - window_rect.left;
            int height = window_rect.bottom - window_rect.top;
            HDC screen_dc = GetDC(nullptr);
            if (!screen_dc)
                return;

            if (!EnsureFrameBuffer(screen_dc, width, height))
            {
                ReleaseDC(nullptr, screen_dc);
                return;
            }

            std::fill_n(static_cast<std::uint32_t *>(g_frame_bits), static_cast<size_t>(width) * height, 0);

            float raw_progress = static_cast<float>(NowMs() - g_started_at) / static_cast<float>(kDurationMs);
            float progress = Clamp01(raw_progress);
            float travel = SmootherStep(raw_progress);
            float x = static_cast<float>(g_start.x) + (static_cast<float>(g_end.x - g_start.x) * travel);
            float y = static_cast<float>(g_start.y) + (static_cast<float>(g_end.y - g_start.y) * travel);
            float thickness = 1.5f + (g_max_thickness - 1.5f) * progress;

            Gdiplus::Graphics graphics(g_frame_dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

            FillTaperedStroke(graphics, x, y, thickness * 1.35f, 45);
            FillTaperedStroke(graphics, x, y, thickness, 235);

            POINT dst{window_rect.left, window_rect.top};
            POINT src{};
            SIZE size{width, height};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(hwnd, screen_dc, &dst, &size, g_frame_dc, &src, 0, &blend, ULW_ALPHA);
            ReleaseDC(nullptr, screen_dc);
        }

        void DestroyAnimationWindow()
        {
            if (g_hwnd)
                DestroyWindow(g_hwnd);
        }

        LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            switch (msg)
            {
            case WM_CREATE:
                g_started_at = NowMs();
                SetTimer(hwnd, kAnimationTimer, kFrameIntervalMs, nullptr);
                RenderFrame(hwnd);
                return 0;
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_TIMER:
                if (wparam == kAnimationTimer)
                {
                    RenderFrame(hwnd);
                    if (NowMs() - g_started_at >= kDurationMs)
                        DestroyWindow(hwnd);
                    return 0;
                }
                break;
            case WM_DESTROY:
                KillTimer(hwnd, kAnimationTimer);
                if (g_hwnd == hwnd)
                    g_hwnd = nullptr;
                CleanupFrameBuffer();
                ShutdownGdiplus();
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        bool RegisterAnimationClass(HINSTANCE instance)
        {
            if (g_registered)
                return true;

            WNDCLASSW wc{};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = instance;
            wc.lpszClassName = kWindowClassName;
            g_registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            return g_registered;
        }
    }

    void Start(HINSTANCE instance, const RECT &target, COLORREF color)
    {
        DestroyAnimationWindow();

        if (!EnsureGdiplus())
            return;

        if (!RegisterAnimationClass(instance))
        {
            ShutdownGdiplus();
            return;
        }

        POINT from{};
        GetCursorPos(&from);
        POINT to{(target.left + target.right) / 2, (target.top + target.bottom) / 2};
        int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_max_thickness = std::max(2.0f, static_cast<float>(virtual_height) * 0.03f);
        int padding = std::max(8, static_cast<int>(std::ceil(g_max_thickness * 0.75f)) + 4);
        int left = std::min(from.x, to.x) - padding;
        int top = std::min(from.y, to.y) - padding;
        int right = std::max(from.x, to.x) + padding;
        int bottom = std::max(from.y, to.y) + padding;
        int width = std::max(1, right - left);
        int height = std::max(1, bottom - top);

        g_start = {from.x - left, from.y - top};
        g_end = {to.x - left, to.y - top};
        g_color = color;

        g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
                                 kWindowClassName, L"", WS_POPUP, left, top, width, height,
                                 nullptr, nullptr, instance, nullptr);
        if (!g_hwnd)
        {
            ShutdownGdiplus();
            return;
        }

        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        RenderFrame(g_hwnd);
    }

}
