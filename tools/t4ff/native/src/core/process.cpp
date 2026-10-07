#include "core/process.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>

#include <windows.h>

namespace t4ff
{
namespace
{
std::wstring quote(const std::wstring &arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

void read_all(HANDLE h, std::vector<uint8_t> &out)
{
    uint8_t buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got)
        out.insert(out.end(), buf, buf + got);
}
} // namespace

ProcessResult run_process(const std::vector<std::wstring> &args, double timeout, const std::vector<uint8_t> *input)
{
    std::wstring cmdline;
    for (const auto &a : args)
        cmdline += (cmdline.empty() ? L"" : L" ") + quote(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r, out_w, err_r, err_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0))
        throw std::runtime_error("cannot create pipes");
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    // its input: a pipe the input is written to, else NUL
    HANDLE nul, in_w = nullptr;
    if (input)
    {
        if (!CreatePipe(&nul, &in_w, &sa, 0))
            throw std::runtime_error("cannot create pipes");
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    }
    else
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    // the child inherits these three handles only: not the files and pipes other threads have open
    // (an encoder holding another one's input open makes that one fail)
    HANDLE inherit[3] = {nul, out_w, err_w};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool have_attrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    si.lpAttributeList = have_attrs ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmdline.begin(), cmdline.end());
    line.push_back(0);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (have_attrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                             nullptr, &si.StartupInfo, &pi);
    if (have_attrs)
        DeleteProcThreadAttributeList(attrs);
    CloseHandle(out_w);
    CloseHandle(err_w);
    CloseHandle(nul);
    if (!ok)
    {
        CloseHandle(out_r);
        CloseHandle(err_r);
        if (in_w)
            CloseHandle(in_w);
        throw std::runtime_error("cannot run " + std::filesystem::path(args.at(0)).string());
    }
    ProcessResult r;
    std::vector<uint8_t> err;
    std::thread out_reader([&] { read_all(out_r, r.out); });
    std::thread err_reader([&] { read_all(err_r, err); });
    std::thread writer;
    if (in_w)
        writer = std::thread([&] {
            size_t at = 0;
            while (at < input->size())
            {
                DWORD wrote = 0;
                DWORD n = static_cast<DWORD>(std::min<size_t>(input->size() - at, 1 << 20));
                if (!WriteFile(in_w, input->data() + at, n, &wrote, nullptr) || !wrote)
                    break; // the program stopped reading
                at += wrote;
            }
            CloseHandle(in_w);
        });
    DWORD wait = WaitForSingleObject(pi.hProcess, timeout > 0 ? static_cast<DWORD>(timeout * 1000) : INFINITE);
    if (wait == WAIT_TIMEOUT)
    {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    if (writer.joinable())
        writer.join();
    out_reader.join();
    err_reader.join();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    r.err.assign(err.begin(), err.end());
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(out_r);
    CloseHandle(err_r);
    return r;
}

TempDir::TempDir()
{
    static std::atomic<uint64_t> counter{0};
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        path_ = base / (L"t4ff-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter++));
        if (std::filesystem::create_directory(path_))
            return;
    }
    throw std::runtime_error("cannot make a temporary folder");
}

TempDir::~TempDir()
{
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

std::filesystem::path find_on_path(const std::wstring &name)
{
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, name.c_str(), L".exe", MAX_PATH, buf, nullptr);
    if (n && n < MAX_PATH)
        return buf;
    return {};
}

bool launch(const std::vector<std::wstring> &args)
{
    std::wstring cmdline;
    for (const auto &a : args)
        cmdline += (cmdline.empty() ? L"" : L" ") + quote(a);
    std::vector<wchar_t> line(cmdline.begin(), cmdline.end());
    line.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

std::wstring quote_argument(const std::wstring &arg)
{
    return quote(arg);
}

void opt_out_of_power_throttling()
{
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);
}
} // namespace t4ff
