#include "gui/theme.h"

#include <filesystem>
#include <string>

#include <windows.h>

namespace t4ff::gui
{
Palette palette;
Fonts fonts;

namespace
{
ImVec4 rgb(unsigned hex, float alpha = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

std::filesystem::path windows_fonts()
{
    wchar_t dir[MAX_PATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    return std::filesystem::path(n ? dir : L"C:\\Windows") / L"Fonts";
}

// Latin-1 and Latin Extended-A (map names), general punctuation (dashes, quotes, the ellipsis), arrows
const ImWchar GLYPHS[] = {0x0020, 0x017F, 0x2010, 0x2027, 0x2030, 0x203A, 0x2190, 0x2193, 0x2212, 0x2212, 0x00D7, 0x00D7, 0};

ImFont *add_font(const wchar_t *file, float size)
{
    ImGuiIO &io = ImGui::GetIO();
    std::filesystem::path path = windows_fonts() / file;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    auto utf8 = path.u8string();
    return io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char *>(utf8.c_str()), size, &cfg, GLYPHS);
}
} // namespace

void load_fonts(float scale)
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    fonts.ui = add_font(L"segoeui.ttf", 16.0f * scale);
    if (!fonts.ui)
    {
        // Windows without its fonts: ImGui's own, for everything
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        fonts.ui = fonts.bold = fonts.mono = fonts.title = io.Fonts->AddFontDefault(&cfg);
        io.Fonts->Build();
        return;
    }
    fonts.bold = add_font(L"seguisb.ttf", 16.0f * scale);
    fonts.title = add_font(L"seguisb.ttf", 21.0f * scale);
    fonts.mono = add_font(L"consola.ttf", 13.5f * scale);
    if (!fonts.bold)
        fonts.bold = fonts.ui;
    if (!fonts.title)
        fonts.title = fonts.bold;
    if (!fonts.mono)
        fonts.mono = fonts.ui;
    io.FontDefault = fonts.ui;
    io.Fonts->Build();
}

void apply_theme(bool dark, float scale)
{
    ImGuiStyle style;
    if (dark)
        ImGui::StyleColorsDark(&style);
    else
        ImGui::StyleColorsLight(&style);
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(9, 5);
    style.CellPadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.ItemInnerSpacing = ImVec2(6, 5);
    style.IndentSpacing = 18;
    style.ScrollbarSize = 13;
    style.GrabMinSize = 10;
    style.WindowRounding = 0;
    style.ChildRounding = 6;
    style.FrameRounding = 5;
    style.PopupRounding = 6;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 4;
    style.TabRounding = 5;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = dark ? 0.0f : 1.0f;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = ImVec2(0, 3);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4 *c = style.Colors;
    if (dark)
    {
        palette.accent = rgb(0xC9A24A);
        palette.accent_hover = rgb(0xD9B45E);
        palette.accent_active = rgb(0xB08A36);
        palette.on_accent = rgb(0x1B1508);
        palette.good = rgb(0x62C27E);
        palette.bad = rgb(0xEF6461);
        palette.warn = rgb(0xE6914A);
        palette.note = rgb(0x79AEDD);
        palette.dim = rgb(0x8E918F);
        palette.panel = rgb(0x1C1E21);
        palette.line = rgb(0x30332F);
        c[ImGuiCol_Text] = rgb(0xE8E4DA);
        c[ImGuiCol_TextDisabled] = palette.dim;
        c[ImGuiCol_WindowBg] = rgb(0x141517);
        c[ImGuiCol_ChildBg] = rgb(0x000000, 0);
        c[ImGuiCol_PopupBg] = rgb(0x1E2023);
        c[ImGuiCol_MenuBarBg] = rgb(0x1A1B1E);
        c[ImGuiCol_TitleBg] = rgb(0x26292D);
        c[ImGuiCol_TitleBgActive] = rgb(0x2E3136);
        c[ImGuiCol_TitleBgCollapsed] = rgb(0x26292D);
        c[ImGuiCol_Border] = palette.line;
        c[ImGuiCol_FrameBg] = rgb(0x24272A);
        c[ImGuiCol_FrameBgHovered] = rgb(0x2C2F33);
        c[ImGuiCol_FrameBgActive] = rgb(0x33373B);
        c[ImGuiCol_Button] = rgb(0x2A2D31);
        c[ImGuiCol_ButtonHovered] = rgb(0x363A3F);
        c[ImGuiCol_ButtonActive] = rgb(0x41464C);
        c[ImGuiCol_Header] = rgb(0xC9A24A, 0.20f);
        c[ImGuiCol_HeaderHovered] = rgb(0xC9A24A, 0.13f);
        c[ImGuiCol_HeaderActive] = rgb(0xC9A24A, 0.28f);
        c[ImGuiCol_CheckMark] = palette.accent;
        c[ImGuiCol_SliderGrab] = palette.accent;
        c[ImGuiCol_SliderGrabActive] = palette.accent_hover;
        c[ImGuiCol_PlotHistogram] = palette.accent;
        c[ImGuiCol_Separator] = palette.line;
        c[ImGuiCol_TableHeaderBg] = rgb(0x1C1E21);
        c[ImGuiCol_TableBorderStrong] = palette.line;
        c[ImGuiCol_TableBorderLight] = rgb(0x26292C);
        c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.02f);
        c[ImGuiCol_ScrollbarBg] = rgb(0x000000, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(0x3A3D41);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x46494E);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(0x52565B);
        c[ImGuiCol_TextSelectedBg] = rgb(0xC9A24A, 0.35f);
        c[ImGuiCol_NavCursor] = palette.accent;
        c[ImGuiCol_TextLink] = palette.accent;
        c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.55f);
        c[ImGuiCol_ResizeGrip] = rgb(0x000000, 0);
    }
    else
    {
        palette.accent = rgb(0x9A7322);
        palette.accent_hover = rgb(0xAA822E);
        palette.accent_active = rgb(0x84611A);
        palette.on_accent = rgb(0xFFFFFF);
        palette.good = rgb(0x1E7A3B);
        palette.bad = rgb(0xC1302B);
        palette.warn = rgb(0xB0570F);
        palette.note = rgb(0x235E9E);
        palette.dim = rgb(0x6C6E70);
        palette.panel = rgb(0xFBFAF7);
        palette.line = rgb(0xDAD6CC);
        c[ImGuiCol_Text] = rgb(0x1F2124);
        c[ImGuiCol_TextDisabled] = palette.dim;
        c[ImGuiCol_WindowBg] = rgb(0xF2F0EB);
        c[ImGuiCol_ChildBg] = rgb(0x000000, 0);
        c[ImGuiCol_PopupBg] = rgb(0xFFFFFF);
        c[ImGuiCol_MenuBarBg] = rgb(0xE9E6DF);
        c[ImGuiCol_TitleBg] = rgb(0xE9E6DF);
        c[ImGuiCol_TitleBgActive] = rgb(0xE2DED5);
        c[ImGuiCol_TitleBgCollapsed] = rgb(0xE9E6DF);
        c[ImGuiCol_Border] = palette.line;
        c[ImGuiCol_BorderShadow] = rgb(0x000000, 0);
        c[ImGuiCol_FrameBg] = rgb(0xFFFFFF);
        c[ImGuiCol_FrameBgHovered] = rgb(0xF7F5F0);
        c[ImGuiCol_FrameBgActive] = rgb(0xEFECE5);
        c[ImGuiCol_Button] = rgb(0xFFFFFF);
        c[ImGuiCol_ButtonHovered] = rgb(0xF1EEE7);
        c[ImGuiCol_ButtonActive] = rgb(0xE6E2D9);
        c[ImGuiCol_Header] = rgb(0x9A7322, 0.16f);
        c[ImGuiCol_HeaderHovered] = rgb(0x9A7322, 0.09f);
        c[ImGuiCol_HeaderActive] = rgb(0x9A7322, 0.24f);
        c[ImGuiCol_CheckMark] = palette.accent;
        c[ImGuiCol_SliderGrab] = palette.accent;
        c[ImGuiCol_SliderGrabActive] = palette.accent_active;
        c[ImGuiCol_PlotHistogram] = palette.accent;
        c[ImGuiCol_Separator] = palette.line;
        c[ImGuiCol_TableHeaderBg] = rgb(0xF3F1EC);
        c[ImGuiCol_TableBorderStrong] = palette.line;
        c[ImGuiCol_TableBorderLight] = rgb(0xE7E4DC);
        c[ImGuiCol_TableRowBgAlt] = rgb(0x000000, 0.02f);
        c[ImGuiCol_ScrollbarBg] = rgb(0x000000, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(0xC9C5BC);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0xB9B5AC);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(0xA9A59C);
        c[ImGuiCol_TextSelectedBg] = rgb(0x9A7322, 0.25f);
        c[ImGuiCol_NavCursor] = palette.accent;
        c[ImGuiCol_TextLink] = palette.accent;
        c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.30f);
        c[ImGuiCol_ResizeGrip] = rgb(0x000000, 0);
    }
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}
} // namespace t4ff::gui
