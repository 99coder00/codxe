#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// The window runs each command (a conversion, setup, the menu) as t4ff.exe --worker <t4ff-cli's
// command line>: a process of its own, so the window stays responsive, a conversion's memory goes with
// it, and Stop ends it (and the encoders it runs) at once, as the Python window ends its python -m t4ff.
namespace t4ff::gui
{
class Worker
{
  public:
    using LineFn = std::function<void(const std::string &line)>; // a line of its output (without the end of line)
    using ExitFn = std::function<void(int code)>;

    Worker() = default;
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    // starts this program with --worker and args; on_line and on_exit are called on a thread of the
    // worker's. False (and error) when it cannot start.
    bool start(const std::vector<std::wstring> &args, LineFn on_line, ExitFn on_exit, std::string &error);
    // ends it and every program it started
    void stop();
    // waits for the thread of the last one (its on_exit has run)
    void wait();
    bool running() const
    {
        return running_;
    }

  private:
    void *job_ = nullptr;
    void *process_ = nullptr;
    std::thread reader_;
    std::atomic<bool> running_{false};
};

// the process exit code of a worker ended by stop()
constexpr int STOPPED_EXIT_CODE = 0x7473;

// the worker's side: runs t4ff-cli's command line (args after --worker) with its output on stdout
int worker_main(const std::vector<std::wstring> &args);
} // namespace t4ff::gui
