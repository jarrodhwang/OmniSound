#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <omni/model.hpp>
#include <filesystem>
#include <functional>
#include <atomic>
#include <stdexcept>
namespace omni
{
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
class Handle
{
    HANDLE h_{};

  public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle()
    {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&o) noexcept : h_(o.release()) {}
    Handle &operator=(Handle &&o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }
    void reset(HANDLE h = nullptr)
    {
        if (h_ && h_ != INVALID_HANDLE_VALUE)
            CloseHandle(h_);
        h_ = h;
    }
    HANDLE get() const
    {
        return h_;
    }
    HANDLE release()
    {
        auto h = h_;
        h_ = nullptr;
        return h;
    }
    explicit operator bool() const
    {
        return h_ && h_ != INVALID_HANDLE_VALUE;
    }
};
struct ComScope
{
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
        if (FAILED(hr))
            throw std::runtime_error("COM initialization failed");
    }
    ~ComScope()
    {
        CoUninitialize();
    }
};
class HResultError : public std::runtime_error
{
  public:
    HRESULT code;
    HResultError(HRESULT, const char *);
};
void check(HRESULT, const char *);
void check_win(bool, const char *);
std::wstring wide(std::string_view);
std::string utf8(std::wstring_view);
double qpc_seconds();
std::string utc_now();
std::string new_id();
std::wstring user_sid(HANDLE token = nullptr);
DWORD process_session();
std::filesystem::path executable_dir();
std::filesystem::path local_data();
std::filesystem::path machine_data();
bool is_lite_install();
bool login_start_enabled(bool lite);
std::wstring login_launch_command(bool lite);
void set_login_start(bool lite, bool enabled);
Config load_lite_config();
Config save_lite_config(Config, uint64_t expected_revision);
Json read_json(const std::filesystem::path &, Json fallback = Json::object());
void atomic_json(const std::filesystem::path &, const Json &);
void start_sibling(const wchar_t *filename, const wchar_t *arguments = L"");
void log_event(const std::string &event, const std::string &detail);
} // namespace omni
