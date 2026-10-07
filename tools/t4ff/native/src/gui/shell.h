#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Windows' own dialogs and folders for the window.
namespace t4ff::gui
{
namespace fs = std::filesystem;

struct FileType
{
    const wchar_t *name, *pattern; // L"Fastfiles", L"*.ff"
};

// Windows' open dialog (nothing when cancelled); owner is the window (HWND)
std::vector<fs::path> pick_files(void *owner, const wchar_t *title, const std::vector<FileType> &types, bool multiple, const fs::path &start = {});
std::vector<fs::path> pick_folders(void *owner, const wchar_t *title, bool multiple, const fs::path &start = {});

void open_folder(const fs::path &folder);  // in Explorer
bool system_uses_dark_theme();             // Windows' setting for apps
void set_dark_title_bar(void *window, bool dark);
} // namespace t4ff::gui
