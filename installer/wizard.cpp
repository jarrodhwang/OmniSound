#include "wizard.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <fstream>
#include <thread>
#include <wincodec.h>

namespace omni::setup
{
namespace
{
constexpr UINT progress_message = WM_APP + 1, result_message = WM_APP + 2;
constexpr int next_id = 101, back_id = 102, cancel_id = 103, lite_id = 104, driver_id = 105, accept_id = 106,
              browse_id = 107, log_id = 108;
constexpr COLORREF ink = RGB(25, 36, 55), accent = RGB(39, 98, 230), muted = RGB(92, 106, 128);
struct ProgressUpdate
{
    int percent;
    std::wstring text;
};

std::wstring text(HWND control)
{
    std::wstring value(GetWindowTextLengthW(control) + 1, L'\0');
    GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    return value;
}
std::wstring document(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return L"패키지에 사용권 문서가 없습니다. 전체 설치 ZIP을 먼저 압축 해제하세요.";
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    auto value = wide(bytes);
    std::wstring normalized;
    for (auto c : value)
    {
        if (c == L'\n')
            normalized += L'\r';
        normalized += c;
    }
    return normalized;
}
bool signed_driver_available()
{
    auto manifest = read_json(executable_dir() / L"manifest.json");
    return manifest.value("flavor", std::string{}) == "release" &&
           std::filesystem::exists(executable_dir() / L"driver" / L"OmniSound.cat");
}

class Wizard
{
    HINSTANCE module_;
    HWND window_{}, next_{}, back_{}, cancel_{}, editor_{}, agreement_{}, launch_{}, progress_{};
    std::vector<HWND> controls_;
    HFONT font_{}, heading_{}, brand_{};
    HBRUSH white_ = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH background_ = CreateSolidBrush(RGB(247, 249, 252));
    Options options_;
    Result result_;
    std::thread worker_;
    int page_ = 0, dpi_ = 96;
    bool accepted_ = false, started_ = false, driver_enabled_ = false, existing_ = false;
    std::wstring notice_;

