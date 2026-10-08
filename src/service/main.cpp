#include <omni/devices.hpp>
#include <omni/ipc.hpp>
#include <mutex>
#include <iostream>
#include <sddl.h>
using namespace omni;
namespace
{
SERVICE_STATUS_HANDLE status_handle{};
SERVICE_STATUS service_status{};
Handle stop_event;
std::mutex config_lock;
void report(DWORD state, DWORD error = NO_ERROR)
{
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    service_status.dwWin32ExitCode = error;
    service_status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
    if (status_handle)
        SetServiceStatus(status_handle, &service_status);
}
void WINAPI control(DWORD code)
{
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN)
    {
        report(SERVICE_STOP_PENDING);
        SetEvent(stop_event.get());
    }
}
BOOL WINAPI console_signal(DWORD)
{
    SetEvent(stop_event.get());
    return TRUE;
}
std::filesystem::path users_dir()
{
    return machine_data() / L"users";
}
void reject_reparse(const std::filesystem::path &path)
{
    for (auto p = path; !p.empty() && p != p.root_path(); p = p.parent_path())
    {
        auto attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Configuration directory must not be a reparse point");
    }
}
std::filesystem::path config_path(const std::wstring &sid)
{
    PSID parsed{};
    check_win(ConvertStringSidToSidW(sid.c_str(), &parsed) != 0, "Validate owner identity");
    LocalFree(parsed);
    auto p = users_dir() / (sid + L".json");
    reject_reparse(p);
    return p;
}
Config load(const std::wstring &sid)
{
    return read_json(config_path(sid), Json(Config{})).get<Config>();
}
void run()
{
    ComScope com;
    reject_reparse(users_dir());
    std::filesystem::create_directories(users_dir());
    VirtualDevices devices;
    Json device_status = Json::array();
    PipeServer server(service_pipe, true, [&](const Json &r, const Caller &who) -> Json {
        ComScope thread_com;
        auto op = r.at("op").get<std::string>();
        std::lock_guard lock(config_lock);
        if (op == "load")
        {
            auto config = load(who.sid);
            auto owned_devices = Json::array();
            for (const auto &device : device_status)
                if (std::any_of(config.groups.begin(), config.groups.end(),
                                [&](const Group &g) { return g.id == device.at("id").get<std::string>(); }))
                    owned_devices.push_back(device);
            return {{"config", config}, {"devices", owned_devices}, {"serviceVersion", "0.2.0"}};
        }
        if (op == "save")
        {
            auto c = r.at("config").get<Config>();
            auto previous = load(who.sid);
            if (c.revision != previous.revision)
                throw std::runtime_error("Configuration changed. Reload before saving.");
            auto outputs = endpoints(false, true);
            for (const auto &g : c.groups)
            {
                wide(g.name);
                for (const auto &m : g.members)
                {
                    auto e = std::find_if(outputs.begin(), outputs.end(),
                                          [&](const Endpoint &ep) { return ep.id == m.endpoint_id; });
                    if (e != outputs.end() && !e->group_id.empty())
                        throw std::runtime_error("Virtual outputs cannot be group members");
                    if (e == outputs.end())
                    {
                        bool existed = false;
                        for (const auto &old : previous.groups)
                            for (const auto &om : old.members)
                                existed |= om.endpoint_id == m.endpoint_id;
                        if (!existed)
                            throw std::runtime_error("Selected physical output no longer exists");
                    }
                }
            }
            // A user cannot claim another user's stable device identity.
            for (const auto &entry : std::filesystem::directory_iterator(users_dir()))
            {
                if (entry.path() == config_path(who.sid) || entry.path().extension() != L".json")
                    continue;
                reject_reparse(entry.path());
                auto other = read_json(entry.path()).get<Config>();
                for (const auto &g : c.groups)
                    for (const auto &og : other.groups)
                        if (g.id == og.id)
                            throw std::runtime_error("Group identity belongs to another user");
            }
            if (c.revision == UINT64_MAX)
                throw std::runtime_error("Configuration revision overflow");
            ++c.revision;
            atomic_json(config_path(who.sid), Json(c));
            return {{"config", c}};
        }
        throw std::runtime_error("Unknown device-management command");
    });
    report(SERVICE_RUNNING);
    while (WaitForSingleObject(stop_event.get(), 1000) != WAIT_OBJECT_0)
    {
        std::vector<Group> all;
        bool complete = true;
        {
            std::lock_guard lock(config_lock);
            for (const auto &entry : std::filesystem::directory_iterator(users_dir()))
            {
                if (entry.path().extension() != L".json")
                    continue;
                try
                {
                    reject_reparse(entry.path());
                    auto c = read_json(entry.path()).get<Config>();
                    all.insert(all.end(), c.groups.begin(), c.groups.end());
                }
                catch (const std::exception &e)
                {
                    complete = false;
                    log_event("config-read", e.what());
                }
            }
        }
        // A corrupt or temporarily unreadable file must not remove existing outputs.
        if (complete)
            devices.reconcile(all);
        {
            std::lock_guard lock(config_lock);
            device_status = devices.status();
        }
    }
}
void WINAPI service_main(DWORD, LPWSTR *)
{
    status_handle = RegisterServiceCtrlHandlerW(L"OmniSound", control);
    if (!status_handle)
        return;
    report(SERVICE_START_PENDING);
    try
    {
        run();
        report(SERVICE_STOPPED);
    }
    catch (const std::exception &e)
    {
        log_event("service-failure", e.what());
        report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (argc > 1 && std::wstring(argv[1]) == L"--console")
    {
        SetConsoleCtrlHandler(console_signal, TRUE);
        try
        {
            run();
            return 0;
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(L"OmniSound"), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table))
        return static_cast<int>(GetLastError());
    return 0;
}
