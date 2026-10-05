#include "core/threads.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace t4ff
{
namespace
{
constexpr SIZE_T THREAD_STACK = 64u << 20;

struct Work
{
    size_t count;
    const std::function<void(size_t)> *fn;
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex error_lock;
};

DWORD WINAPI worker(LPVOID param)
{
    Work &w = *static_cast<Work *>(param);
    for (size_t i; (i = w.next++) < w.count;)
    {
        try
        {
            (*w.fn)(i);
        }
        catch (...)
        {
            std::lock_guard lock(w.error_lock);
            if (!w.error)
                w.error = std::current_exception();
            w.next = w.count; // stop handing out work
        }
    }
    return 0;
}
} // namespace

int job_count(int jobs)
{
    return jobs > 0 ? jobs : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
}

void parallel_for(size_t count, int jobs, const std::function<void(size_t)> &fn)
{
    if (count == 0)
        return;
    Work w;
    w.count = count;
    w.fn = &fn;
    size_t threads = std::min<size_t>(static_cast<size_t>(job_count(jobs)), count);
    std::vector<HANDLE> handles;
    for (size_t t = 0; t < threads; ++t)
    {
        HANDLE h = CreateThread(nullptr, THREAD_STACK, worker, &w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (h)
            handles.push_back(h);
    }
    if (handles.empty())
        worker(&w); // no thread could be made: do the work here
    for (HANDLE h : handles)
    {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    if (w.error)
        std::rethrow_exception(w.error);
}
} // namespace t4ff
