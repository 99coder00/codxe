#pragma once

#include "pch.h"

namespace t4
{
namespace sp
{
// The Nazi Zombies map list (the levels_unlock menu) shows the maps of the usermaps folder, scrolling.
// The menu (made by tools/t4ff "menu") has rows whose text and command are dvars this module fills;
// see OnMenuOpen and OnUIRefresh.
class UsermapList : public Module
{
  public:
    UsermapList();
    ~UsermapList();

    static void OnMenuOpen(const char *menuName);
    static void OnUIRefresh();
};
} // namespace sp
} // namespace t4
