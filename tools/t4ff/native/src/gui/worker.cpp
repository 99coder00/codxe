#include "gui/worker.h"

#include <cstdio>
#include <mutex>

#include <windows.h>

#include "cli/t4ff_cli.h"
#include "core/process.h"

namespace t4ff::gui
{
namespace
{
std::wstring own_path()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size())
        {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::string last_error_text()
{
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf, sizeof buf, nullptr);
    std::string text = buf;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    return text;
}

// the job (a Windows job object) is shared between stop() and the thread that ends it
std::mutex job_lock;
} // namespace

Worker::~Worker()
{
    stop();
    wait();
}

void Worker::wait()
{
    if (reader_.joinable())
        reader_.join();
}

bool Worker::start(const std::vector<std::wstring> &args, LineFn on_line, ExitFn on_exit, std::string &error)
{
    wait(); // the previous one's thread
    std::wstring cmdline = quote_argument(own_path()) + L" --worker";
    for (const std::wstring &a : args)
        cmdline += L" " + quote_argument(a);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0))
    {
        error = "cannot create a pipe: " + last_error_text();
        return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // every process it starts belongs to the job: ending the job ends them all, and so does the
    // window closing (the job's last handle)
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job)
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = out_w;
    // it inherits these handles only
    HANDLE inherit[2] = {nul, out_w};
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
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | (have_attrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
                             &si.StartupInfo, &pi);
    std::string start_error = ok ? std::string() : last_error_text();
    if (have_attrs)
        DeleteProcThreadAttributeList(attrs);
    CloseHandle(out_w);
    CloseHandle(nul);
    if (!ok)
    {
        CloseHandle(out_r);
        if (job)
            CloseHandle(job);
        error = "cannot start the converter: " + start_error;
        return false;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> guard(job_lock);
        job_ = job;
        process_ = pi.hProcess;
    }
    running_ = true;

    reader_ = std::thread([this, out_r, process = pi.hProcess, job, on_line = std::move(on_line), on_exit = std::move(on_exit)] {
        std::string pending;
        char buf[16384];
        DWORD got = 0;
        while (ReadFile(out_r, buf, sizeof buf, &got, nullptr) && got)
        {
            pending.append(buf, got);
            size_t start = 0;
            for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1)
            {
                std::string text = pending.substr(start, nl - start);
                if (!text.empty() && text.back() == '\r')
                    text.pop_back();
                on_line(text);
            }
            pending.erase(0, start);
        }
        if (!pending.empty())
            on_line(pending);
        CloseHandle(out_r);
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        {
            std::lock_guard<std::mutex> guard(job_lock);
            process_ = job_ = nullptr;
        }
        CloseHandle(process);
        if (job)
            CloseHandle(job); // the programs it left running end with it
        running_ = false;
        on_exit(static_cast<int>(code));
    });
    return true;
}

void Worker::stop()
{
    std::lock_guard<std::mutex> guard(job_lock);
    if (job_)
        TerminateJobObject(static_cast<HANDLE>(job_), STOPPED_EXIT_CODE);
    else if (process_)
        TerminateProcess(static_cast<HANDLE>(process_), STOPPED_EXIT_CODE);
}

int worker_main(const std::vector<std::wstring> &args)
{
    // a crash ends it quietly (the window reports it), without Windows' error dialog
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // it has no window: Windows would run it in efficiency mode
    opt_out_of_power_throttling();
    try
    {
        int code = cli::run(args);
        if (code < 0)
        {
            fprintf(stderr, "error: not one of t4ff-cli's commands\n");
            return 2;
        }
        fflush(stdout);
        return code;
    }
    catch (const std::exception &e)
    {
        fflush(stdout);
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
} // namespace t4ff::gui
