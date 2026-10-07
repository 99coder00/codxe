#include "gui/shell.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace t4ff::gui
{
namespace
{
template <typename T> struct Com
{
    T *p = nullptr;
    ~Com()
    {
        if (p)
            p->Release();
    }
    T **operator&()
    {
        return &p;
    }
    T *operator->() const
    {
        return p;
    }
};

std::vector<fs::path> run_dialog(void *owner, const wchar_t *title, const std::vector<FileType> &types, bool folders, bool multiple, const fs::path &start)
{
    std::vector<fs::path> out;
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return out;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST) | (multiple ? FOS_ALLOWMULTISELECT : 0);
    dialog->SetOptions(options);
    dialog->SetTitle(title);
    if (!types.empty())
    {
        std::vector<COMDLG_FILTERSPEC> specs;
        for (const FileType &t : types)
            specs.push_back({t.name, t.pattern});
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }
    std::error_code ec;
    fs::path folder = start;
    if (!folder.empty() && !fs::is_directory(folder, ec))
        folder = folder.parent_path();
    if (!folder.empty() && fs::is_directory(folder, ec))
    {
        Com<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(fs::absolute(folder).c_str(), nullptr, IID_PPV_ARGS(&item))))
            dialog->SetFolder(item.p);
    }
    if (FAILED(dialog->Show(static_cast<HWND>(owner))))
        return out; // cancelled
    Com<IShellItemArray> items;
    if (FAILED(dialog->GetResults(&items)))
        return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i)
    {
        Com<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item)))
            continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
        {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}
} // namespace

std::vector<fs::path> pick_files(void *owner, const wchar_t *title, const std::vector<FileType> &types, bool multiple, const fs::path &start)
{
    return run_dialog(owner, title, types, false, multiple, start);
}

std::vector<fs::path> pick_folders(void *owner, const wchar_t *title, bool multiple, const fs::path &start)
{
    return run_dialog(owner, title, {}, true, multiple, start);
}

void open_folder(const fs::path &folder)
{
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool system_uses_dark_theme()
{
    DWORD light = 1, size = sizeof light;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                     nullptr, &light, &size) != ERROR_SUCCESS)
        return true;
    return light == 0;
}

void set_dark_title_bar(void *window, bool dark)
{
    BOOL value = dark;
    DwmSetWindowAttribute(static_cast<HWND>(window), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof value);
}
} // namespace t4ff::gui
