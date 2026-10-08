#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

class DesktopShell
{
  public:
    using Groups = std::vector<std::pair<std::wstring, bool>>;
    DesktopShell(HWND, bool tray, std::function<void(unsigned)> action, std::function<Groups()> groups,
                 std::function<bool()> startup);
    ~DesktopShell();
    void show();
    void hide();
    void remove_tray();
    void tooltip(const std::wstring &);
    void accent(COLORREF);
    bool has_tray() const
    {
        return tray_added_;
    }
    static UINT restore_message();

  private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void add_tray();
    void menu();
    HWND window_{};
    HICON icon_{};
    COLORREF accent_colour_{CLR_INVALID};
    bool tray_added_{}, tray_enabled_{};
    UINT taskbar_created_{};
    std::wstring tooltip_{L"OmniSound"};
    std::function<void(unsigned)> action_;
    std::function<Groups()> groups_;
    std::function<bool()> startup_;
};
