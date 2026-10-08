#pragma once
#include <omni/platform.hpp>

namespace omni::setup
{
struct Options
{
    bool lite = true;
    bool uninstall{};
    bool local_test{};
    bool provision{};
    bool accepted{};
    bool smoke{};
    std::filesystem::path snapshots;
    std::filesystem::path target;
};
struct Result
{
    int code = 1;
    std::wstring message;
    std::filesystem::path log;
};
using Progress = std::function<void(int, std::wstring)>;
std::filesystem::path install_location(bool lite);
Result provision(HINSTANCE module, const Options &, Progress);
int show_wizard(HINSTANCE module, Options options);
} // namespace omni::setup
