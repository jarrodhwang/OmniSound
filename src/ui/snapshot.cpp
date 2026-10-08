#include "snapshot.hpp"
#include <gdiplus.h>
void save_ui_snapshot(HWND window, const std::filesystem::path &path)
{
    RECT rect;
    GetWindowRect(window, &rect);
    HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
    auto previous = SelectObject(memory, bitmap);
    BOOL rendered = PrintWindow(window, memory, 2);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    if (!rendered)
    {
        DeleteObject(bitmap);
        throw std::runtime_error("UI snapshot failed");
    }
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
    {
        DeleteObject(bitmap);
        throw std::runtime_error("Image encoder initialization failed");
    }
    Gdiplus::Status status;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        status = image.Save(path.c_str(), &png, nullptr);
    }
    Gdiplus::GdiplusShutdown(token);
    DeleteObject(bitmap);
    if (status != Gdiplus::Ok)
        throw std::runtime_error("UI snapshot could not be saved");
}
