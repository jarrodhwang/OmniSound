#pragma once
#include <omni/localization.hpp>
#include <omni/platform.hpp>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
inline std::wstring T(std::wstring_view source)
{
    return omni::wide(omni::i18n::text(omni::utf8(source)));
}
inline void translate_tree(winrt::Microsoft::UI::Xaml::DependencyObject const &node)
{
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using winrt::box_value;
    using winrt::hstring;
    using winrt::Windows::Foundation::IInspectable;
    const auto translate_box = [](IInspectable const &value) -> IInspectable {
        if (auto property = value.try_as<winrt::Windows::Foundation::IPropertyValue>();
            property && property.Type() == winrt::Windows::Foundation::PropertyType::String)
            return box_value(hstring(T(property.GetString().c_str())));
        return value;
    };
    if (auto element = node.try_as<FrameworkElement>())
    {
        const auto name = element.Name();
        if (name == L"GroupsList" || name == L"DevicesList" || name == L"SpeakerCanvas" ||
            name == L"SelectedDeviceText" || name == L"QualityStatusText" || name == L"StatusText" ||
            name == L"CalibrationStatusText")
            return;
        Automation::AutomationProperties::SetName(
            element, hstring(T(Automation::AutomationProperties::GetName(element).c_str())));
        ToolTipService::SetToolTip(element, translate_box(ToolTipService::GetToolTip(element)));
    }
    if (auto text = node.try_as<TextBlock>())
        text.Text(hstring(T(text.Text().c_str())));
    if (auto box = node.try_as<TextBox>())
        box.PlaceholderText(hstring(T(box.PlaceholderText().c_str())));
    if (auto content = node.try_as<ContentControl>())
        content.Content(translate_box(content.Content()));
    if (auto toggle = node.try_as<ToggleSwitch>())
        toggle.Header(translate_box(toggle.Header()));
    if (auto number = node.try_as<NumberBox>())
        number.Header(translate_box(number.Header()));
    if (auto combo = node.try_as<ComboBox>())
    {
        combo.Header(translate_box(combo.Header()));
        if (combo.Name() != L"MicrophonePicker" && combo.Name() != L"LanguagePicker")
            for (uint32_t i = 0; i < combo.Items().Size(); ++i)
                combo.Items().SetAt(i, translate_box(combo.Items().GetAt(i)));
        return;
    }
    if (auto expander = node.try_as<Expander>())
        expander.Header(translate_box(expander.Header()));
    if (auto panel = node.try_as<Panel>())
        for (auto child : panel.Children())
            translate_tree(child);
    else if (auto border = node.try_as<Border>(); border && border.Child())
        translate_tree(border.Child());
    else if (auto content = node.try_as<ContentControl>())
        if (auto child = content.Content().try_as<DependencyObject>())
            translate_tree(child);
    if (auto button = node.try_as<Button>())
        if (auto flyout = button.Flyout().try_as<Flyout>(); flyout && flyout.Content())
            translate_tree(flyout.Content());
}
