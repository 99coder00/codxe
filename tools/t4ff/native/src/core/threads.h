#pragma once

#include <cstddef>
#include <functional>

namespace t4ff
{
// The number of threads to use for jobs (0: one per processor).
int job_count(int jobs);

// Runs fn(i) for every i in [0, count) on up to jobs threads (0: one per processor), each with a
// stack as large as the main thread's: zones are walked recursively. The first exception thrown is
// rethrown once every thread has finished.
void parallel_for(size_t count, int jobs, const std::function<void(size_t)> &fn);
} // namespace t4ff
