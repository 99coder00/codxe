// t4ff.exe: the converter window (Dear ImGui on DirectX 11). Started with --worker, it is the process the
// window runs a command in (worker.h) instead.
//
//   t4ff.exe [usermap folders or fastfiles...]
//
// Its developer options, for checking the window without a hand on the mouse: --dev-settings <json>
// (instead of %APPDATA%\t4ff\window.json), --dev-simple / --dev-advanced, --dev-theme dark|light,
// --dev-size <w> <h>, --dev-scale <s>, --dev-select <n> (the n-th map of the list), --dev-options-scroll
// <share> (the advanced options scrolled down that far, 0 to 1), --dev-run (convert
// the list) and --dev-screenshot <png> (the window, hidden, drawn into a picture once everything is
// settled, or --dev-screenshot-after <seconds>; then it quits); --dev-import <gui.json> (the Python
// window's settings, when the --dev-settings file does not exist), --dev-print-args (the command line
// of each map given, on stdout).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <zlib.h>

#include "core/process.h"
#include "gui/app.h"
#include "gui/settings.h"
#include "gui/shell.h"
#include "gui/theme.h"
#include "gui/worker.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
namespace fs = std::filesystem;
using namespace t4ff::gui;

constexpr UINT WM_WAKE = WM_APP + 1;

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_target = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
App *g_app = nullptr;
bool g_quit = false;
float g_min_scale = 1.0f;

void create_target()
{
    ID3D11Texture2D *back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back)
    {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

void release_target()
{
    if (g_target)
    {
        g_target->Release();
        g_target = nullptr;
    }
}

bool create_device(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level,
                                               &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) // no graphics driver: the software one
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level,
                                           &g_context);
    if (FAILED(hr))
        return false;
    create_target();
    return true;
}

void release_device()
{
    release_target();
    if (g_swap)
        g_swap->Release();
    if (g_context)
        g_context->Release();
    if (g_device)
        g_device->Release();
    g_swap = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

// the back buffer as a PNG (developer screenshots)
bool save_png(const fs::path &path)
{
    ID3D11Texture2D *back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back)
        return false;
    D3D11_TEXTURE2D_DESC desc;
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    g_device->CreateTexture2D(&desc, nullptr, &staging);
    if (!staging)
    {
        back->Release();
        return false;
    }
    g_context->CopyResource(staging, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        staging->Release();
        return false;
    }
    UINT w = desc.Width, h = desc.Height;
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(w) * 4 + 1) * h);
    for (UINT y = 0; y < h; ++y)
    {
        raw.push_back(0); // no filter
        const uint8_t *row = static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < w; ++x)
        {
            raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
            raw.push_back(255);
        }
    }
    g_context->Unmap(staging, 0);
    staging->Release();
    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> packed(packed_size);
    compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 6);
    packed.resize(packed_size);
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto be32 = [&](uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            png.push_back(static_cast<uint8_t>(v >> s));
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(static_cast<uint32_t>(data.size()));
        size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        be32(static_cast<uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(png.size() - start))));
    };
    std::vector<uint8_t> ihdr;
    for (uint32_t v : {w, h})
        for (int s = 24; s >= 0; s -= 8)
            ihdr.push_back(static_cast<uint8_t>(v >> s));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8 bits RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", packed);
    chunk("IEND", {});
    FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f)
        return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

