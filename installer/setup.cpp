#include "wizard.hpp"
#include <sddl.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace omni::setup
{
namespace
{
std::wstring literal(const std::wstring &value)
{
    std::wstring result = L"'";
    for (auto c : value)
    {
        result += c;
        if (c == L'\'')
            result += c;
    }
    return result + L"'";
}
std::wstring encoded(const std::wstring &value)
{
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    auto bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    DWORD size{};
    check_win(
        CryptBinaryToStringW(reinterpret_cast<const BYTE *>(value.data()), bytes, flags, nullptr, &size) != 0,
        "Prepare setup command");
    std::wstring result(size, L'\0');
    check_win(CryptBinaryToStringW(reinterpret_cast<const BYTE *>(value.data()), bytes, flags, result.data(),
                                   &size) != 0,
              "Encode setup command");
    result.resize(wcslen(result.c_str()));
    return result;
}
std::string read_log(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
} // namespace
Result provision(HINSTANCE module, const Options &options, Progress progress)
{
    Result result;
    try
    {
        if (!options.uninstall && !options.accepted)
            throw std::runtime_error("사용권 동의 후 설치를 시작하세요.");
        auto temp = std::filesystem::temp_directory_path() / wide("OmniSound-setup-" + new_id());
        PSECURITY_DESCRIPTOR descriptor{};
        auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + user_sid() + L")";
        check_win(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                       &descriptor, nullptr) != 0,
                  "Secure installer directory");
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        auto created = CreateDirectoryW(temp.c_str(), &security);
        LocalFree(descriptor);
        check_win(created != 0, "Create installer directory");
        auto resource = FindResourceW(module, MAKEINTRESOURCEW(101), RT_RCDATA);
        if (!resource)
            throw std::runtime_error("Installer script resource missing");
        auto script = temp / L"Install.ps1";
        {
            std::ofstream file(script, std::ios::binary);
            file.write(static_cast<const char *>(LockResource(LoadResource(module, resource))),
                       SizeofResource(module, resource));
            if (!file)
                throw std::runtime_error("Cannot prepare installer");
        }
        result.log = temp / L"install.log";
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        Handle output(CreateFileW(result.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        check_win(bool(output), "Open installation log");
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        auto powershell = std::filesystem::path(system) / L"WindowsPowerShell\\v1.0\\powershell.exe";
        auto invocation =
            L"$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
            L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $env:PSModulePath=" +
            literal((powershell.parent_path() / L"Modules").wstring()) +
            L"; try { & ([ScriptBlock]::Create([IO.File]::ReadAllText(" + literal(script.wstring()) +
            L"))) -Source " + literal(executable_dir().wstring()) + L" -Mode " +
            literal(options.lite ? L"Lite" : L"Driver") + L" -InstallDirectory " +
            literal(options.target.wstring()) + (options.uninstall ? L" -Uninstall" : L"") +
            (options.local_test ? L" -LocalTest" : L"") +
            L"; if (!$?) { exit 1 } } catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }";
        auto command = L"\"" + powershell.wstring() +
                       L"\" -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand " +
                       encoded(invocation);
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        check_win(bool(input), "Open installer input");
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output.get();
        startup.hStdError = output.get();
        startup.hStdInput = input.get();
        PROCESS_INFORMATION process{};
        check_win(CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, executable_dir().c_str(), &startup, &process) != 0,
                  "Start installation");
        Handle child(process.hProcess), thread(process.hThread);
        std::string previous, previous_phase;
        for (;;)
        {
            const auto wait = WaitForSingleObject(child.get(), 200);
            if (wait == WAIT_FAILED)
                throw std::runtime_error("Cannot wait for installation process");
            auto log = read_log(result.log);
            if (log != previous)
            {
                std::istringstream lines(log);
                std::string line;
                std::string phase;
                while (std::getline(lines, line))
                    if (line.starts_with("OMNISOUND_PHASE|"))
                        phase = line;
                if (!phase.empty() && phase != previous_phase)
                {
                    auto separator = phase.find('|', 16);
                    if (separator != std::string::npos)
                        try
                        {
                            progress(std::stoi(phase.substr(16, separator - 16)),
                                     wide(phase.substr(separator + 1)));
                            previous_phase = phase;
                        }
                        catch (...)
                        {
                        }
                }
                previous = std::move(log);
            }
            if (wait == WAIT_OBJECT_0)
                break;
        }
        DWORD code{};
        check_win(GetExitCodeProcess(child.get(), &code) != 0, "Read installation result");
        result.code = static_cast<int>(code);
        output.reset();
        std::filesystem::remove(script);
        if (result.code)
        {
            auto log = read_log(result.log);
            std::istringstream lines(log);
            std::string line, errors;
            while (std::getline(lines, line))
                if (!line.starts_with("OMNISOUND_PHASE|") && !line.empty())
                    errors += line + '\n';
            if (errors.size() > 12000)
                errors = errors.substr(errors.size() - 12000);
            result.message = L"설치가 중단되었습니다. 아래 원인을 확인한 뒤 다시 시도하세요.\r\n\r\n" +
                             wide(errors) + L"\r\n로그: " + result.log.wstring();
        }
        else
            result.message = options.uninstall
                                 ? L"OmniSound를 제거했습니다.\r\n저장된 그룹과 보정 설정은 보관했습니다."
                                 : (options.lite ? L"OmniSound Lite 설치를 완료했습니다."
                                                 : L"OmniSound Pro 설치를 완료했습니다.") +
                                       std::wstring(L"\r\n\r\n설치 위치\r\n") + options.target.wstring() +
                                       L"\r\n\r\n시작 메뉴에서 앱을 실행할 수 있습니다.";
    }
    catch (const std::exception &error)
    {
        result.code = 1;
        result.message = wide(error.what());
    }
    return result;
}
} // namespace omni::setup

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR, int)
{
    using namespace omni;
    using namespace omni::setup;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Options options;
    bool silent = false;
    int count{};
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    try
    {
        for (int i = 1; args && i < count; ++i)
        {
            std::wstring value(args[i]);
            if (value == L"--lite-install")
                options.lite = true;
            else if (value == L"--lite-uninstall")
            {
                options.lite = true;
                options.uninstall = true;
            }
            else if (value == L"--driver-install")
                options.lite = false;
            else if (value == L"--driver-uninstall")
            {
                options.lite = false;
                options.uninstall = true;
            }
            else if (value == L"--local-test-install" || value == L"--local-test-uninstall")
            {
                options.lite = false;
                options.local_test = true;
                options.uninstall = value == L"--local-test-uninstall";
            }
            else if (value == L"--provision")
                options.provision = true;
            else if (value == L"--accept-license")
                options.accepted = true;
            else if (value == L"--silent")
                silent = true;
            else if (value == L"--wizard-smoke")
                options.smoke = true;
            else if (value == L"--snapshots" && i + 1 < count)
                options.snapshots = args[++i];
            else if (value == L"--install-dir" && i + 1 < count)
                options.target = args[++i];
            else
                throw std::runtime_error("Unknown setup option");
        }
        if (args)
        {
            LocalFree(args);
            args = nullptr;
        }
        if (options.target.empty())
            options.target = install_location(options.lite);
        Handle mutex(CreateMutexW(
            nullptr, FALSE,
            (L"Local\\OmniSound.Setup." + user_sid() + (IsUserAnAdmin() ? L".Admin" : L".User")).c_str()));
        const auto wait = WaitForSingleObject(mutex.get(), 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            throw std::runtime_error("다른 OmniSound 설치 창이 열려 있습니다. 해당 창을 먼저 닫아 주세요.");
        int result;
        if (silent)
        {
            if (!options.lite && !IsUserAnAdmin())
                throw std::runtime_error("Driver 설치에는 관리자 권한이 필요합니다.");
            result = provision(module, options, [](int, std::wstring) {}).code;
        }
        else
            result = show_wizard(module, std::move(options));
        ReleaseMutex(mutex.get());
        CoUninitialize();
        return result;
    }
    catch (const std::exception &error)
    {
        if (args)
            LocalFree(args);
        if (!silent && !options.smoke)
            MessageBoxW(nullptr, wide(error.what()).c_str(), L"OmniSound 설치", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
}
