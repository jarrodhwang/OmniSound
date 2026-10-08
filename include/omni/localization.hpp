#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace omni::i18n
{
std::string resolve_language(std::string_view locale);
void set_language(std::string_view locale);
std::string language();
std::string text(std::string_view source);
bool complete_catalog();
} // namespace omni::i18n
