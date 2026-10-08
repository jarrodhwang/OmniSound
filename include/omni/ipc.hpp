#pragma once
#include <omni/platform.hpp>
#include <thread>
namespace omni
{
inline constexpr auto service_pipe = L"\\\\.\\pipe\\OmniSound.Service.v1";
std::wstring engine_pipe(bool lite = false);
struct Caller
{
    std::wstring sid;
    DWORD session{};
    DWORD pid{};
};
using RequestHandler = std::function<Json(const Json &, const Caller &)>;
class PipeServer
{
  public:
    PipeServer(std::wstring name, bool service, RequestHandler);
    ~PipeServer();
    PipeServer(const PipeServer &) = delete;

  private:
    void run();
    std::wstring name_;
    bool service_;
    RequestHandler handler_;
    Handle stop_;
    Handle ready_;
    std::string startup_error_;
    std::thread thread_;
};
Json request(const std::wstring &, const Json &, DWORD timeout_ms = 3000);
} // namespace omni
