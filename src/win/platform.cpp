#include <omni/platform.hpp>
#include <sddl.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
namespace omni
{
static std::string describe(HRESULT h)
{
    char b[32];
    sprintf_s(b, "0x%08lX", static_cast<unsigned long>(h));
    return b;
}
HResultError::HResultError(HRESULT h, const char *operation)
    : std::runtime_error(std::string(operation) + ": " + describe(h)), code(h)
{
}
void check(HRESULT h, const char *op)
{
    if (FAILED(h))
        throw HResultError(h, op);
}
void check_win(bool ok, const char *op)
{
    if (!ok)
        check(HRESULT_FROM_WIN32(GetLastError()), op);
}
std::wstring wide(std::string_view s)
{
    if (s.empty())
        return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    check_win(n > 0, "Invalid UTF-8");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string utf8(std::wstring_view s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr,
                                0, nullptr, nullptr);
    check_win(n > 0, "Invalid UTF-16");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
double qpc_seconds()
{
    static const double freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / freq;
}
std::string utc_now()
{
    SYSTEMTIME t;
    GetSystemTime(&t);
    char b[40];
    sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
std::string new_id()
{
    GUID id;
    check(CoCreateGuid(&id), "Create group ID");
    wchar_t b[40];
    check_win(StringFromGUID2(id, b, 40) != 0, "Format group ID");
    auto result = utf8(std::wstring_view(b + 1, 36));
    // StringFromGUID2 uses uppercase hex; persistent group IDs are canonical
    // lowercase so validation, identity comparisons and duplicate checks agree.
    for (auto &character : result)
        if (character >= 'A' && character <= 'F')
            character += 'a' - 'A';
    return result;
}
std::wstring user_sid(HANDLE token)
{
    Handle owned;
    if (!token)
    {
        HANDLE t;
        check_win(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) != 0, "Read process token");
        owned.reset(t);
        token = t;
    }
    DWORD n = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> b(n);
    check_win(GetTokenInformation(token, TokenUser, b.data(), n, &n) != 0, "Read user identity");
    LPWSTR text{};
    check_win(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &text) != 0,
              "Format identity");
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}
DWORD process_session()
{
    DWORD id;
    check_win(ProcessIdToSessionId(GetCurrentProcessId(), &id) != 0, "Read session");
    return id;
}
std::filesystem::path executable_dir()
{
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    check_win(n > 0 && n < p.size(), "Find executable");
    p.resize(n);
    return std::filesystem::path(p).parent_path();
}
static std::filesystem::path known(REFKNOWNFOLDERID id)
{
    PWSTR p{};
    check(SHGetKnownFolderPath(id, 0, nullptr, &p), "Find data directory");
    std::filesystem::path out(p);
    CoTaskMemFree(p);
    return out / L"OmniSound";
}
std::filesystem::path local_data()
{
    return known(FOLDERID_LocalAppData);
}
std::filesystem::path machine_data()
{
    return known(FOLDERID_ProgramData);
}
bool is_lite_install()
{
    std::ifstream mode(executable_dir() / L"install.mode", std::ios::binary);
    std::string value;
    std::getline(mode, value);
    return value == "Lite";
}
bool login_start_enabled(bool lite)
{
    DWORD value = 1, bytes = sizeof(value);
    const auto key = lite ? L"Software\\OmniSound\\Desktop\\Lite" : L"Software\\OmniSound\\Desktop\\Driver";
    RegGetValueW(HKEY_CURRENT_USER, key, L"StartWithWindows", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return value != 0;
}
void set_login_start(bool lite, bool enabled)
{
    const auto value_name = lite ? L"OmniSound Lite Engine" : L"OmniSound Engine";
    HKEY run{};
    check(HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_CURRENT_USER,
                                             L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
                                             0, KEY_SET_VALUE, nullptr, &run, nullptr)),
          "Open login settings");
    const auto image = executable_dir() / L"OmniSound.exe";
    const auto command = L"\"" + image.wstring() + L"\" --background";
    auto result =
        enabled ? RegSetValueExW(run, value_name, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                                 static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                : RegDeleteValueW(run, value_name);
    RegCloseKey(run);
    if (result != ERROR_FILE_NOT_FOUND)
        check(HRESULT_FROM_WIN32(result), "Update login launch");
    const auto key = lite ? L"Software\\OmniSound\\Desktop\\Lite" : L"Software\\OmniSound\\Desktop\\Driver";
    const DWORD value = enabled ? 1 : 0;
    check(HRESULT_FROM_WIN32(
              RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &run, nullptr)),
          "Open login preference");
    result = RegSetValueExW(run, L"StartWithWindows", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
                            sizeof(value));
    RegCloseKey(run);
    check(HRESULT_FROM_WIN32(result), "Save login preference");
}
std::wstring login_launch_command(bool lite)
{
    const auto key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const auto name = lite ? L"OmniSound Lite Engine" : L"OmniSound Engine";
    DWORD bytes{};
    auto result = RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND)
        return {};
    check(HRESULT_FROM_WIN32(result), "Read login launch size");
    if (bytes > 65536)
        throw std::runtime_error("Login launch command is too large");
    std::wstring command(bytes / sizeof(wchar_t), L'\0');
    result = RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, command.data(), &bytes);
    check(HRESULT_FROM_WIN32(result), "Read login launch");
    command.resize(wcsnlen_s(command.c_str(), command.size()));
    return command;
}
static std::filesystem::path lite_config_path()
{
    return local_data() / L"lite-config.json";
}
static std::wstring lite_config_mutex()
{
    return L"Local\\OmniSound.LiteConfig." + user_sid();
}
Config load_lite_config()
{
    return read_json(lite_config_path(), Json(Config{})).get<Config>();
}
Config save_lite_config(Config next, uint64_t expected_revision)
{
    Handle mutex(CreateMutexW(nullptr, FALSE, lite_config_mutex().c_str()));
    check_win(bool(mutex), "Lock Lite configuration");
    const auto waited = WaitForSingleObject(mutex.get(), 5000);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
        throw std::runtime_error("Timed out waiting for Lite configuration");
    struct Unlock
    {
        HANDLE handle;
        ~Unlock()
        {
            ReleaseMutex(handle);
        }
    } unlock{mutex.get()};
    auto current = load_lite_config();
    if (current.revision != expected_revision)
        throw std::runtime_error("Lite settings changed. Reload before saving.");
    validate(next);
    if (current.revision == UINT64_MAX)
        throw std::runtime_error("Lite configuration revision overflow");
    next.revision = current.revision + 1;
    atomic_json(lite_config_path(), Json(next));
    return next;
}
Json read_json(const std::filesystem::path &p, Json fallback)
{
    if (!std::filesystem::exists(p))
        return fallback;
    auto n = std::filesystem::file_size(p);
    if (n > max_message_bytes)
        throw std::runtime_error("Configuration is too large");
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read configuration");
    return Json::parse(f);
}
void atomic_json(const std::filesystem::path &p, const Json &j)
{
    std::filesystem::create_directories(p.parent_path());
    auto tmp = p;
    tmp += L".tmp";
    auto bytes = j.dump(2);
    Handle file(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    check_win(bool(file), "Write configuration");
    DWORD written{};
    check_win(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size(),
              "Write configuration");
    check_win(FlushFileBuffers(file.get()) != 0, "Flush configuration");
    file.reset();
    check_win(MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
              "Commit configuration");
}
void start_sibling(const wchar_t *name, const wchar_t *args)
{
    auto p = executable_dir() / name;
    std::wstring cmd = L"\"" + p.wstring() + L"\" " + args;
    STARTUPINFOW s{sizeof(s)};
    s.dwFlags = STARTF_USESHOWWINDOW;
    s.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    check_win(CreateProcessW(p.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                             p.parent_path().c_str(), &s, &pi) != 0,
              "Start background engine");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}
void log_event(const std::string &event, const std::string &detail)
{
    try
    {
        static std::mutex lock;
        std::lock_guard guard(lock);
        auto p = local_data() / L"diagnostics.jsonl";
        std::filesystem::create_directories(p.parent_path());
        if (std::filesystem::exists(p) && std::filesystem::file_size(p) > 2 * 1024 * 1024)
        {
            auto old = p;
            old += L".old";
            MoveFileExW(p.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        std::ofstream f(p, std::ios::app | std::ios::binary);
        f << Json({{"time", utc_now()}, {"event", event}, {"detail", detail.substr(0, 2048)}}).dump() << '\n';
    }
    catch (...)
    {
    }
}
} // namespace omni
