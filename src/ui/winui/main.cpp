#include "pch.h"
#include "App.xaml.h"
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>
#include <omni/platform.hpp>
#include "desktop.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    omni::Handle instance;
    if (std::wstring_view(GetCommandLineW()).find(L"--ui-") == std::wstring_view::npos)
    {
        const bool lite = omni::is_lite_install();
        auto name = L"Local\\OmniSound.UI." + std::wstring(lite ? L"Lite." : L"Driver.") + omni::user_sid();
        instance.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            auto window = FindWindowW(nullptr, lite ? L"OmniSound Lite" : L"OmniSound Pro");
            if (window &&
                std::wstring_view(GetCommandLineW()).find(L"--background") == std::wstring_view::npos)
            {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(window, DesktopShell::restore_message(), 0, 0);
            }
            return 0;
        }
    }
    PACKAGE_VERSION minimum_version{};
    minimum_version.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
    const auto bootstrap =
        MddBootstrapInitialize2(WINDOWSAPPSDK_RELEASE_MAJORMINOR, WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
                                minimum_version, MddBootstrapInitializeOptions_None);
    if (FAILED(bootstrap))
    {
        wchar_t message[256]{};
        swprintf_s(message,
                   L"Windows App Runtime 초기화에 실패했습니다. (0x%08X)\nOmniSound을 다시 설치하거나 "
                   L"Windows App Runtime을 복구하세요.",
                   static_cast<unsigned>(bootstrap));
        MessageBoxW(nullptr, message, L"OmniSound 시작 오류", MB_OK | MB_ICONERROR);
        return static_cast<int>(bootstrap);
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::Microsoft::UI::Xaml::Application::Start(
        [](auto &&) { winrt::make<winrt::OmniSound::implementation::App>(); });
    MddBootstrapShutdown();
    return 0;
}
