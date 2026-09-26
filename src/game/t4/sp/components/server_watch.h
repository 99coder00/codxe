#pragma once

#include "pch.h"

namespace t4
{
namespace sp
{
// log_console: what the Server thread is doing when it stops finishing frames, and the game's
// longjmps (how script and game errors unwind), written to the debug output (xenia.log).
class server_watch : public Module
{
  public:
    server_watch();
    ~server_watch();
};
} // namespace sp
} // namespace t4