LRESULT WINAPI window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
        return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            g_resize_w = LOWORD(lparam);
            g_resize_h = HIWORD(lparam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) // Alt alone: ImGui's menus, not the window's
            return 0;
        break;
    case WM_DPICHANGED: {
        const RECT *r = reinterpret_cast<const RECT *>(lparam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = reinterpret_cast<MINMAXINFO *>(lparam);
        info->ptMinTrackSize.x = static_cast<LONG>(820 * g_min_scale);
        info->ptMinTrackSize.y = static_cast<LONG>(600 * g_min_scale);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<fs::path> paths;
        for (UINT i = 0; i < count; ++i)
        {
            UINT n = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring name(n + 1, L'\0');
            DragQueryFileW(drop, i, name.data(), n + 1);
            name.resize(n);
            paths.emplace_back(name);
        }
        DragFinish(drop);
        if (g_app)
            g_app->add_paths(paths);
        return 0;
    }
    case WM_CLOSE:
        if (g_app)
        {
            g_app->request_close();
            return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

struct Options
{
    std::vector<fs::path> paths;
    fs::path settings, screenshot, import;
    std::string mode, theme;
    int width = 0, height = 0, select = -1;
    float scale = 0, shot_after = 0, options_scroll = -1;
    bool run = false, print_args = false;
};

Options parse_options(const std::vector<std::wstring> &args)
{
    Options o;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring &a = args[i];
        bool more = i + 1 < args.size();
        if (a == L"--dev-settings" && more)
            o.settings = args[++i];
        else if (a == L"--dev-screenshot" && more)
            o.screenshot = args[++i];
        else if (a == L"--dev-simple")
            o.mode = "simple";
        else if (a == L"--dev-advanced")
            o.mode = "advanced";
        else if (a == L"--dev-theme" && more)
            o.theme = utf8_of(fs::path(args[++i]));
        else if (a == L"--dev-size" && i + 2 < args.size())
        {
            o.width = _wtoi(args[++i].c_str());
            o.height = _wtoi(args[++i].c_str());
        }
        else if (a == L"--dev-scale" && more)
            o.scale = static_cast<float>(_wtof(args[++i].c_str()));
        else if (a == L"--dev-select" && more)
            o.select = _wtoi(args[++i].c_str());
        else if (a == L"--dev-screenshot-after" && more)
            o.shot_after = static_cast<float>(_wtof(args[++i].c_str()));
        else if (a == L"--dev-options-scroll" && more)
            o.options_scroll = static_cast<float>(_wtof(args[++i].c_str()));
        else if (a == L"--dev-import" && more)
            o.import = args[++i];
        else if (a == L"--dev-print-args")
            o.print_args = true;
        else if (a == L"--dev-run")
            o.run = true;
        else
            o.paths.emplace_back(a);
    }
    return o;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (!args.empty() && args[0] == L"--worker")
        return worker_main(std::vector<std::wstring>(args.begin() + 1, args.end()));

    Options opt = parse_options(args);
    bool hidden = !opt.screenshot.empty();
    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    fs::path settings_file = opt.settings.empty() ? settings_path() : opt.settings;
    Settings settings = load_settings(settings_file, opt.settings.empty() ? python_settings_path() : opt.import);
    if (opt.print_args)
    {
        // the command line of each map given, an argument a line (checked against the Python window's)
        for (const fs::path &p : opt.paths)
        {
            MapEntry m;
            m.input = utf8_of(p);
            for (const std::string &a : convert_args(settings, m))
                printf("%s\n", a.c_str());
            printf("--\n");
        }
        fflush(stdout);
        return 0;
    }
    if (!opt.mode.empty())
        settings.advanced = opt.mode == "advanced";
    if (!opt.theme.empty())
        settings.theme = opt.theme;

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, window_proc, 0, 0, instance, nullptr, LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), nullptr, nullptr, L"t4ff", nullptr};
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    POINT origin{0, 0};
    float screen_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
    g_min_scale = screen_scale;
    int w = opt.width ? opt.width : settings.window_w ? settings.window_w : static_cast<int>(1240 * screen_scale);
    int h = opt.height ? opt.height : settings.window_h ? settings.window_h : static_cast<int>(860 * screen_scale);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"t4ff \x2014 World at War usermaps for the Xbox 360", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd || !create_device(hwnd))
    {
        MessageBoxW(nullptr, L"t4ff cannot start: Direct3D 11 is not available.", L"t4ff", MB_ICONERROR);
        return 1;
    }
    if (!hidden)
    {
        ShowWindow(hwnd, settings.maximized && !opt.width ? SW_SHOWMAXIMIZED : show);
        UpdateWindow(hwnd);
    }
    else
    {
        // drawn at the size asked: a hidden window keeps the size it was made with
        RECT r{0, 0, w, h};
        GetClientRect(hwnd, &r);
        g_swap->ResizeBuffers(0, static_cast<UINT>(r.right), static_cast<UINT>(r.bottom), DXGI_FORMAT_UNKNOWN, 0);
        release_target();
        create_target();
    }
    DragAcceptFiles(hwnd, TRUE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // the window keeps its own settings
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float scale = opt.scale > 0 ? opt.scale : ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    load_fonts(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    {
        App app(
            hwnd, settings, settings_file, [hwnd] { PostMessageW(hwnd, WM_WAKE, 0, 0); },
            [hwnd] {
                g_quit = true;
                PostMessageW(hwnd, WM_WAKE, 0, 0);
            });
        g_app = &app;
        bool dark = app.want_dark();
        apply_theme(dark, scale);
        set_dark_title_bar(hwnd, dark);
        app.add_paths(opt.paths);
        if (opt.select >= 0)
            app.select_job(opt.select);
        if (opt.options_scroll >= 0)
            app.scroll_options(opt.options_scroll);
        if (opt.run)
            app.start_queue();

        int frames_left = 3, settled_frames = 0;
        while (!g_quit)
        {
            // waits for something to do: input, a worker's output, or the next frame of what moves
            DWORD timeout = INFINITE;
            if (frames_left > 0 || hidden)
                timeout = 0;
            else if (app.animating())
                timeout = 33;
            else if (ImGui::IsAnyItemHovered() || io.WantTextInput)
                timeout = 100; // tooltips appear, the text cursor blinks
            if (timeout)
                MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                    g_quit = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                frames_left = 3;
            }
            if (g_quit)
                break;
            app.update();
            if (IsIconic(hwnd) && !hidden)
            {
                frames_left = 0;
                continue;
            }
            if (g_resize_w && g_resize_h)
            {
                release_target();
                g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
                g_resize_w = g_resize_h = 0;
                create_target();
            }
            // a monitor of another DPI, or another theme
            float now_scale = opt.scale > 0 ? opt.scale : ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
            bool now_dark = app.want_dark();
            bool rescale = now_scale != scale;
            if (rescale)
            {
                scale = g_min_scale = now_scale;
                load_fonts(scale);
                ImGui_ImplDX11_InvalidateDeviceObjects();
            }
            if (rescale || now_dark != dark)
            {
                dark = now_dark;
                apply_theme(dark, scale);
                set_dark_title_bar(hwnd, dark);
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            const float clear[4] = {bg.x, bg.y, bg.z, 1.0f};
            g_context->OMSetRenderTargets(1, &g_target, nullptr);
            g_context->ClearRenderTargetView(g_target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (hidden)
            {
                // the picture once nothing runs any more and a few frames have settled the layout (or
                // after the seconds asked, whatever runs)
                settled_frames = app.settled() ? settled_frames + 1 : 0;
                bool due = opt.shot_after > 0 ? ImGui::GetTime() >= opt.shot_after : settled_frames >= 4;
                if (due)
                {
                    save_png(opt.screenshot);
                    break;
                }
                g_swap->Present(0, 0);
                if (app.busy())
                    Sleep(30);
            }
            else
                g_swap->Present(1, 0);
            if (frames_left > 0)
                --frames_left;
        }
        // the window's size for next time
        WINDOWPLACEMENT wp{sizeof wp};
        if (!hidden && GetWindowPlacement(hwnd, &wp))
        {
            Settings &s = app.settings();
            s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
            s.window_w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            s.window_h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
        if (!hidden || !opt.settings.empty())
            app.save();
        g_app = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
