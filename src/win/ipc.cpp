#include <omni/ipc.hpp>
#include <sddl.h>
#include <array>
namespace omni
{
std::wstring engine_pipe(bool lite)
{
    return L"\\\\.\\pipe\\OmniSound.Engine." + std::wstring(lite ? L"Lite." : L"Driver.") + L"v1." +
           user_sid() + L"." + std::to_wstring(process_session());
}
struct Header
{
    uint32_t magic{0x494E4D4F}, version{1}, size{};
};
static bool finish(HANDLE file, OVERLAPPED &ov, HANDLE stop, DWORD timeout, DWORD &bytes)
{
    HANDLE waits[] = {ov.hEvent, stop};
    auto r = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeout);
    if (r != WAIT_OBJECT_0)
    {
        CancelIoEx(file, &ov);
        GetOverlappedResult(file, &ov, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(file, &ov, &bytes, FALSE) != 0;
}
static bool transfer(HANDLE h, void *data, DWORD count, bool write, HANDLE stop, DWORD timeout)
{
    auto *p = static_cast<BYTE *>(data);
    DWORD done = 0;
    const auto deadline = GetTickCount64() + timeout;
    while (done < count)
    {
        auto now = GetTickCount64();
        if (now >= deadline)
            return false;
        const auto remaining = static_cast<DWORD>(deadline - now);
        Handle e(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = e.get();
        DWORD n{};
        BOOL ok = write ? WriteFile(h, p + done, count - done, &n, &ov)
                        : ReadFile(h, p + done, count - done, &n, &ov);
        if (!ok && (GetLastError() != ERROR_IO_PENDING || !finish(h, ov, stop, remaining, n)))
            return false;
        if (!n)
            return false;
        done += n;
    }
    return true;
}
static Json receive(HANDLE h, HANDLE stop, DWORD timeout)
{
    Header head{};
    if (!transfer(h, &head, sizeof(head), false, stop, timeout))
        throw std::runtime_error("IPC read timeout or disconnected");
    if (head.magic != 0x494E4D4F || head.version != 1 || head.size == 0 || head.size > max_message_bytes)
        throw std::runtime_error("Invalid IPC frame");
    std::string s(head.size, '\0');
    if (!transfer(h, s.data(), head.size, false, stop, timeout))
        throw std::runtime_error("Incomplete IPC message");
    return Json::parse(s);
}
static void send(HANDLE h, const Json &j, HANDLE stop, DWORD timeout)
{
    auto s = j.dump();
    if (s.empty() || s.size() > max_message_bytes)
        throw std::runtime_error("IPC response too large");
    Header head;
    head.size = static_cast<uint32_t>(s.size());
    if (!transfer(h, &head, sizeof(head), true, stop, timeout) ||
        !transfer(h, s.data(), head.size, true, stop, timeout))
        throw std::runtime_error("IPC write timeout or disconnected");
}
Json request(const std::wstring &name, const Json &j, DWORD timeout)
{
    Handle pipe;
    ULONGLONG deadline = GetTickCount64() + timeout;
    while (!pipe)
    {
        auto now = GetTickCount64();
        if (now >= deadline)
            throw std::runtime_error("OmniSound service or engine is not running");
        if (!WaitNamedPipeW(name.c_str(), static_cast<DWORD>(deadline - now)))
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
            {
                Sleep(20);
                continue;
            }
            throw std::runtime_error("OmniSound service or engine is not running");
        }
        pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                               nullptr));
        if (!pipe && GetLastError() != ERROR_PIPE_BUSY)
            check_win(false, "Connect to OmniSound");
    }
    if (name == service_pipe)
    {
        ULONG server_pid{};
        check_win(GetNamedPipeServerProcessId(pipe.get(), &server_pid) != 0, "Identify management service");
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        check_win(manager != nullptr, "Open service manager");
        SC_HANDLE service = OpenServiceW(manager, L"OmniSound", SERVICE_QUERY_STATUS);
        if (!service)
        {
            CloseServiceHandle(manager);
            throw std::runtime_error("OmniSound management service is not installed");
        }
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes{};
        BOOL ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status),
                                       sizeof(status), &bytes);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (!ok || status.dwProcessId != server_pid || status.dwCurrentState != SERVICE_RUNNING)
            throw std::runtime_error("Management pipe does not belong to the registered OmniSound service");
    }
    send(pipe.get(), j, nullptr, timeout);
    auto result = receive(pipe.get(), nullptr, timeout);
    BYTE ack = 1;
    transfer(pipe.get(), &ack, 1, true, nullptr, timeout);
    if (result.contains("error"))
        throw std::runtime_error(result.at("error").get<std::string>());
    return result;
}
PipeServer::PipeServer(std::wstring name, bool service, RequestHandler handler)
    : name_(std::move(name)), service_(service), handler_(std::move(handler)),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      thread_([this] { run(); })
{
    if (WaitForSingleObject(ready_.get(), 5000) != WAIT_OBJECT_0 || !startup_error_.empty())
    {
        SetEvent(stop_.get());
        thread_.join();
        throw std::runtime_error(startup_error_.empty() ? "IPC server startup timed out" : startup_error_);
    }
}
PipeServer::~PipeServer()
{
    SetEvent(stop_.get());
    if (thread_.joinable())
        thread_.join();
}
static Caller caller(HANDLE pipe)
{
    ULONG pid;
    check_win(GetNamedPipeClientProcessId(pipe, &pid) != 0, "Identify IPC client");
    DWORD session{};
    check_win(ProcessIdToSessionId(pid, &session) != 0, "Identify IPC session");
    check_win(ImpersonateNamedPipeClient(pipe) != 0, "Identify caller token");
    HANDLE raw{};
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw))
    {
        auto error = GetLastError();
        RevertToSelf();
        throw HResultError(HRESULT_FROM_WIN32(error), "Read caller token");
    }
    Handle token(raw);
    std::wstring sid;
    try
    {
        sid = user_sid(token.get());
    }
    catch (...)
    {
        RevertToSelf();
        throw;
    }
    check_win(RevertToSelf() != 0, "Revert caller context");
    return {sid, session, pid};
}
void PipeServer::run()
{
    try
    {
        auto own = user_sid();
        std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
        sddl += service_ ? L"(A;;GRGW;;;IU)" : L"(A;;GRGW;;;" + own + L")";
        PSECURITY_DESCRIPTOR descriptor{};
        check_win(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                       &descriptor, nullptr) != 0,
                  "Create IPC permissions");
        struct Free
        {
            PSECURITY_DESCRIPTOR p;
            ~Free()
            {
                LocalFree(p);
            }
        } free{descriptor};
        SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
        // One persistent instance prevents a second process claiming the name between requests.
        Handle pipe(CreateNamedPipeW(
            name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536,
            1000, &sa));
        check_win(bool(pipe), "Create IPC server");
        SetEvent(ready_.get());
        while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
        {
            Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED ov{};
            ov.hEvent = ev.get();
            BOOL ok = ConnectNamedPipe(pipe.get(), &ov);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError(), n{};
            if (!ok && error == ERROR_IO_PENDING)
            {
                if (!finish(pipe.get(), ov, stop_.get(), INFINITE, n))
                    break;
            }
            else if (!ok && error != ERROR_PIPE_CONNECTED)
                throw HResultError(HRESULT_FROM_WIN32(error), "Accept IPC client");
            try
            {
                auto j = receive(pipe.get(), stop_.get(), 3000);
                auto who = caller(pipe.get());
                if (service_)
                {
                    if (who.session != WTSGetActiveConsoleSessionId() || who.session == 0)
                        throw std::runtime_error("Only the active local user can manage OmniSound");
                }
                else if (who.sid != own || who.session != process_session())
                    throw std::runtime_error("IPC user mismatch");
                send(pipe.get(), handler_(j, who), stop_.get(), 3000);
            }
            catch (const std::exception &e)
            {
                try
                {
                    send(pipe.get(), {{"error", e.what()}}, stop_.get(), 1000);
                }
                catch (...)
                {
                }
            }
            BYTE ack{};
            transfer(pipe.get(), &ack, 1, false, stop_.get(), 1000);
            // Bounded acknowledgement avoids discarding unread data without FlushFileBuffers on an untrusted
            // pipe.
            DisconnectNamedPipe(pipe.get());
        }
    }
    catch (const std::exception &e)
    {
        if (WaitForSingleObject(ready_.get(), 0) != WAIT_OBJECT_0)
        {
            startup_error_ = e.what();
            SetEvent(ready_.get());
        }
        log_event("ipc-failure", e.what());
    }
}
} // namespace omni
