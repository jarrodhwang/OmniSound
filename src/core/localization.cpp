#include <omni/localization.hpp>
#include <omni/model.hpp>
#include "strings.generated.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
namespace omni::i18n
{
namespace
{
constexpr std::array<const char *, 5> languages{"en", "ko", "fr", "de", "ja"};
std::atomic<unsigned> selected{};
const Json &catalog()
{
    static const Json entries = Json::parse(catalog_json);
    return entries;
}
} // namespace
std::string resolve_language(std::string_view locale)
{
    std::string code(locale.substr(0, locale.find_first_of("-_")));
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::find(languages.begin(), languages.end(), code) == languages.end() ? "en" : code;
}
void set_language(std::string_view locale)
{
    const auto code = resolve_language(locale);
    selected = static_cast<unsigned>(std::find(languages.begin(), languages.end(), code) - languages.begin());
}
std::string language()
{
    return languages[selected.load()];
}
std::string text(std::string_view source)
{
    const auto index = selected.load();
    for (const auto &entry : catalog())
        for (const auto &alias : entry)
            if (alias.get_ref<const std::string &>() == source)
                return entry[index].get<std::string>();
    return std::string(source); // Device names and technical diagnostics keep their original identity.
}
bool complete_catalog()
{
    for (const auto &entry : catalog())
        if (!entry.is_array() || entry.size() != languages.size() ||
            std::any_of(entry.begin(), entry.end(), [](const Json &v) {
                return !v.is_string() || v.get_ref<const std::string &>().empty();
            }))
            return false;
    return true;
}
} // namespace omni::i18n
