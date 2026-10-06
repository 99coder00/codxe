#pragma once

#include <functional>
#include <string>

// Progress of the long steps of a conversion (the Python t4ff's progress.py).
//
// With --progress-lines the converter prints "@progress <done> <total> <step>" lines (total 0: a step
// without a count), at most five a second within a step, for a window running it as a separate
// process. In a terminal a counted step prints how far it got every 25%. A window running the
// conversion itself gets the steps through its sink instead.
namespace t4ff::progress
{
using Sink = std::function<void(int done, int total, const std::string &step)>;

void use_lines(bool enabled = true);
// the steps go to sink (nullptr: printed again)
void set_sink(Sink sink);
// done of the total items of step label are finished (total 0: no count); thread safe
void step(const std::string &label, int done = 0, int total = 0);
} // namespace t4ff::progress
