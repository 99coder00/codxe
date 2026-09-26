#pragma once

#include "pch.h"

namespace t4
{
namespace sp
{
// log_console: what the Server and main threads are doing when they stop making progress (a runaway
// loop or a freeze), and the game's longjmps (how script and game errors unwind), written to the debug
// output (xenia.log).
class thread_watch : public Module
{
  public:
    thread_watch();
    ~thread_watch();

    // The calling thread got something done (printed, ran a client frame): if it is the main thread,
    // it is not stuck.
    static void OnProgress();
};
} // namespace sp
} // namespace t4
