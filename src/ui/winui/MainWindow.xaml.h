#pragma once
#include "MainWindow.g.h"
#include "MainWindow.xaml.g.h"
#include "../controller.hpp"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <memory>
#include "desktop.hpp"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.UI.ViewManagement.h>

namespace winrt::OmniSound::implementation
{
struct MainWindow : MainWindowT<MainWindow>
{
    MainWindow();
    ~MainWindow();
    int InputSmokeTest();
    int DesktopSmokeTest();
    void HideToTray();
    winrt::Windows::Foundation::IAsyncAction VisualSmoke(std::wstring path);
    void Startup_Toggled(Windows::Foundation::IInspectable const &,
                         Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Exit_Click(Windows::Foundation::IInspectable const &, Microsoft::UI::Xaml::RoutedEventArgs const &);
    void AddGroup_Click(Windows::Foundation::IInspectable const &,
                        Microsoft::UI::Xaml::RoutedEventArgs const &);
    void RemoveGroup_Click(Windows::Foundation::IInspectable const &,
                           Microsoft::UI::Xaml::RoutedEventArgs const &);
    void GroupsList_SelectionChanged(Windows::Foundation::IInspectable const &,
                                     Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const &);
    void DevicesList_SelectionChanged(Windows::Foundation::IInspectable const &,
                                      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const &);
    void MoreDevices_Click(Windows::Foundation::IInspectable const &,
                           Microsoft::UI::Xaml::RoutedEventArgs const &);
    void AllFollowVolume_Click(Windows::Foundation::IInspectable const &,
                               Microsoft::UI::Xaml::RoutedEventArgs const &);
    void GroupName_TextChanged(Windows::Foundation::IInspectable const &,
                               Microsoft::UI::Xaml::Controls::TextChangedEventArgs const &);
    void Identify_Click(Windows::Foundation::IInspectable const &,
                        Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Refresh_Click(Windows::Foundation::IInspectable const &,
                       Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Calibrate_Click(Windows::Foundation::IInspectable const &,
                         Microsoft::UI::Xaml::RoutedEventArgs const &);
    void CancelCalibration_Click(Windows::Foundation::IInspectable const &,
                                 Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Save_Click(Windows::Foundation::IInspectable const &, Microsoft::UI::Xaml::RoutedEventArgs const &);
    void WindowsSound_Click(Windows::Foundation::IInspectable const &,
                            Microsoft::UI::Xaml::RoutedEventArgs const &);
    void Close_Click(Windows::Foundation::IInspectable const &, Microsoft::UI::Xaml::RoutedEventArgs const &);
    void ApplyLive_Click(Windows::Foundation::IInspectable const &,
                         Microsoft::UI::Xaml::RoutedEventArgs const &);
    void ConfigureSpeakers_Click(Windows::Foundation::IInspectable const &,
                                 Microsoft::UI::Xaml::RoutedEventArgs const &);

  private:
    omni::Group *current_group();
    omni::Member *current_member();
    void invoke(std::function<void(omni::ui::Controller::Completion)> operation);
    void refresh(bool background = false);
    void apply_snapshot(omni::ui::Snapshot snapshot, bool background = false);
    void populate_groups();
    void populate_devices();
    void populate_microphones();
    void show_selection();
    void mark_dirty(bool live = false);
    void update_controls();
    void update_status();
    void tray_action(unsigned);
    void activate_selection();
    void device_membership_changed(const std::string &, bool);
    void update_bulk_volume();
    void apply_system_appearance();
    void populate_layout();
    void localize_ui();
    double reference_percent(double fallback) const;
    Windows::UI::ViewManagement::UISettings appearance_;
    Windows::UI::ViewManagement::UISettings::ColorValuesChanged_revoker appearance_changed_;
    std::unique_ptr<DesktopShell> desktop_;
    bool quitting_{}, smoke_{};
    std::shared_ptr<omni::ui::Controller> controller_;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer live_timer_{nullptr};
    omni::Config config_;
    std::vector<omni::Endpoint> outputs_;
    std::vector<omni::Endpoint> microphones_;
    std::string default_render_id_;
    std::string selected_group_id_;
    std::string selected_endpoint_id_;
    omni::Json engine_status_ = omni::Json::object();
    bool dirty_{};
    bool busy_{};
    bool polling_{};
    bool service_available_{};
    bool lite_mode_{};
    bool loaded_{};
    bool filling_{};
    bool show_unavailable_{};
    bool live_pending_{}, manual_draft_{}, dragging_layout_{};
};
} // namespace winrt::OmniSound::implementation

namespace winrt::OmniSound::factory_implementation
{
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
{
};
} // namespace winrt::OmniSound::factory_implementation
