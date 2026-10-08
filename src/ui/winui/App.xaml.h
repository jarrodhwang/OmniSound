#pragma once
#include "App.xaml.g.h"

namespace winrt::OmniSound::implementation
{
struct App : AppT<App>
{
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const &);

  private:
    Microsoft::UI::Xaml::Window window_{nullptr};
    Windows::Foundation::IAsyncAction capture_{nullptr};
};
} // namespace winrt::OmniSound::implementation

