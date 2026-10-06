#pragma once

#include <string>
#include <vector>

// The command line of the Python t4ff (python -m t4ff), for t4ff-cli: the same commands and options,
// parsed as argparse does (an option can be shortened to any unique prefix, --option=value, -o value).
//
//   t4ff-cli [--no-install] [--progress-lines] info <fastfile>... [--list]
//   t4ff-cli roundtrip <fastfile>...
//   t4ff-cli convert <pc fastfile or usermap folder> -o <output folder> [options]
//   t4ff-cli menu <_codxe\t4 folder> [--rows N] [--no-streams] [--menu-zone P]...
//   t4ff-cli streams <folder>...
//   t4ff-cli setup [--xma2encode P] [--no-test]
namespace t4ff::cli
{
// runs the command line (the arguments after the program's name); -1 when it is none of the Python's
// (t4ff-cli's developer commands, the convert form with --out)
int run(const std::vector<std::wstring> &args);
} // namespace t4ff::cli
