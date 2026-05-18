#pragma once

#define NOMINMAX

#include "app_types.h"

#include <windows.h>

namespace codexlimit
{

struct Palette
{
    COLORREF bg, panel, border, title, text, muted, soft_text, button, divider, bar, ok, bad, guide;
};

struct UiFonts
{
    HFONT title = nullptr;
    HFONT value = nullptr;
    HFONT small_font = nullptr;
};

struct ShapeCache
{
    int width = -1;
    int height = -1;
    int radius = -1;
};

Palette DefaultPalette();
int Scale(int value, int ui_scale);
int DpiScaleValue(int value, UINT dpi);
int ComputeEffectiveScale(int resolution_scale, UINT dpi, int compact_scale_percent);
void CreateUiFonts(UiFonts &fonts, int ui_scale);
void DestroyUiFonts(UiFonts &fonts);
void ApplyWindowShape(HWND hwnd, int ui_scale, ShapeCache &shape);
RECT RefreshButtonRect(const RECT &client, int ui_scale);
bool PtInRectInclusive(const RECT &rect, POINT point);
void PaintUsageWindow(HWND hwnd, const UsageState &snapshot, const UiFonts &fonts, int ui_scale);

} // namespace codexlimit
