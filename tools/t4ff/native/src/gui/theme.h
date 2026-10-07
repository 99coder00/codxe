#pragma once

#include <imgui.h>

// The window's look: a dark and a light palette (brass on warm greys), the fonts of Windows (Segoe UI,
// Consolas for the log), everything scaled with the monitor's DPI.
namespace t4ff::gui
{
struct Palette
{
    ImVec4 accent, accent_hover, accent_active, on_accent; // the main action
    ImVec4 good, bad, warn, note, dim, panel, line;
};

struct Fonts
{
    ImFont *ui = nullptr, *bold = nullptr, *mono = nullptr, *title = nullptr;
};

extern Palette palette;
extern Fonts fonts;

void apply_theme(bool dark, float scale);
// (re)builds the font atlas for this scale
void load_fonts(float scale);

inline ImU32 color(const ImVec4 &c, float alpha = 1.0f)
{
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}
} // namespace t4ff::gui
