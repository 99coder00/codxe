#include "core/progress.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace t4ff::progress
{
namespace
{
std::mutex lock;
bool lines = false;
Sink sink;
std::string current;
double last_time = 0;
int quarter = 0;

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

void use_lines(bool enabled)
{
    std::lock_guard<std::mutex> guard(lock);
    lines = enabled;
    current.clear();
    last_time = 0;
    quarter = 0;
}

void set_sink(Sink s)
{
    std::lock_guard<std::mutex> guard(lock);
    sink = std::move(s);
}

void step(const std::string &label, int done, int total)
{
    std::lock_guard<std::mutex> guard(lock);
    double t = now();
    bool fresh = label != current;
    if (fresh)
    {
        current = label;
        quarter = 0;
    }
    if (sink || lines)
    {
        // at most 5 updates a second, besides the first and last of a step
        if (!fresh && 0 < done && done < total && t - last_time < 0.2)
            return;
        last_time = t;
        if (sink)
            sink(done, total, label);
        else
        {
            printf("@progress %d %d %s\n", done, total, label.c_str());
            fflush(stdout);
        }
        return;
    }
    if (total > 0)
    {
        int q = static_cast<int>(static_cast<long long>(done) * 4 / total);
        if (0 < q && q < 4 && q > quarter)
        {
            quarter = q;
            printf("  %s: %lld%% (%d/%d)\n", label.c_str(), static_cast<long long>(done) * 100 / total, done, total);
            fflush(stdout);
        }
    }
}
} // namespace t4ff::progress