    int px(int value) const
    {
        return MulDiv(value, dpi_, 96);
    }
    HFONT make_font(int size, int weight)
    {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                           L"Malgun Gothic");
    }
    HWND control(const wchar_t *type, std::wstring value, DWORD style, int x, int y, int width, int height,
                 int id = 0, bool heading = false)
    {
        auto handle =
            CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, value.c_str(),
                            WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(width), px(height), window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module_, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(heading ? heading_ : font_), TRUE);
        SetWindowTheme(handle, L"Explorer", nullptr);
        controls_.push_back(handle);
        return handle;
    }
    void label(std::wstring value, int y, int height = 48, bool title = false)
    {
        control(L"STATIC", std::move(value), SS_LEFT, 250, y, 555, height, 0, title);
    }
    void buttons(std::wstring next, bool can_back, bool can_next = true)
    {
        back_ = control(L"BUTTON", L"이전", WS_TABSTOP | BS_PUSHBUTTON, 524, 538, 82, 38, back_id);
        next_ = control(L"BUTTON", std::move(next), WS_TABSTOP | BS_OWNERDRAW, 616, 538, 96, 38, next_id);
        cancel_ = control(L"BUTTON", page_ == 5 ? L"닫기" : L"취소", WS_TABSTOP | BS_PUSHBUTTON, 722, 538, 82,
                          38, cancel_id);
        EnableWindow(back_, can_back);
        EnableWindow(next_, can_next);
    }
    void select_mode(bool lite)
    {
        if (options_.lite == lite)
            return;
        options_.lite = lite;
        options_.target = install_location(lite);
        existing_ = std::filesystem::exists(options_.target / L"OmniSound.exe");
    }
    void page()
    {
        for (auto handle : controls_)
            DestroyWindow(handle);
        controls_.clear();
        editor_ = agreement_ = launch_ = progress_ = nullptr;
        notice_.clear();
        if (options_.uninstall)
        {
            if (page_ == 0)
            {
                label(L"OmniSound 제거", 52, 40, true);
                label(L"앱과 백그라운드 실행 구성을 제거합니다.\r\n저장된 그룹과 보정 설정은 보관합니다.",
                      116, 76);
                label((options_.lite ? L"OmniSound Lite\r\n" : L"OmniSound Pro\r\n") +
                          options_.target.wstring(),
                      222, 105);
                buttons(L"제거", false);
            }
        }
        else if (page_ == 0)
        {
            label(L"OmniSound 설치", 52, 40, true);
            label(L"사용할 출력 방식을 선택하세요.", 111, 32);
            auto lite = control(L"BUTTON", L"OmniSound Lite", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 250,
                                176, 555, 35, lite_id);
            control(L"STATIC",
                    L"드라이버 없이 Windows 기본 출력의 소리를 추가 기기로 복제합니다.\r\n현재 사용자 계정에 "
                    L"설치합니다.",
                    SS_LEFT, 278, 216, 515, 70);
            auto driver = control(L"BUTTON", L"OmniSound Pro", BS_AUTORADIOBUTTON | WS_TABSTOP, 250, 313,
                                  555, 35, driver_id);
            control(L"STATIC",
                    L"Windows 소리 출력 목록에 그룹 장치를 추가합니다.\r\n관리자 권한과 Microsoft 서명 "
                    L"드라이버가 필요합니다.",
                    SS_LEFT, 278, 353, 515, 70);
            EnableWindow(driver, driver_enabled_);
            SendMessageW(options_.lite ? lite : driver, BM_SETCHECK, BST_CHECKED, 0);
            label(driver_enabled_ ? L"앱 버전 0.2.0 · Windows 11 x64"
                                  : L"이 개발 패키지에서는 Lite를 설치할 수 있습니다.\r\nDriver는 Microsoft "
                                    L"서명 패키지가 준비되면 활성화됩니다.",
                  448, 65);
            buttons(L"다음", false);
        }
        else if (page_ == 1)
        {
            label(L"사용권 및 제3자 고지", 52, 40, true);
            label(L"설치 전에 포함된 사용권 문서를 확인하세요.", 108, 35);
            editor_ = control(
                L"EDIT",
                document(executable_dir() / L"LICENSE") + L"\r\n\r\n— THIRD-PARTY NOTICES —\r\n\r\n" +
                    document(executable_dir() / L"THIRD_PARTY_NOTICES.md"),
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 250, 156, 555, 296);
            agreement_ = control(L"BUTTON", L"위 사용권 및 제3자 고지를 확인하고 동의합니다.",
                                 BS_AUTOCHECKBOX | WS_TABSTOP, 250, 474, 555, 32, accept_id);
            SendMessageW(agreement_, BM_SETCHECK, accepted_ ? BST_CHECKED : BST_UNCHECKED, 0);
            buttons(L"다음", true, accepted_);
        }
        else if (page_ == 2)
        {
            label(existing_ ? L"기존 설치 업데이트 / 복구" : L"설치 위치", 52, 40, true);
            label(existing_ ? L"기존 OmniSound 설치를 찾았습니다.\r\n현재 설치 위치에서 파일을 교체하고 "
                              L"설정을 유지합니다."
                            : L"프로그램 파일을 저장할 폴더를 선택하세요.",
                  110, 78);
            label(options_.lite ? L"현재 사용자 · 관리자 권한 필요 없음" : L"모든 사용자 · 관리자 권한 필요",
                  224, 30);
            editor_ =
                control(L"EDIT", options_.target.wstring(), ES_AUTOHSCROLL | WS_TABSTOP, 250, 268, 442, 36);
            auto browse =
                control(L"BUTTON", L"찾아보기…", BS_PUSHBUTTON | WS_TABSTOP, 702, 268, 103, 36, browse_id);
            EnableWindow(editor_, !existing_);
            EnableWindow(browse, !existing_);
            label(existing_ ? L"설치 위치를 변경하려면 기존 버전을 먼저 제거하세요.\r\n그룹 설정은 제거 "
                              L"후에도 보관됩니다."
                  : options_.lite
                      ? L"사용자 Programs 폴더 아래에 전용 폴더를 지정하세요.\r\n설정은 프로그램 파일과 "
                        L"별도로 보관됩니다."
                      : L"Program Files 아래에 전용 폴더를 지정하세요.\r\n선택한 폴더는 관리자가 보호합니다.",
                  343, 90);
            buttons(L"다음", true);
        }
        else if (page_ == 3)
        {
            label(existing_ ? L"업데이트 준비 완료" : L"설치 준비 완료", 52, 40, true);
            label(options_.lite ? L"OmniSound Lite" : L"OmniSound Pro", 117, 35, true);
            label(L"설치 위치\r\n" + options_.target.wstring(), 190, 90);
            label(L"시작 메뉴 바로가기\r\n로그온 시 오디오 엔진 자동 실행\r\n그룹 및 보정 설정 유지", 315,
                  98);
            label(existing_ ? L"설치 전에 앱의 편집 내용을 저장하세요.\r\n업데이트 중 앱과 엔진을 종료합니다."
                  : options_.lite ? L"드라이버나 관리자 서비스를 설치하지 않습니다."
                                  : L"설치를 시작하면 Windows 관리자 권한을 요청합니다.",
                  461, 40);
            buttons(existing_ ? L"업데이트" : L"설치", true);
        }
        if (page_ == 4)
        {
            label(options_.uninstall ? L"OmniSound 제거 중" : L"OmniSound 설치 중", 52, 40, true);
            label(L"완료될 때까지 설치 창을 열어 두세요.", 115, 42);
            progress_ = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 250, 211, 555, 12);
            SendMessageW(progress_, PBM_SETRANGE32, 0, 100);
            editor_ = control(L"STATIC", L"준비 중…", SS_LEFT, 250, 247, 555, 76);
            label(options_.target.wstring(), 380, 94);
            buttons(L"설치 중", false, false);
            EnableWindow(cancel_, FALSE);
        }
        else if (page_ == 5)
        {
            label(result_.code         ? L"설치를 완료하지 못했습니다"
                  : options_.uninstall ? L"제거 완료"
                                       : L"설치 완료",
                  52, 64, true);
            editor_ =
                control(result_.code ? L"EDIT" : L"STATIC", result_.message,
                        result_.code ? ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP
                                     : SS_LEFT,
                        250, 151, 555, 237);
            if (!result_.code && !options_.uninstall)
            {
                launch_ = control(L"BUTTON", L"마침을 누르면 OmniSound 실행", BS_AUTOCHECKBOX | WS_TABSTOP,
                                  250, 414, 555, 32);
                SendMessageW(launch_, BM_SETCHECK, BST_CHECKED, 0);
            }
            if (!result_.log.empty())
                control(L"BUTTON", L"설치 로그 열기", BS_PUSHBUTTON | WS_TABSTOP, 250, 466, 144, 34, log_id);
            buttons(result_.code ? L"다시 시도" : L"마침", false);
        }
        InvalidateRect(window_, nullptr, TRUE);
        SetFocus(page_ == 1 ? agreement_ : next_);
    }
    bool capture_path()
    {
        auto value = text(editor_);
        std::filesystem::path path(value);
        auto root =
            options_.lite ? install_location(true).parent_path() : install_location(false).parent_path();
        const auto prefix = root.wstring() + L"\\";
        const auto normalized = std::filesystem::absolute(path).lexically_normal().wstring();
        if (!path.is_absolute() || value.starts_with(L"\\\\") || normalized.size() <= prefix.size() ||
            CompareStringOrdinal(normalized.c_str(), static_cast<int>(prefix.size()), prefix.c_str(),
                                 static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
        {
            notice_ = L"설치 경로는 " + root.wstring() + L" 아래의 전용 폴더여야 합니다.";
            InvalidateRect(window_, nullptr, TRUE);
            return false;
        }
        options_.target = normalized;
        return true;
    }
    void browse()
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"OmniSound 설치 폴더 선택");
        Microsoft::WRL::ComPtr<IShellItem> initial;
        if (SUCCEEDED(SHCreateItemFromParsingName(options_.target.parent_path().c_str(), nullptr,
                                                  IID_PPV_ARGS(&initial))))
            dialog->SetFolder(initial.Get());
        if (SUCCEEDED(dialog->Show(window_)))
        {
            Microsoft::WRL::ComPtr<IShellItem> item;
            PWSTR path{};
            if (SUCCEEDED(dialog->GetResult(&item)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                SetWindowTextW(editor_, path);
                CoTaskMemFree(path);
            }
        }
    }
    void start()
    {
        options_.accepted = accepted_ || options_.uninstall;
        if (!options_.lite && !IsUserAnAdmin())
        {
            auto arguments = (options_.local_test ? L"--local-test-" : L"--driver-") +
                             std::wstring(options_.uninstall ? L"uninstall" : L"install");
            arguments +=
                L" --provision --accept-license --install-dir \"" + options_.target.wstring() + L"\"";
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_NOCLOSEPROCESS;
            info.hwnd = window_;
            info.lpVerb = L"runas";
            const auto exe = executable_dir() / L"OmniSoundSetup.exe";
            info.lpFile = exe.c_str();
            info.lpParameters = arguments.c_str();
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info))
            {
                notice_ = L"관리자 권한 요청이 취소되었습니다. 다시 설치를 시작할 수 있습니다.";
                InvalidateRect(window_, nullptr, TRUE);
                return;
            }
            if (info.hProcess)
                CloseHandle(info.hProcess);
            result_.code = 0;
            DestroyWindow(window_);
            return;
        }
        started_ = true;
        page_ = 4;
        page();
        if (worker_.joinable())
            worker_.join();
        const auto options = options_;
        worker_ = std::thread([this, options] {
            auto result = provision(module_, options, [this](int percent, std::wstring value) {
                auto update = new ProgressUpdate{percent, std::move(value)};
                if (!PostMessageW(window_, progress_message, 0, reinterpret_cast<LPARAM>(update)))
                    delete update;
            });
            auto message = new Result(std::move(result));
            if (!PostMessageW(window_, result_message, 0, reinterpret_cast<LPARAM>(message)))
                delete message;
        });
    }
    void draw(HDC dc)
    {
        RECT client;
        GetClientRect(window_, &client);
        FillRect(dc, &client, background_);
        RECT sidebar{0, 0, px(216), px(600)};
        auto navy = CreateSolidBrush(RGB(17, 31, 54));
        FillRect(dc, &sidebar, navy);
        DeleteObject(navy);
        RECT footer{px(216), px(521), px(840), px(600)};
        FillRect(dc, &footer, white_);
        SetBkMode(dc, TRANSPARENT);
        auto draw = [&](std::wstring value, int x, int y, int width, int height, COLORREF color, HFONT font) {
            SelectObject(dc, font);
            SetTextColor(dc, color);
            RECT rect{px(x), px(y), px(x + width), px(y + height)};
            DrawTextW(dc, value.c_str(), -1, &rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        };
        draw(L"OmniSound", 24, 39, 180, 40, RGB(255, 255, 255), brand_);
        draw(L"SETUP  /  0.2.0", 25, 88, 175, 25, RGB(157, 182, 218), font_);
        const wchar_t *steps[] = {L"01   출력 방식", L"02   사용권 동의", L"03   설치 위치",
                                  L"04   설치 확인", L"05   설치 진행",   L"06   결과"};
        for (int i = 0; i < 6; ++i)
            draw(steps[i], 25, 177 + i * 48, 178, 35, i == page_ ? RGB(255, 255, 255) : RGB(146, 166, 195),
                 font_);
        draw(options_.lite ? L"LITE\r\n현재 사용자 설치" : L"DRIVER\r\n모든 사용자 설치", 25, 483, 174, 70,
             RGB(157, 182, 218), font_);
        if (!notice_.empty())
            draw(notice_, 250, 491, 550, 28, RGB(185, 50, 50), font_);
    }
    void snapshot(const wchar_t *name)
    {
        if (options_.snapshots.empty())
            return;
        std::filesystem::create_directories(options_.snapshots);
        UpdateWindow(window_);
        RECT bounds;
        GetClientRect(window_, &bounds);
        auto source = GetDC(window_);
        auto dc = CreateCompatibleDC(source);
        auto bitmap = CreateCompatibleBitmap(source, bounds.right, bounds.bottom);
        auto previous = SelectObject(dc, bitmap);
        draw(dc);
        for (auto child : controls_)
        {
            RECT rectangle;
            GetWindowRect(child, &rectangle);
            MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT *>(&rectangle), 2);
            auto saved = SaveDC(dc);
            SetViewportOrgEx(dc, rectangle.left, rectangle.top, nullptr);
            SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                         PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
            RestoreDC(dc, saved);
        }
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        check(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "Create screenshot encoder");
        Microsoft::WRL::ComPtr<IWICBitmap> image;
        check(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image),
              "Capture wizard image");
        Microsoft::WRL::ComPtr<IWICStream> stream;
        check(factory->CreateStream(&stream), "Create image stream");
        check(stream->InitializeFromFilename((options_.snapshots / (std::wstring(name) + L".png")).c_str(),
                                             GENERIC_WRITE),
              "Open wizard screenshot");
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        check(encoder->CreateNewFrame(&frame, nullptr), "Create screenshot frame");
        check(frame->Initialize(nullptr), "Initialize screenshot frame");
        check(frame->WriteSource(image.Get(), nullptr), "Write wizard image");
        check(frame->Commit(), "Commit image frame");
        check(encoder->Commit(), "Commit wizard screenshot");
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(window_, source);
    }
    int smoke()
    {
        auto require = [](bool condition) {
            if (!condition)
                throw std::runtime_error("Installer wizard regression failed");
        };
        snapshot(L"01-mode");
        message(WM_COMMAND, next_id, 0);
        require(page_ == 1 && !IsWindowEnabled(next_));
        snapshot(L"02-agreement");
        SendMessageW(agreement_, BM_SETCHECK, BST_CHECKED, 0);
        message(WM_COMMAND, accept_id, 0);
        require(IsWindowEnabled(next_));
        message(WM_COMMAND, next_id, 0);
        require(page_ == 2);
        snapshot(L"03-existing-install");
        const auto installed = options_.target;
        existing_ = false;
        page();
        SetWindowTextW(editor_, (installed.parent_path() / L"OmniSound-Preview").c_str());
        require(capture_path());
        const auto chosen = options_.target;
        snapshot(L"03-path");
        message(WM_COMMAND, back_id, 0);
        require(page_ == 1 && accepted_);
        message(WM_COMMAND, next_id, 0);
        require(page_ == 2 && text(editor_) == chosen.wstring());
        message(WM_COMMAND, next_id, 0);
        require(page_ == 3);
        snapshot(L"04-summary");
        page_ = 4;
        page();
        require(!IsWindowEnabled(cancel_) && !IsWindowEnabled(next_));
        snapshot(L"05-progress");
        result_ = {0, L"OmniSound Lite 설치를 완료했습니다.\r\n\r\n설치 위치\r\n" + installed.wstring(), {}};
        page_ = 5;
        page();
        require(launch_ && IsWindowEnabled(next_));
        snapshot(L"06-complete");
        result_.code = 1;
        result_.message = L"실행 중인 앱 파일을 교체하지 못했습니다.\r\n앱을 종료한 뒤 다시 "
                          L"시도하세요.\r\n\r\n이전 설치 파일을 복원했습니다.";
        page();
        snapshot(L"07-error");
        message(WM_COMMAND, next_id, 0);
        require(page_ == 3);
        result_.code = 0;
        DestroyWindow(window_);
        return 0;
    }
    LRESULT message(UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(window_, &ps);
            draw(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            draw(reinterpret_cast<HDC>(w));
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), ink);
            SetBkColor(reinterpret_cast<HDC>(w), RGB(247, 249, 252));
            return reinterpret_cast<LRESULT>(background_);
        case WM_DRAWITEM: {
            auto item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
            bool enabled = IsWindowEnabled(item->hwndItem);
            auto brush = CreateSolidBrush(enabled ? accent : RGB(190, 200, 218));
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, RGB(255, 255, 255));
            SelectObject(item->hDC, font_);
            auto value = text(item->hwndItem);
            DrawTextW(item->hDC, value.c_str(), -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (item->itemState & ODS_FOCUS)
            {
                auto rect = item->rcItem;
                InflateRect(&rect, -4, -4);
                DrawFocusRect(item->hDC, &rect);
            }
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case lite_id:
                select_mode(true);
                break;
            case driver_id:
                select_mode(false);
                break;
            case accept_id:
                accepted_ = SendMessageW(agreement_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                EnableWindow(next_, accepted_);
                break;
            case browse_id:
                browse();
                break;
            case back_id:
                if (page_ == 2)
                    options_.target = text(editor_);
                --page_;
                page();
                break;
            case next_id:
                if (page_ == 5)
                {
                    if (result_.code)
                    {
                        page_ = options_.uninstall ? 0 : 3;
                        page();
                    }
                    else
                    {
                        if (launch_ && SendMessageW(launch_, BM_GETCHECK, 0, 0) == BST_CHECKED)
                            ShellExecuteW(window_, L"open", (options_.target / L"OmniSound.exe").c_str(),
                                          nullptr, options_.target.c_str(), SW_SHOWNORMAL);
                        DestroyWindow(window_);
                    }
                }
                else if (options_.uninstall || page_ == 3)
                    start();
                else
                {
                    if (page_ == 2 && !capture_path())
                        break;
                    ++page_;
                    page();
                }
                break;
            case cancel_id:
                SendMessageW(window_, WM_CLOSE, 0, 0);
                break;
            case log_id:
                ShellExecuteW(window_, L"open", result_.log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            return 0;
        case progress_message: {
            std::unique_ptr<ProgressUpdate> update(reinterpret_cast<ProgressUpdate *>(l));
            if (progress_)
                SendMessageW(progress_, PBM_SETPOS, update->percent, 0);
            if (editor_)
                SetWindowTextW(editor_, update->text.c_str());
            return 0;
        }
        case result_message: {
            std::unique_ptr<Result> result(reinterpret_cast<Result *>(l));
            result_ = std::move(*result);
            page_ = 5;
            page();
            return 0;
        }
        case WM_CLOSE:
            if (page_ == 4)
                return 0;
            result_.code = started_ ? result_.code : 0;
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, w, l);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        auto self = reinterpret_cast<Wizard *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Wizard *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->message(message, w, l) : DefWindowProcW(window, message, w, l);
    }

  public:
    Wizard(HINSTANCE module, Options options) : module_(module), options_(std::move(options)) {}
    ~Wizard()
    {
        if (worker_.joinable())
            worker_.join();
        DeleteObject(font_);
        DeleteObject(heading_);
        DeleteObject(brand_);
        DeleteObject(white_);
        DeleteObject(background_);
    }
    int run()
    {
        driver_enabled_ = signed_driver_available() || options_.local_test;
        if (!driver_enabled_ && !options_.uninstall && !options_.provision && !options_.lite)
        {
            options_.lite = true;
            options_.target = install_location(true);
        }
        if (options_.target.empty())
            options_.target = install_location(options_.lite);
        existing_ = std::filesystem::exists(options_.target / L"OmniSound.exe");
        dpi_ = GetDpiForSystem();
        font_ = make_font(14, FW_NORMAL);
        heading_ = make_font(26, FW_NORMAL);
        brand_ = make_font(25, FW_MEDIUM);
        WNDCLASSW cls{};
        cls.hInstance = module_;
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = L"OmniSound.Setup.Wizard";
        cls.hbrBackground = background_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cls);
        RECT bounds{0, 0, px(840), px(600)};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, dpi_);
        window_ = CreateWindowExW(0, cls.lpszClassName, L"OmniSound 설치", style, CW_USEDEFAULT,
                                  CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                  nullptr, nullptr, module_, this);
        if (!window_)
            return 1;
        if (options_.provision)
        {
            accepted_ = options_.accepted;
            start();
        }
        else
            page();
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        if (options_.smoke)
            return smoke();
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            if (!IsDialogMessageW(window_, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        return result_.code;
    }
};
} // namespace
std::filesystem::path install_location(bool lite)
{
    wchar_t location[32768]{};
    DWORD size = sizeof(location);
    const auto key = lite ? L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OmniSoundLite"
                          : L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OmniSoundDriver";
    if (RegGetValueW(lite ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, key, L"InstallLocation", RRF_RT_REG_SZ,
                     nullptr, location, &size) == ERROR_SUCCESS)
        return location;
    PWSTR value{};
    check(SHGetKnownFolderPath(lite ? FOLDERID_LocalAppData : FOLDERID_ProgramFiles, 0, nullptr, &value),
          "Locate installation folder");
    auto path = std::filesystem::path(value);
    CoTaskMemFree(value);
    return lite ? path / L"Programs" / L"OmniSoundLite" : path / L"OmniSound";
}
int show_wizard(HINSTANCE module, Options options)
{
    return Wizard(module, std::move(options)).run();
}
} // namespace omni::setup
