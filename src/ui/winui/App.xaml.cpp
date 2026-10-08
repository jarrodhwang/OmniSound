#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include <shellapi.h>

namespace winrt::OmniSound::implementation
{
App::App()
{
    InitializeComponent();
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const &)
{
    window_ = make<MainWindow>();
    window_.Activate();
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool input_smoke = false, desktop_smoke = false, background = false;
    std::wstring snapshot_path;
    if (arguments)
    {
        for (int i = 1; i < count; ++i)
        {
            input_smoke |= std::wstring_view(arguments[i]) == L"--ui-input-smoke";
            desktop_smoke |= std::wstring_view(arguments[i]) == L"--ui-desktop-smoke";
            background |= std::wstring_view(arguments[i]) == L"--background";
            if (std::wstring_view(arguments[i]) == L"--ui-visual-smoke" && i + 1 < count)
                snapshot_path = arguments[++i];
        }
        LocalFree(arguments);
    }
    if (background)
        get_self<MainWindow>(window_.as<winrt::OmniSound::MainWindow>())->HideToTray();
    if (!snapshot_path.empty())
    {
        auto timer = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
        timer.Interval(std::chrono::milliseconds(500));
        timer.Tick([this, timer, snapshot_path](auto const &, auto const &) {
            timer.Stop();
            capture_ =
                get_self<MainWindow>(window_.as<winrt::OmniSound::MainWindow>())->VisualSmoke(snapshot_path);
        });
        timer.Start();
    }
    if (input_smoke || desktop_smoke)
    {
        try
        {
            auto self = get_self<MainWindow>(window_.as<winrt::OmniSound::MainWindow>());
            const auto result = desktop_smoke ? self->DesktopSmokeTest() : self->InputSmokeTest();
            ExitProcess(result);
        }
        catch (...)
        {
            ExitProcess(2);
        }
    }
}
} // namespace winrt::OmniSound::implementation

#include "App.xaml.g.hpp"
