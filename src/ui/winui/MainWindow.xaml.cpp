#include "pch.h"
#include "MainWindow.xaml.h"
#include "localization.hpp"
#include <omni/ipc.hpp>
#include <shellapi.h>
#include <cmath>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>

namespace
{
using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

std::wstring endpoint_label(const omni::Endpoint &endpoint, bool duplicate_name)
{
    auto label = omni::wide(endpoint.name);
    if (!duplicate_name)
        return label;
    auto identity = omni::wide(endpoint.id.substr(endpoint.id.size() > 9 ? endpoint.id.size() - 9 : 0));
    return label + L"  ·  " + identity;
}

bool same_devices(const std::vector<omni::Endpoint> &left, const std::vector<omni::Endpoint> &right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i)
        if (left[i].id != right[i].id || left[i].name != right[i].name ||
            left[i].instance != right[i].instance || left[i].active != right[i].active)
            return false;
    return true;
}
} // namespace

namespace winrt::OmniSound::implementation
{
MainWindow::MainWindow()
{
    InitializeComponent();
    lite_mode_ = omni::is_lite_install();
    Title(lite_mode_ ? L"OmniSound Lite" : L"OmniSound Pro");
    smoke_ = std::wstring_view(GetCommandLineW()).find(L"--ui-") != std::wstring_view::npos;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    DWORD locale_size = sizeof(locale);
    if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\OmniSound\\Desktop", L"Language", RRF_RT_REG_SZ, nullptr,
                     locale, &locale_size) != ERROR_SUCCESS)
        GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    omni::i18n::set_language(omni::utf8(locale));
    static const wchar_t *language_labels[]{L"English", L"한국어", L"Français", L"Deutsch", L"日本語"};
    static const char *language_codes[]{"en", "ko", "fr", "de", "ja"};
    filling_ = true;
    for (unsigned i = 0; i < 5; ++i)
    {
        LanguagePicker().Items().Append(box_value(language_labels[i]));
        if (omni::i18n::language() == language_codes[i])
            LanguagePicker().SelectedIndex(i);
    }
    filling_ = false;
    LanguagePicker().SelectionChanged([this](auto const &, auto const &) {
        const auto index = LanguagePicker().SelectedIndex();
        if (filling_ || index < 0 || index >= 5)
            return;
        omni::i18n::set_language(language_codes[index]);
        if (!smoke_)
        {
            HKEY key{};
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\OmniSound\\Desktop", 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
            {
                const auto language = omni::wide(language_codes[index]);
                RegSetValueExW(key, L"Language", 0, REG_SZ, reinterpret_cast<const BYTE *>(language.c_str()),
                               static_cast<DWORD>((language.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(key);
            }
        }
        localize_ui();
        populate_devices();
        show_selection();
        update_status();
    });
    ModeBadgeText().Text(lite_mode_ ? L"LITE" : L"PRO");
    ProPanel().Visibility(lite_mode_ ? Visibility::Collapsed : Visibility::Visible);
    ProcessingPicker().Items().Append(box_value(L"Original PCM (RAW)"));
    ProcessingPicker().Items().Append(box_value(L"Windows audio processing"));
    static const wchar_t *route_labels[]{L"Stereo / surround downmix",
                                         L"Swap left / right",
                                         L"Mono",
                                         L"Left only",
                                         L"Right only",
                                         L"Speaker position",
                                         L"Front left",
                                         L"Front right",
                                         L"Center",
                                         L"LFE",
                                         L"Back left",
                                         L"Back right",
                                         L"Side left",
                                         L"Side right"};
    for (auto label : route_labels)
        RoutingPicker().Items().Append(box_value(label));
    ProcessingPicker().SelectionChanged([this](auto const &, auto const &) {
        if (!filling_ && current_group())
        {
            current_group()->output_processing = ProcessingPicker().SelectedIndex() == 1 ? "system" : "raw";
            mark_dirty(true);
        }
    });
    RoutingPicker().SelectionChanged([this](auto const &, auto const &) {
        static const char *routes[]{"stereo",    "swap",       "mono",        "left",      "right",
                                    "position",  "front-left", "front-right", "center",    "lfe",
                                    "back-left", "back-right", "side-left",   "side-right"};
        auto selected = RoutingPicker().SelectedIndex();
        if (!filling_ && current_member() && selected >= 0 && selected < 14)
        {
            current_member()->routing = routes[selected];
            mark_dirty(true);
            populate_layout();
        }
    });
    SafetyBufferBox().ValueChanged([this](auto const &, auto const &args) {
        if (!filling_ && current_group() && std::isfinite(args.NewValue()))
        {
            current_group()->safety_ms = args.NewValue();
            mark_dirty(true);
        }
    });
    AzimuthBox().ValueChanged([this](auto const &, auto const &args) {
        if (!filling_ && current_member() && std::isfinite(args.NewValue()))
        {
            current_member()->azimuth_deg = args.NewValue();
            mark_dirty(true);
            populate_layout();
        }
    });
    DistanceBox().ValueChanged([this](auto const &, auto const &args) {
        if (!filling_ && current_member() && std::isfinite(args.NewValue()))
        {
            current_member()->distance_m = args.NewValue();
            mark_dirty(true);
            populate_layout();
        }
    });
    SpeakerCanvas().SizeChanged([this](auto const &, auto const &) { populate_layout(); });
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(TitleBarPanel());
    AppWindow().Resize({980, 900});
    const auto titlebar = AppWindow().TitleBar();
    const Windows::UI::Color clear{0, 0, 0, 0};
    titlebar.ButtonBackgroundColor(clear);
    titlebar.ButtonInactiveBackgroundColor(clear);
    HWND window{};
    check_hresult(get_strong().as<IWindowNative>()->get_WindowHandle(&window));
    const bool tray = !smoke_ || std::wstring_view(GetCommandLineW()).find(L"--ui-desktop-smoke") !=
                                     std::wstring_view::npos;
    desktop_ = std::make_unique<DesktopShell>(
        window, tray, [this](unsigned action) { tray_action(action); },
        [this] {
            DesktopShell::Groups groups;
            for (const auto &group : config_.groups)
                groups.emplace_back(omni::wide(group.name), group.id == omni::active_group_id(config_));
            return lite_mode_ ? groups : DesktopShell::Groups{};
        },
        [this] {
            return engine_status_.value("engine", omni::Json::object()).value("startWithWindows", true);
        });
    AppWindow().Closing([this](auto const &, auto const &args) {
        if (!quitting_ && desktop_->has_tray())
        {
            args.Cancel(true);
            HideToTray();
        }
    });
    dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    const auto weak = get_weak();
    const auto queue = dispatcher_;
    RootPanel().ActualThemeChanged([weak](auto const &, auto const &) {
        if (auto self = weak.get())
            self->apply_system_appearance();
    });
    appearance_changed_ =
        appearance_.ColorValuesChanged(winrt::auto_revoke, [weak, queue](auto const &, auto const &) {
            queue.TryEnqueue([weak] {
                if (auto self = weak.get())
                    self->apply_system_appearance();
            });
        });
    apply_system_appearance();
    if (!smoke_)
        controller_ = std::make_shared<omni::ui::IpcController>(lite_mode_);

    MemberGainSlider().ValueChanged([this](auto const &, auto const &) {
        GainValueText().Text(std::to_wstring(static_cast<int>(MemberGainSlider().Value())) + L"%");
        if (filling_)
            return;
        if (auto *member = current_member())
        {
            member->gain = MemberGainSlider().Value() / 100.0;
            mark_dirty(true);
        }
    });
    MemberDelayBox().ValueChanged([this](auto const &, auto const &args) {
        const double delay = args.NewValue();
        if (filling_ || !std::isfinite(delay))
            return;
        if (auto *member = current_member())
        {
            if (delay < 0 || delay > 2000)
            {
                StatusText().Text(T(L"수동 지연은 0–2000 ms 범위여야 합니다."));
                return;
            }
            member->delay_ms = delay;
            if (member->calibration && lite_mode_)
                member->calibration.reset();
            mark_dirty(true);
        }
    });

    FollowVolumeCheck().Checked([this](auto const &, auto const &) {
        if (!filling_)
            if (auto *member = current_member())
            {
                member->follow_reference_volume = true;
                filling_ = true;
                MemberGainSlider().Value(reference_percent(member->gain * 100));
                filling_ = false;
                mark_dirty(true);
            }
        update_controls();
    });
    FollowVolumeCheck().Unchecked([this](auto const &, auto const &) {
        if (!filling_)
            if (auto *member = current_member())
            {
                member->follow_reference_volume = false;
                filling_ = true;
                MemberGainSlider().Value(member->gain * 100);
                filling_ = false;
                mark_dirty(true);
            }
        update_controls();
    });
    timer_ = dispatcher_.CreateTimer();
    timer_.Interval(std::chrono::seconds(2));
    timer_.Tick([this](auto const &, auto const &) {
        HWND window{};
        check_hresult(get_strong().as<IWindowNative>()->get_WindowHandle(&window));
        timer_.Interval(IsWindowVisible(window) ? std::chrono::seconds(2) : std::chrono::seconds(15));
        if (!busy_ && !polling_)
            refresh(true);
    });
    live_timer_ = dispatcher_.CreateTimer();
    live_timer_.Interval(std::chrono::milliseconds(250));
    live_timer_.Tick([this](auto const &, auto const &) {
        if (lite_mode_ || !live_pending_ || manual_draft_ || busy_ || dragging_layout_ || !controller_ ||
            !LiveControlsToggle().IsOn() || !service_available_ || !loaded_)
            return;
        // Do not commit an incomplete NumberBox or group-name edit while typing.
        if (auto focused = Input::FocusManager::GetFocusedElement(RootPanel().XamlRoot());
            focused && focused.try_as<TextBox>())
            return;
        live_pending_ = false;
        Save_Click(nullptr, RoutedEventArgs{nullptr});
    });
    if (!smoke_)
    {
        refresh();
        timer_.Start();
        if (!lite_mode_)
            live_timer_.Start();
    }
    localize_ui();
    StatusText().Text(T(L"연결 중"));
}

void MainWindow::localize_ui()
{
    const auto filling = filling_;
    filling_ = true;
    translate_tree(RootPanel());
    FollowVolumeCheck().Content(
        box_value(winrt::hstring(T(lite_mode_ ? L"기본 출력과 볼륨 동기화" : L"Match group volume"))));
    AllFollowVolumeCheck().Content(box_value(winrt::hstring(T(L"Synchronize all device volumes"))));
    ToolTipService::SetToolTip(
        FollowVolumeCheck(),
        box_value(winrt::hstring(T(
            lite_mode_ ? L"기본 출력 50%일 때 추가 출력도 50%로 설정합니다. 볼륨 비율을 다시 곱하지 않습니다."
                       : L"A group output at 50% sets this device to Windows 50%, without multiplying the "
                         L"volume twice."))));
    ToolTipService::SetToolTip(
        AllFollowVolumeCheck(),
        box_value(winrt::hstring(
            T(lite_mode_
                  ? L"이 그룹에 선택한 추가 출력 장치들을 Windows 기본 출력과 같은 퍼센트로 설정합니다. 체크 "
                    L"후 저장하세요."
                  : L"Match every selected device to the Windows volume of this group's virtual output."))));
    StartupToggle().OnContent(box_value(winrt::hstring(T(L"On"))));
    StartupToggle().OffContent(box_value(winrt::hstring(T(L"Off"))));
    LiveControlsToggle().OnContent(box_value(winrt::hstring(T(L"On"))));
    LiveControlsToggle().OffContent(box_value(winrt::hstring(T(L"Off"))));
    filling_ = filling;
}

void MainWindow::populate_layout()
{
    if (lite_mode_)
        return;
    SpeakerCanvas().Children().Clear();
    const auto *group = current_group();
    if (!group)
        return;
    const double width = std::max(260.0, SpeakerCanvas().ActualWidth()), cx = width / 2, cy = 115;
    const double radius = std::min(width / 2 - 40, 85.0);
    auto listener = TextBlock();
    listener.Text(winrt::hstring(T(L"Listener")));
    listener.FontSize(12);
    Canvas::SetLeft(listener, cx - 25);
    Canvas::SetTop(listener, cy - 8);
    SpeakerCanvas().Children().Append(listener);
    unsigned index = 0;
    for (const auto &member : group->members)
    {
        auto marker = Button();
        marker.Content(box_value(winrt::hstring(std::to_wstring(++index))));
        marker.Width(36);
        marker.Height(32);
        const double angle = member.azimuth_deg * 3.141592653589793 / 180;
        const double r = member.distance_m / (member.distance_m + 1) * radius;
        Canvas::SetLeft(marker, cx + std::sin(angle) * r - 18);
        Canvas::SetTop(marker, cy - std::cos(angle) * r - 16);
        auto endpoint = std::find_if(outputs_.begin(), outputs_.end(),
                                     [&](const auto &e) { return e.id == member.endpoint_id; });
        const auto name = endpoint == outputs_.end() ? member.endpoint_id : endpoint->name;
        ToolTipService::SetToolTip(marker, box_value(winrt::hstring(omni::wide(name))));
        Automation::AutomationProperties::SetName(marker, omni::wide(name));
        marker.Click([this, id = member.endpoint_id](auto const &, auto const &) {
            selected_endpoint_id_ = id;
            populate_devices();
            show_selection();
        });
        marker.PointerPressed([this](auto const &sender, auto const &args) {
            dragging_layout_ = true;
            sender.template as<Button>().CapturePointer(args.Pointer());
        });
        marker.PointerMoved(
            [this, id = member.endpoint_id, cx, cy, radius](auto const &sender, auto const &args) {
                auto button = sender.template as<Button>();
                if (!button.PointerCaptures().Size() || !current_group())
                    return;
                auto member = std::find_if(current_group()->members.begin(), current_group()->members.end(),
                                           [&](auto const &m) { return m.endpoint_id == id; });
                if (member == current_group()->members.end())
                    return;
                const auto point = args.GetCurrentPoint(SpeakerCanvas()).Position();
                const double dx = point.X - cx, dy = point.Y - cy;
                member->azimuth_deg = std::atan2(dx, -dy) * 180 / 3.141592653589793;
                const double r = std::sqrt(dx * dx + dy * dy);
                member->distance_m = std::clamp(r / std::max(.001, radius - r), .2, 20.0);
                member->routing = "position";
                selected_endpoint_id_ = id;
                filling_ = true;
                RoutingPicker().SelectedIndex(5);
                AzimuthBox().Value(member->azimuth_deg);
                DistanceBox().Value(member->distance_m);
                filling_ = false;
                const double scale = std::min(1.0, radius / std::max(.001, std::sqrt(dx * dx + dy * dy)));
                Canvas::SetLeft(button, cx + dx * scale - 18);
                Canvas::SetTop(button, cy + dy * scale - 16);
                mark_dirty(true);
            });
        marker.PointerReleased([this](auto const &sender, auto const &) {
            sender.template as<Button>().ReleasePointerCaptures();
            dragging_layout_ = false;
            show_selection();
        });
        marker.PointerCaptureLost([this](auto const &, auto const &) { dragging_layout_ = false; });
        SpeakerCanvas().Children().Append(marker);
    }
}
void MainWindow::ApplyLive_Click(IInspectable const &sender, RoutedEventArgs const &args)
{
    Save_Click(sender, args);
}
void MainWindow::ConfigureSpeakers_Click(IInspectable const &, RoutedEventArgs const &)
{
    ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl,,0", nullptr, SW_SHOWNORMAL);
}

MainWindow::~MainWindow()
{
    appearance_changed_.revoke();
    if (timer_)
        timer_.Stop();
    if (live_timer_)
        live_timer_.Stop();
    controller_.reset();
    desktop_.reset();
}
void MainWindow::apply_system_appearance()
{
    using namespace Windows::UI::ViewManagement;
    const bool dark = RootPanel().ActualTheme() == ElementTheme::Dark;
    const auto foreground = AccessibilitySettings().HighContrast()
                                ? appearance_.GetColorValue(UIColorType::Foreground)
                            : dark ? Windows::UI::Color{255, 255, 255, 255}
                                   : Windows::UI::Color{255, 0, 0, 0};
    const auto titlebar = AppWindow().TitleBar();
    titlebar.ButtonForegroundColor(foreground);
    titlebar.ButtonHoverForegroundColor(foreground);
    titlebar.ButtonPressedForegroundColor(foreground);
    titlebar.ButtonInactiveForegroundColor(foreground);
    titlebar.ButtonHoverBackgroundColor(dark ? Windows::UI::Color{255, 60, 60, 60}
                                             : Windows::UI::Color{255, 230, 230, 230});
    const auto accent = appearance_.GetColorValue(UIColorType::Accent);
    if (desktop_)
        desktop_->accent(RGB(accent.R, accent.G, accent.B));
}

int MainWindow::InputSmokeTest()
{
    lite_mode_ = true; // This section verifies the Lite picker contract explicitly.
    ProPanel().Visibility(Visibility::Collapsed);
    const auto stage = [](int step) {
        omni::atomic_json(omni::executable_dir() / L"input-smoke-stage.json", {{"step", step}});
    };
    stage(0);
    if (RootPanel().RequestedTheme() != ElementTheme::Default)
        return 52;
    // Exercise real XAML editors with synthetic snapshots. No configuration is
    // saved and no test sound is played by this dedicated smoke-test process.
    timer_.Stop();
    controller_.reset();
    busy_ = polling_ = dirty_ = loaded_ = false;
    config_ = {};
    selected_group_id_.clear();
    selected_endpoint_id_.clear();
    omni::ui::Snapshot snapshot;
    snapshot.service_available = true;
    snapshot.config.groups.push_back(
        {"input-smoke", "original", {{"input-output", 1, 0, {}}, {"offline-output", 1, 0, {}}}, 200});
    snapshot.outputs.push_back({"input-output", "test output", "", "", "", false, false, true});
    snapshot.outputs.push_back({"offline-output", "AAA unavailable output", "", "", "", false, false, false});
    snapshot.engine_status = {{"engine", {{"state", "running"}}}, {"job", {{"state", "idle"}}}};
    stage(1);
    apply_snapshot(snapshot);
    stage(2);
    if (DevicesList().Items().Size() != 1 || selected_endpoint_id_ != "input-output")
        return 16;
    MoreDevices_Click(nullptr, RoutedEventArgs{nullptr});
    if (DevicesList().Items().Size() != 2 || selected_endpoint_id_ != "input-output")
        return 17;
    MoreDevices_Click(nullptr, RoutedEventArgs{nullptr});
    if (DevicesList().Items().Size() != 1 || current_group()->members.size() != 2)
        return 18;

    const auto row = GroupsList().SelectedItem();
    GroupNameBox().Text(L"한글 그룹 abc");
    // XAML defers TextChanged until the dispatcher runs. Exercise its handler
    // explicitly here so this synchronous test does not pump nested messages.
    GroupName_TextChanged(nullptr, Controls::TextChangedEventArgs{nullptr});
    GroupNameBox().Select(2, 3);
    if (!dirty_)
        return 10;
    if (GroupsList().SelectedItem() != row)
        return 11;
    if (row.as<ListViewItem>().Content().as<winrt::hstring>() != GroupNameBox().Text())
        return 12;
    MemberDelayBox().Text(L"12.");
    const auto draft = MemberDelayBox().Text();
    polling_ = true;
    update_controls();
    if (!GroupNameBox().IsEnabled() || !MemberDelayBox().IsEnabled() || !SaveButton().IsEnabled())
        return 13;
    apply_snapshot(snapshot, true);
    if (polling_ || GroupNameBox().Text() != L"한글 그룹 abc" || GroupNameBox().SelectionStart() != 2 ||
        GroupNameBox().SelectionLength() != 3 || MemberDelayBox().Text() != draft ||
        GroupsList().SelectedItem() != row)
        return 14;

    // A stale poll must not re-enable controls or overwrite command progress
    // while a foreground operation is waiting on the same controller queue.
    busy_ = polling_ = true;
    StatusText().Text(L"command in progress");
    update_controls();
    apply_snapshot(snapshot, true);
    if (!(busy_ && !polling_ && !GroupNameBox().IsEnabled() &&
          StatusText().Text() == L"command in progress" && MemberDelayBox().Text() == draft))
        return 15;
    busy_ = false;
    config_.groups.clear();
    AddGroup_Click(nullptr, RoutedEventArgs{nullptr});
    current_group()->members.push_back({"input-output", 1, 0, {}});
    selected_endpoint_id_ = "input-output";
    show_selection();
    MemberGainSlider().Value(73);
    MemberDelayBox().Value(2000);
    FollowVolumeCheck().IsChecked(true);
    if (std::abs(current_member()->gain - 0.73) > 1e-9 || current_member()->delay_ms != 2000 ||
        !current_member()->follow_reference_volume || MemberGainSlider().IsEnabled())
        return 20;
    FollowVolumeCheck().IsChecked(false);
    if (!MemberGainSlider().IsEnabled())
        return 21;
    omni::validate(config_);
    const auto roundtrip = omni::Json(config_).get<omni::Config>();
    if (roundtrip.groups.size() != 1 || roundtrip.groups[0].id != current_group()->id)
        return 19;

    // Selecting the real ListView row must not toggle its checkbox. Checkbox
    // automation exercises keyboard/screen-reader membership and bulk toggles.
    using namespace Automation::Peers;
    using namespace Automation::Provider;
    omni::ui::Snapshot devices;
    devices.service_available = devices.save_committed = true;
    devices.config = config_;
    devices.config.groups.front().members.push_back({"other-output", 0.65, 0, {}});
    devices.config.groups.front().members.push_back({"primary-output", 0.42, 0, {}});
    devices.default_render_id = "primary-output";
    const std::string long_name =
        "DELL S2725DS (NVIDIA High Definition Audio) - DisplayPort secondary monitor output";
    devices.outputs = {{"primary-output", long_name, "", "", "", false, false, true},
                       {"other-output", long_name, "", "", "", false, false, true},
                       {"input-output", "AW2521HFL", "", "", "", false, false, true},
                       {"new-output", "USB Audio", "", "", "", false, false, true}};
    devices.engine_status = {{"engine", {{"state", "running"}, {"referenceVolumePercent", 50.0}}},
                             {"job", {{"state", "idle"}}}};
    stage(39);
    apply_snapshot(devices);
    stage(40);
    const auto find_row = [&](const std::string &id) {
        for (const auto &item : DevicesList().Items())
        {
            auto row = item.as<ListViewItem>();
            if (omni::utf8(row.Tag().as<winrt::hstring>().c_str()) == id)
                return row;
        }
        throw std::runtime_error("Device row missing");
    };
    const auto checkbox = [](const ListViewItem &row) {
        return row.Content().as<Grid>().FindName(L"MembershipCheck").as<CheckBox>();
    };
    const auto select = [this](const ListViewItem &row) { DevicesList().SelectedItem(row); };
    const auto toggle = [](const CheckBox &check) {
        const auto pattern = CheckBoxAutomationPeer(check).GetPattern(PatternInterface::Toggle);
        if (!pattern)
            throw std::runtime_error("Checkbox Toggle pattern missing");
        pattern.as<IToggleProvider>().Toggle();
    };
    const auto primary = find_row("primary-output");
    if (DevicesList().Items().GetAt(0) != primary || checkbox(primary).IsEnabled() ||
        checkbox(primary).IsChecked().Value() ||
        primary.Content().as<Grid>().FindName(L"DefaultBadge").as<TextBlock>().Visibility() !=
            Visibility::Visible)
        return 40;
    const auto label = primary.Content().as<Grid>().FindName(L"DeviceName").as<TextBlock>();
    if (label.Text() != omni::wide(long_name) || label.TextWrapping() != TextWrapping::Wrap ||
        label.TextTrimming() != TextTrimming::None)
        return 41;
    stage(42);
    select(primary);
    if (MemberGainSlider().IsEnabled() || MemberDelayBox().IsEnabled() ||
        SelectedDeviceHintText().Visibility() != Visibility::Visible || MemberGainSlider().Value() != 50)
        return 42;
    const auto member_count = current_group()->members.size();
    stage(43);
    select(find_row("other-output"));
    if (selected_endpoint_id_ != "other-output" || current_group()->members.size() != member_count ||
        !checkbox(find_row("other-output")).IsChecked().Value() || MemberGainSlider().Value() != 65)
        return 43;
    stage(44);
    toggle(AllFollowVolumeCheck());
    if (!AllFollowVolumeCheck().IsChecked().Value() || MemberGainSlider().Value() != 50 ||
        MemberGainSlider().IsEnabled() || current_group()->members.back().follow_reference_volume)
        return 44;
    stage(45);
    toggle(FollowVolumeCheck());
    if (AllFollowVolumeCheck().IsChecked() || !MemberGainSlider().IsEnabled() ||
        MemberGainSlider().Value() != 65)
        return 45;
    stage(46);
    toggle(AllFollowVolumeCheck());
    if (!AllFollowVolumeCheck().IsChecked() || !AllFollowVolumeCheck().IsChecked().Value())
        return 46;
    stage(47);
    select(find_row("new-output"));
    if (current_group()->members.size() != member_count || MemberGainSlider().IsEnabled())
        return 47;
    stage(48);
    toggle(checkbox(find_row("new-output")));
    if (current_group()->members.size() != member_count + 1 || !current_member() ||
        !current_member()->follow_reference_volume || !AllFollowVolumeCheck().IsChecked().Value())
        return 48;
    stage(49);
    toggle(AllFollowVolumeCheck());
    if (AllFollowVolumeCheck().IsChecked().Value() || !MemberGainSlider().IsEnabled() ||
        std::any_of(current_group()->members.begin(), current_group()->members.end(),
                    [](const auto &m) { return m.follow_reference_volume; }))
        return 49;
    const auto saved = omni::Json(config_).get<omni::Config>();
    omni::validate(saved);
    devices.default_render_id = "new-output";
    apply_snapshot(devices, true);
    if (checkbox(find_row("new-output")).IsEnabled() ||
        checkbox(find_row("new-output")).IsChecked().Value() ||
        !checkbox(find_row("primary-output")).IsEnabled() || MemberGainSlider().IsEnabled() ||
        SelectedDeviceHintText().Visibility() != Visibility::Visible)
        return 50;
    stage(51);
    // Pro routes the virtual source, so a physical Windows default output is
    // a valid member. Exercise real Pro editors and language switching too.
    lite_mode_ = false;
    ProPanel().Visibility(Visibility::Visible);
    populate_devices();
    const auto pro_primary = find_row("new-output");
    if (!checkbox(pro_primary).IsEnabled())
        return 60;
    select(pro_primary);
    if (!current_member())
        toggle(checkbox(pro_primary));
    show_selection();
    if (!MemberGainSlider().IsEnabled() || !RoutingPicker().IsEnabled())
        return 61;
    RoutingPicker().SelectedIndex(5);
    AzimuthBox().Value(-45);
    DistanceBox().Value(2);
    SafetyBufferBox().Value(0);
    ProcessingPicker().SelectedIndex(1);
    if (current_member()->routing != "position" || current_member()->azimuth_deg != -45 ||
        current_member()->distance_m != 2 || current_group()->safety_ms != 0 ||
        current_group()->output_processing != "system")
        return 62;
    omni::validate(config_);
    const auto group_name = GroupNameBox().Text();
    LanguagePicker().SelectedIndex(2);
    if (SaveButton().Content().as<winrt::hstring>() != L"Enregistrer" || GroupNameBox().Text() != group_name)
        return 63;
    LanguagePicker().SelectedIndex(4);
    if (SaveButton().Content().as<winrt::hstring>() != L"保存" || GroupNameBox().Text() != group_name)
        return 64;
    stage(65);
    LanguagePicker().SelectedIndex(0);
    const omni::Json measured_member = {
        {"endpointId", selected_endpoint_id_},
        {"calibrationValid", false},
        {"lastMeasurement", {{"verifiedSpreadMs", .4}, {"measuredAt", "2026-10-02T00:00:00Z"}}}};
    const omni::Json measured_group = {
        {"id", current_group()->id}, {"state", "playing"}, {"members", omni::Json::array({measured_member})}};
    engine_status_ = {{"engine", {{"state", "running"}, {"groups", omni::Json::array({measured_group})}}},
                      {"job",
                       {{"state", "complete"},
                        {"type", "calibrate"},
                        {"result", {{"passed", true}, {"group", {{"id", current_group()->id}}}}}}}};
    update_status();
    if (std::wstring_view(CalibrationStatusText().Text()).find(L"Recalibration required") ==
        std::wstring_view::npos)
        return 66;
    engine_status_["engine"]["groups"][0]["members"][0]["calibrationValid"] = true;
    update_status();
    if (std::wstring_view(CalibrationStatusText().Text()).find(L"Calibration current") ==
        std::wstring_view::npos)
        return 67;
    lite_mode_ = true;
    localize_ui();
    if (FollowVolumeCheck().Content().as<winrt::hstring>() != L"Match source volume")
        return 68;
    stage(69);
    return 0;
}

omni::Group *MainWindow::current_group()
{
    auto found = std::find_if(config_.groups.begin(), config_.groups.end(),
                              [this](const omni::Group &group) { return group.id == selected_group_id_; });
    return found == config_.groups.end() ? nullptr : &*found;
}

omni::Member *MainWindow::current_member()
{
    auto *group = current_group();
    if (!group || selected_endpoint_id_.empty())
        return nullptr;
    auto found =
        std::find_if(group->members.begin(), group->members.end(), [this](const omni::Member &member) {
            return member.endpoint_id == selected_endpoint_id_;
        });
    return found == group->members.end() ? nullptr : &*found;
}

double MainWindow::reference_percent(double fallback) const
{
    const auto engine = engine_status_.value("engine", omni::Json::object());
    if (!lite_mode_)
        for (const auto &group : engine.value("groups", omni::Json::array()))
            if (group.value("id", std::string{}) == selected_group_id_)
                return group.value("referenceVolumePercent", fallback);
    return engine.value("referenceVolumePercent", fallback);
}

void MainWindow::invoke(std::function<void(omni::ui::Controller::Completion)> operation)
{
    if (busy_ || !controller_)
        return;
    busy_ = true;
    update_controls();
    const auto weak = get_weak();
    const auto dispatcher = dispatcher_;
    try
    {
        operation([weak, dispatcher](omni::ui::Snapshot snapshot) mutable {
            dispatcher.TryEnqueue([weak, snapshot = std::move(snapshot)]() mutable {
                if (auto self = weak.get())
                    self->apply_snapshot(std::move(snapshot));
            });
        });
    }
    catch (const std::exception &e)
    {
        busy_ = false;
        StatusText().Text(omni::wide(e.what()));
        update_controls();
    }
}

void MainWindow::refresh(bool background)
{
    if (busy_ || polling_ || !controller_)
        return;
    if (background)
    {
        // Polls share the controller's serialized queue with user commands, but
        // never disable editors or replace their status with a progress message.
        polling_ = true;
        const auto weak = get_weak();
        const auto dispatcher = dispatcher_;
        try
        {
            controller_->refresh([weak, dispatcher](omni::ui::Snapshot snapshot) mutable {
                dispatcher.TryEnqueue([weak, snapshot = std::move(snapshot)]() mutable {
                    if (auto self = weak.get())
                        self->apply_snapshot(std::move(snapshot), true);
                });
            });
        }
        catch (const std::exception &e)
        {
            polling_ = false;
            StatusText().Text(omni::wide(e.what()));
        }
        return;
    }
    StatusText().Text(lite_mode_ ? T(L"Windows 기본 출력과 기기를 확인하는 중…")
                                 : T(L"서비스와 출력 기기를 확인하는 중…"));
    invoke([controller = controller_](auto completion) { controller->refresh(std::move(completion)); });
}

void MainWindow::apply_snapshot(omni::ui::Snapshot snapshot, bool background)
{
    if (background)
    {
        polling_ = false;
        // A user command queued after this poll owns the next visible result.
        if (busy_)
            return;
    }
    else
        busy_ = false;
    const auto previous_selection = selected_endpoint_id_;
    if (snapshot.activation_committed && dirty_)
    {
        config_.revision = snapshot.config.revision;
        config_.active_group_id = snapshot.config.active_group_id;
    }
    service_available_ = snapshot.service_available;
    engine_status_ = std::move(snapshot.engine_status);
    const auto previous_default = default_render_id_;
    default_render_id_ = std::move(snapshot.default_render_id);
    const auto previous_outputs = outputs_;
    const auto previous_microphones = microphones_;
    outputs_.clear();
    for (auto &endpoint : snapshot.outputs)
        if (endpoint.group_id.empty())
            outputs_.push_back(std::move(endpoint));

    microphones_.clear();
    for (auto &microphone : snapshot.microphones)
        if (microphone.capture && microphone.active && !microphone.bluetooth)
            microphones_.push_back(std::move(microphone));

    bool config_changed = false;
    if (snapshot.service_available &&
        (snapshot.save_committed || !loaded_ || (!dirty_ && snapshot.config.revision != config_.revision)))
    {
        if (current_group())
            selected_group_id_ = current_group()->id;
        const bool first_load = !loaded_;
        config_ = std::move(snapshot.config);
        if (first_load && lite_mode_)
            selected_group_id_ = omni::active_group_id(config_);
        if (snapshot.save_committed)
        {
            dirty_ = false;
            manual_draft_ = live_pending_ = false;
        }
        loaded_ = true;
        config_changed = true;
        if (!std::any_of(config_.groups.begin(), config_.groups.end(),
                         [this](const omni::Group &group) { return group.id == selected_group_id_; }))
            selected_group_id_ = config_.groups.empty() ? std::string{} : config_.groups.front().id;
    }
    else if (snapshot.service_available)
        loaded_ = true;

    for (const auto &group : config_.groups)
        for (const auto &member : group.members)
            if (std::none_of(outputs_.begin(), outputs_.end(), [&](const omni::Endpoint &endpoint) {
                    return endpoint.id == member.endpoint_id;
                }))
                outputs_.push_back(
                    {member.endpoint_id, "연결되지 않은 저장 기기", "", "", "", false, false, false});
    std::sort(outputs_.begin(), outputs_.end(), [this](const omni::Endpoint &a, const omni::Endpoint &b) {
        if ((a.id == default_render_id_) != (b.id == default_render_id_))
            return a.id == default_render_id_;
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    });

    if (config_changed)
        populate_groups();
    if (!same_devices(previous_outputs, outputs_) || config_changed || previous_default != default_render_id_)
        populate_devices();
    if (!same_devices(previous_microphones, microphones_))
        populate_microphones();
    if (config_changed || selected_endpoint_id_ != previous_selection ||
        previous_default != default_render_id_)
        show_selection();
    update_status();
    if (!snapshot.error.empty())
        StatusText().Text(std::wstring(T(L"요청 실패 · ")) + omni::wide(snapshot.error));
    update_controls();
}

void MainWindow::populate_groups()
{
    filling_ = true;
    GroupsList().Items().Clear();
    int selected = -1;
    for (size_t i = 0; i < config_.groups.size(); ++i)
    {
        const auto &group = config_.groups[i];
        auto item = ListViewItem();
        item.Content(box_value(winrt::hstring(omni::wide(group.name))));
        item.Tag(box_value(winrt::hstring(omni::wide(group.id))));
        GroupsList().Items().Append(item);
        if (group.id == selected_group_id_)
            selected = static_cast<int>(i);
    }
    GroupsList().SelectedIndex(selected);
    filling_ = false;
}

void MainWindow::populate_devices()
{
    filling_ = true;
    DevicesList().Items().Clear();
    std::map<std::string, unsigned> counts;
    for (const auto &endpoint : outputs_)
        if (endpoint.active || show_unavailable_)
            ++counts[endpoint.name];

    const auto *group = current_group();
    std::vector<const omni::Endpoint *> visible;
    for (const auto &endpoint : outputs_)
        if (endpoint.active || show_unavailable_)
            visible.push_back(&endpoint);
    const auto unavailable = std::count_if(outputs_.begin(), outputs_.end(),
                                           [](const omni::Endpoint &endpoint) { return !endpoint.active; });
    MoreDevicesButton().Visibility(unavailable ? Visibility::Visible : Visibility::Collapsed);
    MoreDevicesButton().Content(box_value(
        winrt::hstring(show_unavailable_ ? T(L"활성 장치만 보기")
                                         : T(L"장치 더보기 (") + std::to_wstring(unavailable) + L")")));
    int selected = -1;
    for (size_t i = 0; i < visible.size(); ++i)
    {
        const auto &endpoint = *visible[i];
        const bool included = group && std::any_of(group->members.begin(), group->members.end(),
                                                   [&](const omni::Member &member) {
                                                       return member.endpoint_id == endpoint.id;
                                                   });
        const bool primary = lite_mode_ && endpoint.id == default_render_id_;
        auto row = ListViewItem();
        row.Tag(box_value(winrt::hstring(omni::wide(endpoint.id))));
        row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        row.Padding({12, 10, 12, 10});
        row.MinHeight(56);
        const auto content = RootPanel()
                                 .Resources()
                                 .Lookup(box_value(L"DeviceRowTemplate"))
                                 .as<DataTemplate>()
                                 .LoadContent()
                                 .as<Grid>();
        const auto check = content.FindName(L"MembershipCheck").as<CheckBox>();
        const auto icon = content.FindName(L"DeviceGlyph").as<FontIcon>();
        icon.Glyph(endpoint.bluetooth ? L"\xE702" : L"\xE767");
        const auto name = content.FindName(L"DeviceName").as<TextBlock>();
        name.Text(winrt::hstring(omni::wide(endpoint.name)));
        const auto default_badge = content.FindName(L"DefaultBadge").as<TextBlock>();
        default_badge.Text(T(L"Windows default output"));
        default_badge.Visibility(primary ? Visibility::Visible : Visibility::Collapsed);
        const auto metadata = content.FindName(L"DeviceMetadata").as<TextBlock>();
        std::wstring detail;
        if (!endpoint.active)
            detail = T(L"현재 사용 불가");
        if (counts[endpoint.name] > 1)
            detail += (detail.empty() ? L"" : L" · ") + std::wstring(T(L"장치 ")) +
                      omni::wide(endpoint.id.substr(endpoint.id.size() > 9 ? endpoint.id.size() - 9 : 0));
        metadata.Text(detail);
        metadata.Visibility(detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        row.Content(content);
        Automation::AutomationProperties::SetName(
            row, winrt::hstring((primary ? T(L"Windows 기본 출력 · ") : L"") +
                                endpoint_label(endpoint, counts[endpoint.name] > 1)));
        Automation::AutomationProperties::SetName(check,
                                                  T(L"추가 출력에 포함 · ") + omni::wide(endpoint.name));
        ToolTipService::SetToolTip(row, box_value(winrt::hstring(omni::wide(
                                            endpoint.name + "\n" + endpoint.instance + "\n" + endpoint.id))));
        if (!endpoint.active)
            ToolTipService::SetToolTip(
                check,
                box_value(winrt::hstring(
                    T(L"Windows에서 활성 출력으로 인식되지 않은 기기입니다. 연결·전원과 Windows 소리 설정의 "
                      L"사용 상태를 확인하세요. 저장된 구성원은 체크를 해제해 제거할 수 있습니다."))));
        check.IsChecked(included && !primary);
        check.IsEnabled(!primary && (endpoint.active || included));
        if (primary)
            ToolTipService::SetToolTip(
                check, box_value(winrt::hstring(
                           T(L"Windows에서 직접 재생 중인 기본 출력은 복제 대상으로 선택할 수 없습니다."))));
        check.Checked(
            [this, id = endpoint.id](auto const &, auto const &) { device_membership_changed(id, true); });
        check.Unchecked(
            [this, id = endpoint.id](auto const &, auto const &) { device_membership_changed(id, false); });
        DevicesList().Items().Append(row);
        if (endpoint.id == selected_endpoint_id_)
            selected = static_cast<int>(i);
    }
    if (selected < 0 && group && !group->members.empty())
    {
        const auto first = std::find_if(group->members.begin(), group->members.end(), [this](const auto &m) {
            return !lite_mode_ || m.endpoint_id != default_render_id_;
        });
        if (first != group->members.end())
            selected_endpoint_id_ = first->endpoint_id;
        for (size_t i = 0; i < visible.size(); ++i)
            if (visible[i]->id == selected_endpoint_id_)
                selected = static_cast<int>(i);
    }
    if (selected < 0 && !visible.empty())
        selected = 0;
    DevicesList().SelectedIndex(selected);
    if (selected >= 0)
        selected_endpoint_id_ = visible[static_cast<size_t>(selected)]->id;
    else
        selected_endpoint_id_.clear();
    filling_ = false;
}

void MainWindow::device_membership_changed(const std::string &id, bool checked)
{
    if (filling_ || (lite_mode_ && id == default_render_id_))
        return;
    auto *group = current_group();
    if (!group)
        return;
    auto member = std::find_if(group->members.begin(), group->members.end(),
                               [&](const auto &m) { return m.endpoint_id == id; });
    if (checked && member == group->members.end())
    {
        omni::Member added{id, 1, 0, {}};
        if (!lite_mode_)
        {
            static const double angles[]{-30, 30, -150, 150, 0, -90, 90, 180};
            added.azimuth_deg = angles[group->members.size() % 8];
        }
        added.follow_reference_volume =
            (!lite_mode_ && group->members.empty()) ||
            (AllFollowVolumeCheck().IsChecked() && AllFollowVolumeCheck().IsChecked().Value());
        group->members.push_back(std::move(added));
    }
    else if (!checked && member != group->members.end())
        group->members.erase(member);
    else
        return;
    selected_endpoint_id_ = id;
    for (uint32_t i = 0; i < DevicesList().Items().Size(); ++i)
        if (omni::utf8(
                DevicesList().Items().GetAt(i).as<ListViewItem>().Tag().as<winrt::hstring>().c_str()) == id)
            DevicesList().SelectedIndex(static_cast<int>(i));
    mark_dirty();
    show_selection();
}

void MainWindow::update_bulk_volume()
{
    unsigned members = 0, following = 0;
    if (const auto *group = current_group())
        for (const auto &member : group->members)
            if (!lite_mode_ || member.endpoint_id != default_render_id_)
            {
                ++members;
                following += member.follow_reference_volume;
            }
    const bool previous = filling_;
    filling_ = true;
    if (members && following == members)
        AllFollowVolumeCheck().IsChecked(true);
    else if (following)
        AllFollowVolumeCheck().IsChecked(nullptr);
    else
        AllFollowVolumeCheck().IsChecked(false);
    AllFollowVolumeCheck().IsEnabled(service_available_ && loaded_ && !busy_ && members != 0);
    filling_ = previous;
}

void MainWindow::AllFollowVolume_Click(IInspectable const &, RoutedEventArgs const &)
{
    if (filling_ || busy_ || !current_group())
        return;
    auto &members = current_group()->members;
    const bool enabled = !std::all_of(members.begin(), members.end(), [this](const auto &member) {
        return (lite_mode_ && member.endpoint_id == default_render_id_) || member.follow_reference_volume;
    });
    for (auto &member : members)
        if (!lite_mode_ || member.endpoint_id != default_render_id_)
            member.follow_reference_volume = enabled;
    mark_dirty(true);
    show_selection();
}

void MainWindow::populate_microphones()
{
    std::string previous;
    if (MicrophonePicker().SelectedItem())
    {
        const auto selected = MicrophonePicker().SelectedItem().as<ComboBoxItem>();
        if (selected.Tag())
            previous = omni::utf8(selected.Tag().as<winrt::hstring>().c_str());
    }
    filling_ = true;
    MicrophonePicker().Items().Clear();
    int selected = -1;
    for (size_t i = 0; i < microphones_.size(); ++i)
    {
        const auto &microphone = microphones_[i];
        auto item = ComboBoxItem();
        item.Content(box_value(winrt::hstring(omni::wide(microphone.name))));
        item.Tag(box_value(winrt::hstring(omni::wide(microphone.id))));
        MicrophonePicker().Items().Append(item);
        if (microphone.id == previous)
            selected = static_cast<int>(i);
    }
    MicrophonePicker().SelectedIndex(selected >= 0 ? selected : (microphones_.empty() ? -1 : 0));
    filling_ = false;
}

void MainWindow::show_selection()
{
    filling_ = true;
    const auto *group = current_group();
    auto endpoint = std::find_if(outputs_.begin(), outputs_.end(),
                                 [this](const omni::Endpoint &ep) { return ep.id == selected_endpoint_id_; });
    SelectedDeviceText().Text(endpoint == outputs_.end() ? winrt::hstring(T(L"장치를 선택하세요"))
                                                         : winrt::hstring(omni::wide(endpoint->name)));
    const winrt::hstring name = group ? winrt::hstring(omni::wide(group->name)) : winrt::hstring{};
    if (GroupNameBox().Text() != name)
        GroupNameBox().Text(name);

    const bool primary =
        lite_mode_ && !selected_endpoint_id_.empty() && selected_endpoint_id_ == default_render_id_;
    if (auto *member = current_member(); member && !primary)
    {
        MemberGainSlider().Value(member->follow_reference_volume ? reference_percent(member->gain * 100)
                                                                 : member->gain * 100);
        FollowVolumeCheck().IsChecked(member->follow_reference_volume);
        MemberDelayBox().Value(member->delay_ms);
    }
    else
    {
        MemberGainSlider().Value(primary ? reference_percent(100.0) : 100.0);
        FollowVolumeCheck().IsChecked(false);
        MemberDelayBox().Value(0);
    }
    SelectedDeviceHintText().Text(
        primary ? T(L"Windows에서 직접 재생하는 기본 출력입니다. 볼륨은 Windows 소리 설정에서 조절하세요.")
                : T(L"왼쪽 체크박스를 켜면 이 장치로 소리를 복제합니다."));
    SelectedDeviceHintText().Visibility(primary || (!selected_endpoint_id_.empty() && !current_member())
                                            ? Visibility::Visible
                                            : Visibility::Collapsed);
    filling_ = false;
    if (!lite_mode_)
    {
        filling_ = true;
        if (group)
        {
            ProcessingPicker().SelectedIndex(group->output_processing == "system" ? 1 : 0);
            SafetyBufferBox().Value(group->safety_ms);
        }
        static const char *routes[]{"stereo",    "swap",       "mono",        "left",      "right",
                                    "position",  "front-left", "front-right", "center",    "lfe",
                                    "back-left", "back-right", "side-left",   "side-right"};
        if (auto *member = current_member())
        {
            for (int i = 0; i < 14; ++i)
                if (member->routing == routes[i])
                    RoutingPicker().SelectedIndex(i);
            AzimuthBox().Value(member->azimuth_deg);
            DistanceBox().Value(member->distance_m);
        }
        filling_ = false;
        populate_layout();
    }
    update_controls();
}

void MainWindow::mark_dirty(bool live)
{
    dirty_ = true;
    if (live && !lite_mode_)
        live_pending_ = true;
    else
        manual_draft_ = true;
    update_controls();
}

void MainWindow::update_controls()
{
    update_bulk_volume();
    const bool ready = service_available_ && loaded_ && !busy_;
    UnsavedText().Visibility(dirty_ ? Visibility::Visible : Visibility::Collapsed);
    StartupToggle().IsEnabled(ready);
    AddGroupButton().IsEnabled(ready && config_.groups.size() < omni::max_groups);
    RemoveGroupButton().IsEnabled(ready && current_group());
    GroupsList().IsEnabled(ready);
    GroupNameBox().IsEnabled(ready && current_group());
    DevicesList().IsEnabled(ready && current_group());
    MoreDevicesButton().IsEnabled(ready);
    const bool has_member = ready && current_member();
    const bool direct_primary =
        lite_mode_ && current_member() && current_member()->endpoint_id == default_render_id_;
    MemberGainSlider().IsEnabled(has_member && !direct_primary && !current_member()->follow_reference_volume);
    FollowVolumeCheck().IsEnabled(has_member && !direct_primary);
    MemberDelayBox().IsEnabled(has_member && !direct_primary);
    RoutingPicker().IsEnabled(has_member && !lite_mode_);
    const bool positioning = has_member && !lite_mode_ && current_member()->routing == "position";
    AzimuthBox().IsEnabled(positioning);
    DistanceBox().IsEnabled(positioning);
    ProcessingPicker().IsEnabled(ready && current_group());
    SafetyBufferBox().IsEnabled(ready && current_group());
    ApplyLiveButton().IsEnabled(ready && dirty_);
    const auto engine = engine_status_.value("engine", omni::Json::object());
    const auto engine_state = engine.value("state", std::string{});
    const bool engine_ready = !engine_state.empty() && engine_state != "disconnected" &&
                              engine_state != "service-unavailable" &&
                              engine_state != "configuration-unavailable";
    IdentifyButton().IsEnabled(
        engine_ready && current_member() && current_member()->endpoint_id.size() > 0 &&
        std::any_of(outputs_.begin(), outputs_.end(), [&](const omni::Endpoint &endpoint) {
            return endpoint.id == selected_endpoint_id_ && endpoint.active;
        }));
    const auto mirror_count =
        current_group() ? std::count_if(current_group()->members.begin(), current_group()->members.end(),
                                        [this](const omni::Member &member) {
                                            return !lite_mode_ || member.endpoint_id != default_render_id_;
                                        })
                        : 0;
    CalibrateButton().IsEnabled(
        ready && engine_ready && current_group() && mirror_count >= 2 &&
        (!lite_mode_ || !default_render_id_.empty()) && !microphones_.empty() &&
        engine_status_.value("job", omni::Json::object()).value("state", std::string("idle")) != "running");
    CancelCalibrationButton().IsEnabled(
        engine_status_.value("job", omni::Json::object()).value("state", std::string{}) == "running");
    MicrophonePicker().IsEnabled(ready && !microphones_.empty());
    SaveButton().IsEnabled(ready && dirty_);
    RefreshButton().IsEnabled(!busy_);
    WindowsSoundButton().IsEnabled(true);
}

void MainWindow::update_status()
{
    if (!service_available_)
    {
        StatusText().Text(lite_mode_
                              ? T(L"Lite 설정 파일을 읽지 못했습니다. 설치와 사용자 권한을 확인하세요.")
                              : T(L"관리 서비스 연결 실패 — 설치 상태를 확인하고 새로 고침하세요."));
        CalibrationStatusText().Text(lite_mode_
                                         ? T(L"설정 파일을 읽을 수 있으면 기기와 동기화 상태가 표시됩니다.")
                                         : T(L"서비스가 연결되면 그룹과 보정 상태가 표시됩니다."));
        return;
    }

    const auto engine = engine_status_.value("engine", omni::Json::object());
    const auto job = engine_status_.value("job", omni::Json::object());
    if (!lite_mode_ && current_group())
    {
        unsigned input_channels = 2;
        bool raw = false;
        double buffer = 0;
        for (const auto &group : engine.value("groups", omni::Json::array()))
            if (group.value("id", std::string{}) == current_group()->id)
            {
                input_channels = group.value("inputChannels", 2u);
                for (const auto &member : group.value("members", omni::Json::array()))
                    if (member.value("endpointId", std::string{}) == selected_endpoint_id_)
                    {
                        raw = member.value("rawOutput", false);
                        buffer = member.value("bufferDelayMs", 0.0);
                    }
            }
        auto endpoint = std::find_if(outputs_.begin(), outputs_.end(),
                                     [this](const auto &e) { return e.id == selected_endpoint_id_; });
        if (endpoint != outputs_.end())
        {
            wchar_t text[300]{};
            swprintf_s(
                text,
                T(L"48 kHz float32 processing · %u input channels · Windows mix %u Hz / %u-bit / %u channels")
                    .c_str(),
                input_channels, endpoint->mix_rate, endpoint->mix_bits, endpoint->mix_channels);
            QualityStatusText().Text(std::wstring(text) + L" · " +
                                     T(raw ? L"RAW output" : L"Windows processing") + L" · " +
                                     std::to_wstring(static_cast<int>(buffer)) + L" ms");
        }
    }
    const auto state = engine.value("state", std::string{});
    filling_ = true;
    StartupToggle().IsOn(engine.value("startWithWindows", true));
    if (current_member() && current_member()->follow_reference_volume &&
        (!lite_mode_ || selected_endpoint_id_ != default_render_id_))
        MemberGainSlider().Value(reference_percent(current_member()->gain * 100));
    filling_ = false;
    if (desktop_)
        desktop_->tooltip(L"OmniSound · " +
                          omni::wide(omni::active_group_id(config_).empty() ? std::string("대기") : [&] {
                              for (const auto &group : config_.groups)
                                  if (group.id == omni::active_group_id(config_))
                                      return group.name;
                              return std::string("대기");
                          }()));
    if (lite_mode_)
    {
        if (state == "configuration-unavailable")
            StatusText().Text(std::wstring(T(L"Lite 엔진의 설정 파일 연결 실패 · ")) +
                              omni::wide(engine.value("message", std::string{})));
        else if (state.empty() || state == "disconnected")
            StatusText().Text(T(L"OmniSound Lite · Windows 기본 출력 미러링 엔진 연결 대기"));
        else
        {
            unsigned playing = 0, waiting = 0;
            for (const auto &group : engine.value("groups", omni::Json::array()))
                for (const auto &member : group.value("members", omni::Json::array()))
                    if (member.value("state", std::string{}) == "playing")
                        ++playing;
                    else
                        ++waiting;
            StatusText().Text(std::to_wstring(playing) + T(L"개 출력 · ") + std::to_wstring(waiting) +
                              T(L"개 대기"));
        }
    }
    else if (state.empty() || state == "disconnected")
    {
        const auto reason = job.value("message", std::string{});
        StatusText().Text(reason.empty() ? T(L"관리 서비스 연결됨 · 오디오 엔진 연결 대기")
                                         : std::wstring(T(L"오디오 엔진 연결 대기 · ")) + omni::wide(reason));
    }
    else if (state == "service-unavailable")
        StatusText().Text(T(L"오디오 엔진이 관리 서비스에 연결되지 않았습니다."));
    else
        StatusText().Text(T(L"관리 서비스와 오디오 엔진 연결됨"));

    const auto job_state = job.value("state", std::string{});
    const bool selected_calibration = job.value("type", std::string{}) == "calibrate" && current_group() &&
                                      job.value("groupId", std::string{}) == current_group()->id;
    if (job_state == "running" && selected_calibration)
    {
        auto progress = job.value("progress", std::string("Preparing…"));
        for (const auto prefix : {"Measuring ", "Verifying "})
            if (progress.starts_with(prefix))
            {
                progress = omni::i18n::text(prefix) + progress.substr(std::char_traits<char>::length(prefix));
                break;
            }
        CalibrationStatusText().Text(omni::wide(omni::i18n::text(progress)));
        return;
    }
    if (job_state == "error" && selected_calibration)
    {
        CalibrationStatusText().Text(std::wstring(T(L"보정 실패 · ")) +
                                     omni::wide(job.value("message", std::string("원인 미상"))));
        return;
    }
    if (lite_mode_ && job_state == "complete" && job.contains("result") && current_group() &&
        job.at("result").value("group", omni::Json::object()).value("id", std::string{}) ==
            current_group()->id)
    {
        const auto result = job.at("result");
        const bool passed = result.value("passed", false);
        const double spread = result.value("maxSpreadMs", 0.0);
        const double uncertainty = result.value("uncertaintyMs", 0.0);
        wchar_t buffer[240]{};
        if (lite_mode_)
            swprintf_s(buffer,
                       T(L"복제 기기 보정 %s · 실측 편차 %.2f ms · 불확실성 %.2f ms · Windows 기본 출력 제외")
                           .c_str(),
                       (passed ? T(L"통과") : T(L"미달")).c_str(), spread, uncertainty);
        else
            swprintf_s(buffer, T(L"%s · 마지막 실측 편차 %.2f ms · 불확실성 %.2f ms").c_str(),
                       (passed ? T(L"보정 목표 통과") : T(L"보정 목표 미달")).c_str(), spread, uncertainty);
        CalibrationStatusText().Text(buffer);
        return;
    }
    if (const auto *group = current_group())
    {
        for (const auto &status : engine.value("groups", omni::Json::array()))
        {
            if (status.value("id", std::string{}) != group->id)
                continue;
            const auto group_state = status.value("state", std::string{});
            if (group_state == "driver-unavailable")
                CalibrationStatusText().Text(
                    T(L"가상 출력 드라이버가 없습니다. 가상 출력 상태를 확인하세요."));
            else if (group_state == "source-unavailable")
                CalibrationStatusText().Text(T(
                    L"Lite의 Windows 기본 출력을 찾지 못했습니다. 실제 출력 기기를 기본값으로 선택하세요."));
            else if (group_state == "direct-only")
                CalibrationStatusText().Text(T(L"추가 출력 없음 · Windows 기본 출력으로만 재생합니다."));
            else if (group_state == "no-outputs")
                CalibrationStatusText().Text(T(L"출력 기기 없음 · 동기화하지 않습니다."));
            else if (lite_mode_)
            {
                double delay = status.value("latencyMs", 0.0);
                for (const auto &member : status.value("members", omni::Json::array()))
                    if (member.value("endpointId", std::string{}) == selected_endpoint_id_)
                        delay = member.value("bufferDelayMs", delay);
                wchar_t buffer[180]{};
                swprintf_s(buffer, T(L"엔진 버퍼 지연 %.1f ms · 실측 아님 · 장치 내부 지연 제외").c_str(),
                           delay);
                CalibrationStatusText().Text(buffer);
            }
            else if (group_state == "error" || group_state == "disconnected")
                CalibrationStatusText().Text(
                    T(L"Output unavailable · check connection and Windows sound settings."));
            else
            {
                bool measured = false, correction_seen = false, valid = !group->members.empty();
                double spread = 0, min_correction = 0, max_correction = 0;
                std::string measured_at;
                for (const auto &member : status.value("members", omni::Json::array()))
                {
                    valid &= member.value("calibrationValid", false);
                    const auto correction = member.value("estimatedCorrectionMs", 0.0);
                    if (!correction_seen)
                        min_correction = max_correction = correction;
                    correction_seen = true;
                    min_correction = std::min(min_correction, correction);
                    max_correction = std::max(max_correction, correction);
                    if (member.contains("lastMeasurement"))
                    {
                        const auto &last = member.at("lastMeasurement");
                        measured = true;
                        spread = std::max(spread, last.value("verifiedSpreadMs", 0.0));
                        if (measured_at.empty())
                            measured_at = last.value("measuredAt", std::string{});
                    }
                }
                wchar_t estimate[240]{};
                swprintf_s(
                    estimate,
                    T(L"Estimated clock correction spread %.2f ms · not an acoustic measurement").c_str(),
                    max_correction - min_correction);
                std::wstring message = estimate;
                if (measured)
                {
                    wchar_t last[200]{};
                    swprintf_s(last, T(L"Last measured spread %.2f ms").c_str(), spread);
                    message += L"\n" + std::wstring(last) + L" · " + omni::wide(measured_at) + L" · " +
                               T(valid ? L"Calibration current" : L"Recalibration required");
                }
                else
                    message += L"\n" + T(L"No acoustic measurement · run automatic calibration.");
                CalibrationStatusText().Text(message);
            }
            return;
        }
    }
    CalibrationStatusText().Text(T(L"보정 전 · 자동 보정은 지정된 마이크 위치에서 실제 출력을 측정합니다."));
}

void MainWindow::AddGroup_Click(IInspectable const &, RoutedEventArgs const &)
{
    if (!service_available_ || config_.groups.size() >= omni::max_groups)
        return;
    std::string name = omni::i18n::text("New group");
    unsigned suffix = 1;
    while (std::any_of(config_.groups.begin(), config_.groups.end(),
                       [&](const omni::Group &group) { return group.name == name; }))
        name = omni::i18n::text("New group") + " " + std::to_string(++suffix);
    config_.groups.push_back({omni::new_id(), std::move(name), {}, 0});
    selected_group_id_ = config_.groups.back().id;
    selected_endpoint_id_.clear();
    mark_dirty();
    populate_groups();
    populate_devices();
    show_selection();
}

void MainWindow::RemoveGroup_Click(IInspectable const &, RoutedEventArgs const &)
{
    auto *group = current_group();
    if (!service_available_ || !group)
        return;
    const auto removed_id = group->id;
    std::erase_if(config_.groups, [&](const omni::Group &item) { return item.id == removed_id; });
    selected_group_id_ = config_.groups.empty() ? std::string{} : config_.groups.front().id;
    if (config_.active_group_id == removed_id)
        config_.active_group_id = selected_group_id_;
    selected_endpoint_id_.clear();
    mark_dirty();
    populate_groups();
    populate_devices();
    show_selection();
}

void MainWindow::GroupsList_SelectionChanged(IInspectable const &,
                                             Controls::SelectionChangedEventArgs const &)
{
    if (filling_ || !GroupsList().SelectedItem())
        return;
    const auto item = GroupsList().SelectedItem().as<ListViewItem>();
    const auto selected = omni::utf8(item.Tag().as<winrt::hstring>().c_str());
    if (selected == selected_group_id_)
        return;
    selected_group_id_ = selected;
    selected_endpoint_id_.clear();
    populate_devices();
    show_selection();
    activate_selection();
}

void MainWindow::DevicesList_SelectionChanged(IInspectable const &,
                                              Controls::SelectionChangedEventArgs const &)
{
    if (filling_)
        return;
    const auto selected =
        DevicesList().SelectedItem()
            ? omni::utf8(DevicesList().SelectedItem().as<ListViewItem>().Tag().as<winrt::hstring>().c_str())
            : std::string{};
    if (selected == selected_endpoint_id_)
        return;
    selected_endpoint_id_ = selected;
    show_selection();
}

void MainWindow::MoreDevices_Click(IInspectable const &, RoutedEventArgs const &)
{
    const auto previous = selected_endpoint_id_;
    show_unavailable_ = !show_unavailable_;
    populate_devices();
    if (selected_endpoint_id_ != previous)
        show_selection();
    update_controls();
}

void MainWindow::GroupName_TextChanged(IInspectable const &, Controls::TextChangedEventArgs const &)
{
    if (filling_)
        return;
    if (auto *group = current_group())
    {
        const auto name = omni::utf8(GroupNameBox().Text().c_str());
        if (group->name == name)
            return;
        group->name = name;
        mark_dirty();
        // Update the existing row without rebuilding selection or touching the
        // editor. Reassigning Text here also breaks caret/IME composition.
        if (GroupsList().SelectedItem())
            GroupsList().SelectedItem().as<ListViewItem>().Content(box_value(GroupNameBox().Text()));
    }
}

void MainWindow::Identify_Click(IInspectable const &, RoutedEventArgs const &)
{
    if (selected_endpoint_id_.empty())
        return;
    StatusText().Text(T(L"확인음을 재생하는 중…"));
    invoke([controller = controller_, endpoint = selected_endpoint_id_](auto completion) {
        controller->test(endpoint, std::move(completion));
    });
}

void MainWindow::Refresh_Click(IInspectable const &, RoutedEventArgs const &)
{
    refresh();
}

void MainWindow::Calibrate_Click(IInspectable const &, RoutedEventArgs const &)
{
    const auto *group = current_group();
    const int index = MicrophonePicker().SelectedIndex();
    const auto mirror_count =
        group ? std::count_if(group->members.begin(), group->members.end(),
                              [this](const omni::Member &member) {
                                  return !lite_mode_ || member.endpoint_id != default_render_id_;
                              })
              : 0;
    if (!group || index < 0 || index >= static_cast<int>(microphones_.size()) || mirror_count < 2)
    {
        StatusText().Text(lite_mode_ ? T(L"Lite 자동 보정은 Windows 기본 출력 외에 복제할 기기 두 개 이상과 "
                                         L"PC 또는 USB 마이크가 필요합니다.")
                                     : T(L"두 개 이상의 출력 기기와 PC 또는 USB 마이크를 선택하세요."));
        return;
    }
    const auto group_id = group->id;
    const auto microphone_id = microphones_[static_cast<size_t>(index)].id;
    if (dirty_)
    {
        StatusText().Text(T(L"먼저 그룹 변경을 저장한 뒤 보정하세요."));
        return;
    }
    auto copy = config_;
    try
    {
        omni::validate(copy);
    }
    catch (const std::exception &e)
    {
        StatusText().Text(omni::wide(e.what()));
        return;
    }
    StatusText().Text(T(L"오디오 엔진에 자동 보정을 요청하는 중…"));
    invoke([controller = controller_, group_id, microphone_id](auto completion) {
        controller->calibrate(group_id, microphone_id, std::move(completion));
    });
}

void MainWindow::CancelCalibration_Click(IInspectable const &, RoutedEventArgs const &)
{
    invoke([controller = controller_](auto completion) { controller->cancel({}, std::move(completion)); });
}

void MainWindow::Save_Click(IInspectable const &, RoutedEventArgs const &)
{
    if (lite_mode_)
        config_.active_group_id = selected_group_id_;
    if (auto *group = current_group())
        group->name = omni::utf8(GroupNameBox().Text().c_str());
    try
    {
        omni::validate(config_);
    }
    catch (const std::exception &e)
    {
        StatusText().Text(omni::wide(e.what()));
        return;
    }
    StatusText().Text(lite_mode_ ? T(L"Lite 그룹 설정을 이 사용자 계정에 저장하는 중…")
                                 : T(L"서비스에 그룹 설정을 저장하는 중…"));
    invoke([controller = controller_, config = config_](auto completion) mutable {
        controller->save(std::move(config), std::move(completion));
    });
}

void MainWindow::WindowsSound_Click(IInspectable const &, RoutedEventArgs const &)
{
    ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::Close_Click(IInspectable const &, RoutedEventArgs const &)
{
    HideToTray();
}
void MainWindow::HideToTray()
{
    desktop_->hide();
}
void MainWindow::activate_selection()
{
    if (!lite_mode_ || !controller_ || selected_group_id_.empty())
        return;
    invoke([controller = controller_, id = selected_group_id_](auto completion) {
        controller->activate(id, std::move(completion));
    });
}
void MainWindow::Startup_Toggled(IInspectable const &, RoutedEventArgs const &)
{
    if (filling_ || !controller_)
        return;
    invoke([controller = controller_, enabled = StartupToggle().IsOn()](auto completion) {
        controller->startup(enabled, std::move(completion));
    });
}
void MainWindow::Exit_Click(IInspectable const &, RoutedEventArgs const &)
{
    if (quitting_)
        return;
    if (timer_)
        timer_.Stop();
    if (live_timer_)
        live_timer_.Stop();
    quitting_ = true;
    const auto weak = get_weak();
    const auto queue = dispatcher_;
    if (!controller_)
    {
        desktop_->remove_tray();
        Close();
        return;
    }
    controller_->shutdown([weak, queue](auto) {
        queue.TryEnqueue([weak] {
            if (auto self = weak.get())
            {
                self->desktop_->remove_tray();
                self->Close();
            }
        });
    });
}
void MainWindow::tray_action(unsigned action)
{
    if (action == 2)
    {
        filling_ = true;
        StartupToggle().IsOn(!StartupToggle().IsOn());
        filling_ = false;
        Startup_Toggled(nullptr, RoutedEventArgs{nullptr});
    }
    if (action == 3)
        WindowsSound_Click(nullptr, RoutedEventArgs{nullptr});
    if (action == 4)
        Exit_Click(nullptr, RoutedEventArgs{nullptr});
    if (action >= 100 && action - 100 < config_.groups.size() && !busy_)
        GroupsList().SelectedIndex(static_cast<int>(action - 100));
}
int MainWindow::DesktopSmokeTest()
{
    HWND window{};
    check_hresult(get_strong().as<IWindowNative>()->get_WindowHandle(&window));
    if (!desktop_->has_tray())
        return 30;
    SendMessageW(window, WM_CLOSE, 0, 0);
    if (!IsWindow(window) || IsWindowVisible(window) || !desktop_->has_tray())
        return 31;
    SendMessageW(window, DesktopShell::restore_message(), 0, 0);
    if (!IsWindowVisible(window))
        return 32;
    desktop_->remove_tray();
    return 0;
}
Windows::Foundation::IAsyncAction MainWindow::VisualSmoke(std::wstring path)
{
    try
    {
        lite_mode_ = true;
        ModeBadgeText().Text(L"LITE");
        ProPanel().Visibility(Visibility::Collapsed);
        localize_ui();
        AppWindow().Resize({1020, 920});
        omni::ui::Snapshot snapshot;
        snapshot.service_available = true;
        const auto id = omni::new_id();
        snapshot.config.groups = {{id, "책상", {{"aw", 0.7, 0, {}}, {"realtek", 0.7, 0, {}}}, 200},
                                  {omni::new_id(), "거실", {{"realtek", 0.5, 0, {}}}, 200}};
        snapshot.config.active_group_id = id;
        snapshot.outputs = {
            {"dell", "DELL S2725DS (NVIDIA High Definition Audio) · DisplayPort 모니터 오디오 출력",
             "DP · NVIDIA", "", "", false, false, true},
            {"dell-other", "DELL S2725DS (NVIDIA High Definition Audio) · DisplayPort 모니터 오디오 출력",
             "DP · NVIDIA", "", "", false, false, true},
            {"aw", "AW2521HFL (HD Audio Driver for Display Audio)", "HDMI · Intel", "", "", false, false,
             true},
            {"realtek", "스피커 (Realtek(R) Audio) · USB 연결 출력 장치", "", "", "", false, false, true}};
        snapshot.default_render_id = "dell";
        snapshot.engine_status = {{"engine",
                                   {{"state", "running"},
                                    {"activeGroupId", id},
                                    {"startWithWindows", true},
                                    {"referenceVolumePercent", 50.0},
                                    {"groups",
                                     {{{"id", id},
                                       {"state", "playing"},
                                       {"members", {{{"state", "playing"}}, {{"state", "playing"}}}}}}}}},
                                  {"job", {{"state", "idle"}}}};
        apply_snapshot(snapshot);
        winrt::apartment_context ui;
        co_await winrt::resume_after(std::chrono::milliseconds(350));
        co_await ui;
        const std::filesystem::path base(path);
        static const ElementTheme themes[]{ElementTheme::Default, ElementTheme::Light, ElementTheme::Dark,
                                           ElementTheme::Default, ElementTheme::Light, ElementTheme::Dark,
                                           ElementTheme::Light,   ElementTheme::Dark,  ElementTheme::Light};
        for (unsigned preview = 0; preview < 9; ++preview)
        {
            const auto theme = themes[preview];
            const bool narrow = preview == 3 || preview == 7;
            AppWindow().Resize(narrow ? Windows::Graphics::SizeInt32{820, 1080}
                                      : Windows::Graphics::SizeInt32{1020, 1080});
            if (preview >= 4)
            {
                lite_mode_ = false;
                Title(L"OmniSound Pro");
                ModeBadgeText().Text(L"PRO");
                ProPanel().Visibility(Visibility::Visible);
                ProPanel().IsExpanded(true);
                snapshot.save_committed = true;
                snapshot.config.groups.front().safety_ms = 0;
                snapshot.config.groups.front().members[0].routing = "position";
                snapshot.config.groups.front().members[0].azimuth_deg = -30;
                snapshot.config.groups.front().members[1].azimuth_deg = 30;
                for (auto &device : snapshot.outputs)
                {
                    device.mix_rate = 48000;
                    device.mix_bits = 24;
                    device.mix_channels = 2;
                }
                snapshot.engine_status["engine"]["groups"][0]["inputChannels"] = 8;
                snapshot.engine_status["engine"]["groups"][0]["members"] = {{{"endpointId", "aw"},
                                                                             {"state", "playing"},
                                                                             {"rawOutput", true},
                                                                             {"bufferDelayMs", 18.0}},
                                                                            {{"endpointId", "realtek"},
                                                                             {"state", "playing"},
                                                                             {"rawOutput", true},
                                                                             {"bufferDelayMs", 18.0}}};
                apply_snapshot(snapshot);
                LanguagePicker().SelectedIndex(static_cast<int>(preview - 4));
                localize_ui();
                show_selection();
                update_status();
            }
            RootPanel().RequestedTheme(theme);
            co_await winrt::resume_after(std::chrono::milliseconds(200));
            co_await ui;
            RootPanel().UpdateLayout();
            if (preview >= 4)
            {
                WorkspaceScroll().ChangeView(
                    nullptr, box_value(550.0).as<Windows::Foundation::IReference<double>>(), nullptr);
                co_await winrt::resume_after(std::chrono::milliseconds(150));
                co_await ui;
            }
            // The name's hit area must select its row, never overlap the checkbox.
            const auto row = DevicesList().Items().GetAt(0).as<ListViewItem>();
            const auto content = row.Content().as<Grid>();
            const auto check = content.FindName(L"MembershipCheck").as<CheckBox>();
            const auto label = content.FindName(L"DeviceName").as<TextBlock>();
            if (check.ActualWidth() > 40 || label.ActualWidth() <= check.ActualWidth())
                throw std::runtime_error("Device checkbox consumes the row hit area");
            const auto point = label.TransformToVisual(RootPanel()).TransformPoint({20, 8});
            for (const auto &hit : Media::VisualTreeHelper::FindElementsInHostCoordinates(point, RootPanel()))
                if (hit.try_as<CheckBox>())
                    throw std::runtime_error("Device name hit toggles a checkbox");
            if (narrow && label.ActualHeight() < 30)
                throw std::runtime_error("Long device name did not wrap at the minimum window width");
            auto bitmap = Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap();
            co_await bitmap.RenderAsync(RootPanel());
            const auto pixels = co_await bitmap.GetPixelsAsync();
            auto reader = Windows::Storage::Streams::DataReader::FromBuffer(pixels);
            std::vector<uint8_t> data(pixels.Length());
            reader.ReadBytes(data);
            auto output = base;
            if (theme != ElementTheme::Default)
                output = base.parent_path() / (base.stem().wstring() +
                                               (theme == ElementTheme::Light ? L"-light.png" : L"-dark.png"));
            if (narrow)
                output = base.parent_path() / (base.stem().wstring() + L"-narrow.png");
            if (preview >= 4)
                output = base.parent_path() /
                         (base.stem().wstring() + L"-pro-" + omni::wide(omni::i18n::language()) +
                          (narrow ? L"-narrow.png" : L".png"));
            std::filesystem::create_directories(output.parent_path());
            auto folder = co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(
                output.parent_path().wstring());
            auto file = co_await folder.CreateFileAsync(
                output.filename().wstring(), Windows::Storage::CreationCollisionOption::ReplaceExisting);
            auto stream = co_await file.OpenAsync(Windows::Storage::FileAccessMode::ReadWrite);
            auto encoder = co_await Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
                Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);
            encoder.SetPixelData(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                                 Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,
                                 bitmap.PixelWidth(), bitmap.PixelHeight(), 96, 96, data);
            co_await encoder.FlushAsync();
            stream.Close();
        }
        RootPanel().RequestedTheme(ElementTheme::Default);
        desktop_->remove_tray();
        ExitProcess(0);
    }
    catch (...)
    {
        desktop_->remove_tray();
        ExitProcess(40);
    }
    co_return;
}
} // namespace winrt::OmniSound::implementation

// The native XAML compiler emits the page's template implementations as a
// header. Include it in the translation unit that owns MainWindow so the
// generated event wiring and InitializeComponent are linked into the app.
#include "MainWindow.xaml.g.hpp"
