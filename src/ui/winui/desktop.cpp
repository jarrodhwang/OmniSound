#include "pch.h"
#include "desktop.hpp"
#include "localization.hpp"
#include <commctrl.h>
#include <shellapi.h>

namespace
{
constexpr UINT tray_message = WM_APP + 71;
HICON make_icon(COLORREF accent)
{
    constexpr int size = 64;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void *raw{};
    const auto dc = GetDC(nullptr);
    auto bitmap =
        CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &raw, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bitmap)
        return nullptr;
    auto pixels = static_cast<unsigned long *>(raw);
    constexpr int heights[]{12, 24, 38, 24, 12};
    const auto fill = 0xff000000UL | (GetRValue(accent) << 16) | (GetGValue(accent) << 8) | GetBValue(accent);
    const auto ink = GetRValue(accent) * 299 + GetGValue(accent) * 587 + GetBValue(accent) * 114 > 128000
                         ? 0xff000000UL
                         : 0xffffffffUL;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const int dx = x - 32, dy = y - 32;
            auto color = dx * dx + dy * dy < 30 * 30 ? fill : 0UL;
            for (int bar = 0; bar < 5; ++bar)
                if (x >= 15 + bar * 7 && x < 19 + bar * 7 && y >= 32 - heights[bar] / 2 &&
                    y < 32 + heights[bar] / 2)
                    color = ink;
            pixels[y * size + x] = color;
        }
    auto mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, bitmap};
    auto icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(bitmap);
    return icon;
}
} // namespace
UINT DesktopShell::restore_message()
{
    return RegisterWindowMessageW(L"OmniSound.Restore.v1");
}
DesktopShell::DesktopShell(HWND window, bool tray, std::function<void(unsigned)> action,
                           std::function<Groups()> groups, std::function<bool()> startup)
    : window_(window), tray_enabled_(tray), action_(std::move(action)), groups_(std::move(groups)),
      startup_(std::move(startup))
{
    accent_colour_ = GetSysColor(COLOR_HIGHLIGHT);
    icon_ = make_icon(accent_colour_);
    SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    SetWindowSubclass(window_, window_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    if (tray_enabled_)
        add_tray();
}
DesktopShell::~DesktopShell()
{
    remove_tray();
    RemoveWindowSubclass(window_, window_proc, 1);
    if (icon_)
        DestroyIcon(icon_);
}
void DesktopShell::add_tray()
{
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = tray_message;
    data.hIcon = icon_;
    wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (tray_added_)
    {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}
void DesktopShell::remove_tray()
{
    if (tray_added_)
    {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added_ = false;
    }
    tray_enabled_ = false;
}
void DesktopShell::tooltip(const std::wstring &value)
{
    if (value == tooltip_)
        return;
    tooltip_ = value;
    if (tray_added_)
    {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = 1;
        data.uFlags = NIF_TIP;
        wcsncpy_s(data.szTip, value.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}
void DesktopShell::accent(COLORREF colour)
{
    if (colour == accent_colour_)
        return;
    auto icon = make_icon(colour);
    if (!icon)
        return;
    auto previous = icon_;
    icon_ = icon;
    accent_colour_ = colour;
    SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
    if (tray_added_)
    {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = 1;
        data.uFlags = NIF_ICON;
        data.hIcon = icon_;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (previous)
        DestroyIcon(previous);
}
void DesktopShell::show()
{
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
}
void DesktopShell::hide()
{
    if (tray_added_)
        ShowWindow(window_, SW_HIDE);
}
void DesktopShell::menu()
{
    auto popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING, 1, T(L"OmniSound 열기").c_str());
    const auto groups = groups_();
    if (!groups.empty())
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < groups.size(); ++i)
        AppendMenuW(popup, MF_STRING | (groups[i].second ? MF_CHECKED : 0), 100 + i, groups[i].first.c_str());
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING | (startup_() ? MF_CHECKED : 0), 2, T(L"Windows 로그인 시 실행").c_str());
    AppendMenuW(popup, MF_STRING, 3, T(L"Windows 소리 설정").c_str());
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, 4, T(L"OmniSound 종료").c_str());
    POINT position{};
    GetCursorPos(&position);
    SetForegroundWindow(window_);
    auto command =
        TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, 0, window_, nullptr);
    DestroyMenu(popup);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == 1)
        show();
    else if (command)
        action_(command);
}
LRESULT CALLBACK DesktopShell::window_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                                           DWORD_PTR ref)
{
    auto self = reinterpret_cast<DesktopShell *>(ref);
    try
    {
        if (message == WM_GETMINMAXINFO)
        {
            const auto result = DefSubclassProc(window, message, w, l);
            auto limits = reinterpret_cast<MINMAXINFO *>(l);
            const auto dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize = {MulDiv(820, dpi, 96), MulDiv(620, dpi, 96)};
            return result;
        }
        if (message == WM_CLOSE && self->tray_added_)
        {
            self->hide();
            return 0;
        }
        if (message == restore_message())
        {
            self->show();
            return 0;
        }
        if (message == self->taskbar_created_ && self->tray_enabled_)
        {
            self->add_tray();
            return 0;
        }
        if (message == tray_message)
        {
            if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT || LOWORD(l) == WM_LBUTTONDBLCLK)
                self->show();
            if (LOWORD(l) == WM_CONTEXTMENU)
                self->menu();
            return 0;
        }
    }
    catch (...)
    { /* Native callbacks must never unwind through User32. */
    }
    return DefSubclassProc(window, message, w, l);
}
