#include "gui/app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <fstream>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "app/deps.h"
#include "app/usermap.h"
#include "audio/audio.h"
#include "core/fastfile.h"
#include "gui/codxe_config.h"
#include "gui/shell.h"
#include "gui/theme.h"

namespace t4ff::gui
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int TOOL_TASK = 0;

// the log's colors
enum : uint8_t
{
    KIND_PLAIN,
    KIND_NOTE,
    KIND_WARNING,
    KIND_ERROR,
    KIND_COMMAND,
    KIND_GOOD,
};

constexpr size_t LOG_LIMIT = 200000;

const char *const TEXTURE_SIZES[] = {"No limit", "2048", "1024", "512", "256"};
const int TEXTURE_SIZE_VALUES[] = {0, 2048, 1024, 512, 256};
const char *const SOUND_RATES[] = {"Keep", "48000 Hz", "44100 Hz", "32000 Hz", "24000 Hz"};
const int SOUND_RATE_VALUES[] = {0, 48000, 44100, 32000, 24000};
const char *const STREAM_RATES[] = {"Keep", "44100 Hz", "32000 Hz", "24000 Hz", "22050 Hz"};
const int STREAM_RATE_VALUES[] = {0, 44100, 32000, 24000, 22050};

bool starts_with(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring lower(std::wstring s)
{
    for (wchar_t &c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// gui.flush_line's tags
uint8_t classify(const std::string &line)
{
    std::string l = lower_ascii(line);
    if (starts_with(l, "error") || starts_with(l, "traceback") || l.substr(0, 40).find("error:") != std::string::npos)
        return KIND_ERROR;
    if (starts_with(l, "warning"))
        return KIND_WARNING;
    return KIND_PLAIN;
}

// The worker writes UTF-8, but the names it takes from fastfiles are latin-1: a line that is not UTF-8
// is shown as latin-1.
std::string displayable(const std::string &s)
{
    size_t i = 0;
    bool valid = true;
    while (i < s.size() && valid)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size())
            valid = false;
        for (size_t k = 1; valid && k < n; ++k)
            valid = (static_cast<unsigned char>(s[i + k]) >> 6) == 2;
        i += n ? n : 1;
    }
    if (valid)
        return s;
    std::string out;
    for (char ch : s)
    {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x80)
            out += ch;
        else
        {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// a command line as the Python window shows it
std::string command_text(const std::vector<std::string> &args)
{
    std::string out;
    for (const std::string &a : args)
        out += (out.empty() ? "" : " ") + (a.find(' ') != std::string::npos ? "\"" + a + "\"" : a);
    return out;
}

std::string seconds_text(double s)
{
    char buf[32];
    if (s < 60)
        snprintf(buf, sizeof buf, "%.1f s", s);
    else
        snprintf(buf, sizeof buf, "%d min %02d s", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return buf;
}

std::string exit_text(int code)
{
    char buf[64];
    if (static_cast<unsigned>(code) >= 0xC0000000u)
        snprintf(buf, sizeof buf, "the converter crashed (0x%08X)", static_cast<unsigned>(code));
    else
        snprintf(buf, sizeof buf, "exit code %d", code);
    return buf;
}

// 1: a PC fastfile, 2: an Xbox 360 one, 0: none
int fastfile_kind(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    uint8_t header[12] = {};
    if (!f.read(reinterpret_cast<char *>(header), sizeof header))
        return 0;
    try
    {
        return fastfile_big_endian(header, sizeof header, "") ? 2 : 1;
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

// CoD Xenon's _codxe or _codxe\t4 folder (zone and usermaps side by side), or a folder of Xbox 360
// fastfiles: console fastfiles, not a usermap
bool console_folder(const fs::path &folder)
{
    std::error_code ec;
    if (lower(folder.filename().wstring()) == L"_codxe" || (fs::is_directory(folder / L"zone", ec) && fs::is_directory(folder / L"usermaps", ec)))
        return true;
    int checked = 0;
    for (const auto &e : fs::directory_iterator(folder, ec))
    {
        if (lower(e.path().extension().wstring()) != L".ff" || !e.is_regular_file(ec))
            continue;
        if (fastfile_kind(e.path()) == 2)
            return true;
        if (++checked >= 8)
            break;
    }
    return false;
}

bool has_iwds(const fs::path &folder)
{
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(folder, ec))
        if (lower(e.path().extension().wstring()) == L".iwd")
            return true;
    return false;
}

bool picture_file(const fs::path &path)
{
    std::wstring ext = lower(path.extension().wstring());
    for (const wchar_t *e : {L".png", L".jpg", L".jpeg", L".bmp", L".tga", L".dds", L".webp", L".iwi"})
        if (ext == e)
            return true;
    return false;
}

void add_unique(std::vector<std::string> &items, const std::string &item)
{
    if (std::find(items.begin(), items.end(), item) == items.end())
        items.push_back(item);
}

// -- drawing helpers

float em()
{
    return ImGui::GetFontSize();
}

float button_width(const char *label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

bool accent_button(const char *label, const ImVec2 &size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, palette.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.on_accent);
    ImGui::PushFont(fonts.bold);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return clicked;
}

void colored(const ImVec4 &c, const std::string &text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

void wrapped(const std::string &text, const ImVec4 *c = nullptr)
{
    if (c)
        ImGui::PushStyleColor(ImGuiCol_Text, *c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    if (c)
        ImGui::PopStyleColor();
}

void tooltip(const std::string &text)
{
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(em() * 30);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A section: its number in a circle, its title and a hint, when it fits before the right_reserved width
// (buttons placed at the line's end).
void section_title(int number, const char *title, const char *hint, float right_reserved)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = std::round(h * 0.38f);
    ImVec2 c(p.x + r, p.y + h * 0.5f);
    dl->AddCircleFilled(c, r, color(palette.accent), 24);
    char digits[4];
    snprintf(digits, sizeof digits, "%d", number);
    ImGui::PushFont(fonts.bold);
    ImVec2 ts = ImGui::CalcTextSize(digits);
    dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), color(palette.on_accent), digits);
    ImGui::Dummy(ImVec2(r * 2, h));
    ImGui::SameLine(0, em() * 0.55f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (hint)
    {
        ImGui::SameLine();
        float room = ImGui::GetContentRegionAvail().x - right_reserved - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::CalcTextSize(hint).x < room)
        {
            ImGui::AlignTextToFramePadding();
            colored(palette.dim, hint);
        }
        else
            ImGui::Dummy(ImVec2(0, h)); // (the line goes on)
    }
}

// what comes next (width wide) at the right end of the current line
void right_aligned(float width)
{
    ImGui::SameLine();
    float room = ImGui::GetContentRegionAvail().x;
    if (room > width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - width);
}

// A form row: the label on the left, the control (drawn next) filling the rest.
void field(const char *label, const char *help = nullptr)
{
    float label_w = std::min(em() * 9.5f, ImGui::GetContentRegionAvail().x * 0.45f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help)
        tooltip(help);
    ImGui::SameLine(label_w + ImGui::GetStyle().WindowPadding.x);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool check(const char *label, bool *v, const char *help)
{
    bool changed = ImGui::Checkbox(label, v);
    if (help)
        tooltip(help);
    return changed;
}

int index_of(const int *values, int count, int v)
{
    for (int i = 0; i < count; ++i)
        if (values[i] == v)
            return i;
    return 0;
}

// a list of paths and its buttons; true when it changed
bool path_list(const char *id, std::vector<std::string> &items, int &selected, int lines, const char *empty_hint,
               const std::function<std::vector<fs::path>()> &add_folders, const std::function<std::vector<fs::path>()> &add_files)
{
    bool changed = false;
    ImGui::PushID(id);
    const char *labels[] = {"Add folder\xE2\x80\xA6", "Add files\xE2\x80\xA6", "Remove"};
    float bw = 0;
    for (const char *l : labels)
        bw = std::max(bw, button_width(l));
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float h = std::max(lines * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2,
                       3 * ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().ItemSpacing.y);
    if (ImGui::BeginListBox("##list", ImVec2(ImGui::GetContentRegionAvail().x - bw - spacing, h)))
    {
        if (items.empty())
            colored(palette.dim, empty_hint);
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i].c_str(), selected == i))
                selected = i;
            tooltip(items[i]);
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button(labels[0], ImVec2(bw, 0)))
        for (const fs::path &p : add_folders())
        {
            add_unique(items, utf8_of(p));
            changed = true;
        }
    if (ImGui::Button(labels[1], ImVec2(bw, 0)))
        for (const fs::path &p : add_files())
        {
            add_unique(items, utf8_of(p));
            changed = true;
        }
    ImGui::BeginDisabled(selected < 0 || selected >= static_cast<int>(items.size()));
    if (ImGui::Button(labels[2], ImVec2(bw, 0)))
    {
        items.erase(items.begin() + selected);
        selected = std::min(selected, static_cast<int>(items.size()) - 1);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

ImVec4 state_color(JobState st)
{
    switch (st)
    {
    case JobState::Running: return palette.accent;
    case JobState::Done: return palette.good;
    case JobState::Failed: return palette.bad;
    case JobState::Stopped: return palette.warn;
    default: return palette.dim;
    }
}

void state_dot(JobState st)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    float r = std::round(h * 0.17f);
    float alpha = 1.0f;
    if (st == JobState::Running)
        alpha = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r + 1, p.y + h * 0.5f), r, color(state_color(st), alpha), 16);
    ImGui::Dummy(ImVec2(r * 2 + 2, h));
}

// a thin bar: a share of done, or a moving one (fraction < 0)
void thin_bar(float fraction, float width, float height)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float rounding = height * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    if (fraction >= 0)
    {
        float w = std::clamp(fraction, 0.0f, 1.0f) * width;
        if (w > 0)
            dl->AddRectFilled(p, ImVec2(p.x + std::max(w, height), p.y + height), color(palette.accent), rounding);
    }
    else
    {
        // a segment going back and forth
        float t = static_cast<float>(ImGui::GetTime());
        float seg = width * 0.25f;
        float x = (0.5f + 0.5f * std::sin(t * 2.2f)) * (width - seg);
        dl->AddRectFilled(ImVec2(p.x + x, p.y), ImVec2(p.x + x + seg, p.y + height), color(palette.accent), rounding);
    }
    ImGui::Dummy(ImVec2(width, height));
}
} // namespace

// -- the app

App::App(void *window, Settings settings, fs::path settings_file, std::function<void()> wake, std::function<void()> quit)
    : window_(window), s_(std::move(settings)), settings_file_(std::move(settings_file)), wake_(std::move(wake)), quit_(std::move(quit))
{
    std::string budget = budget_text(s_.texture_budget);
    if (budget != "auto")
        budget_fixed_ = strtod(budget.c_str(), nullptr);
    status_ = "Ready";
    add_log("Add the PC usermaps to convert (drop their folders here), the Xbox 360 fastfiles to take shaders from and an output folder, then Convert.",
            KIND_NOTE);
    check_dependencies();
}

App::~App()
{
    worker_.stop();
    worker_.wait();
    if (deps_thread_.joinable())
        deps_thread_.join();
}

bool App::want_dark() const
{
    if (s_.theme == "dark")
        return true;
    if (s_.theme == "light")
        return false;
    static bool dark = system_uses_dark_theme();
    static auto checked = Clock::now();
    if (Clock::now() - checked > std::chrono::seconds(2))
    {
        dark = system_uses_dark_theme();
        checked = Clock::now();
    }
    return dark;
}

void App::save()
{
    save_settings(s_, settings_file_);
}

void App::post(Event e)
{
    {
        std::lock_guard<std::mutex> guard(inbox_lock_);
        inbox_.push_back(std::move(e));
    }
    if (wake_)
        wake_();
}

void App::add_log(const std::string &text, uint8_t kind)
{
    if (log_.size() >= LOG_LIMIT)
    {
        size_t drop = LOG_LIMIT / 10;
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(drop));
        for (Job &j : jobs_)
            j.log_start = j.log_start > drop ? j.log_start - drop : 0;
    }
    log_.push_back({text, kind});
}

Job *App::find_job(int id)
{
    for (Job &j : jobs_)
        if (j.id == id)
            return &j;
    return nullptr;
}

void App::check_dependencies()
{
    if (deps_thread_.joinable())
        deps_thread_.join();
    deps_checked_ = false;
    deps_thread_ = std::thread([this] {
        // the encoder of this computer, installed from a .zip in the Downloads folder when it is one
        // (gui.find_and_install_encoder); a conversion finds the same one
        Event e{Event::Deps};
        try
        {
            e.encoder = ensure_xma2encode({}, [this](const std::string &m) { post({Event::Log, 0, "note: " + m}); });
        }
        catch (const std::exception &ex)
        {
            post({Event::Log, 0, std::string("note: cannot use the xma2encode.exe found: ") + ex.what()});
        }
        fs::path ff = ffmpeg_exe();
        if (!ff.empty())
            e.ffmpeg = ff;
        post(std::move(e));
    });
}

void App::refresh_advice()
{
    std::string key = std::to_string(encoder_.has_value()) + std::to_string(s_.no_sounds) + "|" + s_.xma_encoder;
    for (const std::string &z : s_.console_zones)
        key += "|" + z;
    if (key == advice_key_)
        return;
    advice_key_ = key;
    advice_ = advice(s_, encoder_.has_value() || !deps_checked_);
}

void App::update()
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> guard(inbox_lock_);
        events.swap(inbox_);
    }
    for (Event &e : events)
        switch (e.kind)
        {
        case Event::Line: on_line(e.task, std::move(e.text)); break;
        case Event::Exit: on_exit(e.task, e.code); break;
        case Event::Log: add_log(displayable(e.text), KIND_NOTE); break;
        case Event::Deps:
            deps_checked_ = true;
            ffmpeg_ = e.ffmpeg;
            encoder_ = e.encoder;
            advice_key_.clear();
            if (!encoder_)
                add_log("note: xma2encode.exe was not found on this computer: sounds are not encoded. It comes with Microsoft's Xbox developer kits "
                        "and cannot be downloaded automatically. Put it (or a .zip with it) in your Downloads folder, or choose it with "
                        "Tools > Set up, then set up again.",
                        KIND_NOTE);
            break;
        }
}

// -- running commands

bool App::run_task(int task, const std::vector<std::string> &args, const std::string &label)
{
    std::vector<std::wstring> wide{L"--progress-lines"};
    for (const std::string &a : args)
        wide.push_back(wide_of(a));
    add_log("$ t4ff-cli " + command_text(args), KIND_COMMAND);
    std::string error;
    bool ok = worker_.start(
        wide, [this, task](const std::string &line) { post({Event::Line, task, line}); },
        [this, task](int code) {
            Event e{Event::Exit, task};
            e.code = code;
            post(std::move(e));
        },
        error);
    if (!ok)
    {
        add_log("error: " + error, KIND_ERROR);
        return false;
    }
    running_task_ = task;
    status_ = label;
    step_.clear();
    done_ = total_ = 0;
    task_started_ = Clock::now();
    return true;
}

void App::on_line(int task, std::string text)
{
    text = displayable(text);
    if (starts_with(text, "@progress "))
    {
        // progress.parse: "@progress <done> <total> <step>"
        int done = 0, total = 0, used = 0;
        if (sscanf(text.c_str() + 10, "%d %d %n", &done, &total, &used) >= 2)
        {
            done_ = done;
            total_ = total;
            step_ = used ? text.substr(10 + static_cast<size_t>(used)) : std::string();
        }
        return;
    }
    uint8_t kind = classify(text);
    if (Job *j = find_job(task))
    {
        // the first zone written is the map's (gui.flush_line)
        if (starts_with(text, "  memory:") && j->memory.empty())
        {
            std::string m = text.substr(9);
            m = m.substr(0, m.find('('));
            m.erase(0, m.find_first_not_of(' '));
            m.erase(m.find_last_not_of(' ') + 1);
            j->memory = m;
        }
        if (starts_with(text, "map list name: \""))
        {
            size_t end = text.find('"', 16);
            if (end != std::string::npos)
                j->title = text.substr(16, end - 16);
        }
        if (text.find("converting again") != std::string::npos)
            ++j->conversions;
        if (kind == KIND_WARNING)
            ++j->warnings;
        if (kind == KIND_ERROR)
            j->last_error = text;
    }
    if (task == TOOL_TASK && starts_with(text, "xma2encode.exe works"))
        setup_test_ok_ = true;
    add_log(text, kind);
}

void App::on_exit(int task, int code)
{
    worker_.wait();
    running_task_ = -1;
    step_.clear();
    done_ = total_ = 0;
    double took = std::chrono::duration<double>(Clock::now() - task_started_).count();
    bool stopped = code == STOPPED_EXIT_CODE || stop_requested_;
    if (Job *j = find_job(task))
    {
        j->seconds = took;
        ++queue_done_;
        if (code == 0)
        {
            j->state = JobState::Done;
            ++queue_ok_;
            if (s_.codxe_settings)
                write_codxe(j->map_name);
            add_log("Finished: " + j->map_name + (j->memory.empty() ? "" : " needs " + j->memory) + " (" + seconds_text(took) + ").", KIND_GOOD);
            status_ = j->memory.empty() ? "Done" : "Done: the map needs " + j->memory;
        }
        else if (stopped)
        {
            j->state = JobState::Stopped;
            add_log("Stopped.", KIND_WARNING);
            status_ = "Stopped";
        }
        else
        {
            j->state = JobState::Failed;
            if (j->last_error.empty())
                j->last_error = exit_text(code);
            add_log("Failed (" + exit_text(code) + ").", KIND_ERROR);
            status_ = "Failed, see the log";
        }
        if (queue_running_ && !stop_requested_ && start_next_job())
            return;
        if (queue_running_)
        {
            if (queue_ok_)
            {
                fs::path target = path_of(s_.output) / L"_codxe";
                banner_ = "Copy " + utf8_of(target) + " into the World at War game folder on the console (or Xenia's).";
                banner_folder_ = target;
            }
            if (queue_done_ > 1)
                status_ = "Converted " + std::to_string(queue_ok_) + " of " + std::to_string(queue_done_) + " maps";
        }
        queue_running_ = false;
        stop_requested_ = false;
        return;
    }
    stop_requested_ = false;
    status_ = stopped ? "Stopped" : code == 0 ? "Ready" : "Failed, see the log";
    if (stopped)
        add_log("Stopped.", KIND_WARNING);
    else if (code != 0)
        add_log("Failed (" + exit_text(code) + ").", KIND_ERROR);
    auto done = std::move(tool_done_);
    tool_done_ = nullptr;
    if (done && !stopped)
        done(code);
}

bool App::start_next_job()
{
    for (Job &j : jobs_)
    {
        if (j.state != JobState::Waiting)
            continue;
        j.state = JobState::Running;
        j.memory.clear();
        j.title.clear();
        j.last_error.clear();
        j.warnings = 0;
        j.conversions = 1;
        add_log("", KIND_PLAIN);
        j.log_start = log_.size();
        fs::path out = path_of(s_.output) / L"_codxe";
        if (s_.t4_layout)
            out /= L"t4";
        j.out_dir = out / L"usermaps" / path_of(j.map_name);
        if (run_task(j.id, convert_args(s_, j.entry), "Converting " + j.map_name + "\xE2\x80\xA6"))
            return true;
        j.state = JobState::Failed;
    }
    return false;
}

void App::write_codxe(const std::string &map)
{
    // CoD Xe's codxe.json next to the converted maps, with the window's CoD Xe settings
    CodxeConfig c;
    c.log_console = s_.codxe_log_console;
    c.thread_watch = s_.codxe_thread_watch;
    c.dump_rawfile = s_.codxe_dump_rawfile;
    c.dump_map_ents = s_.codxe_dump_map_ents;
    c.active_mod = s_.codxe_active_mod;
    c.active_mod.erase(0, c.active_mod.find_first_not_of(" \t"));
    c.active_mod.erase(c.active_mod.find_last_not_of(" \t") + 1);
    c.start_map = s_.codxe_start_map ? map : std::string();
    c.start_command = s_.codxe_start_command;
    try
    {
        add_log("CoD Xe: " + write_codxe_config(codxe_config_path(path_of(s_.output), s_.t4_layout), c), KIND_NOTE);
    }
    catch (const std::exception &e)
    {
        add_log(std::string("warning: CoD Xe's settings: ") + e.what(), KIND_WARNING);
    }
}

void App::stop()
{
    if (!busy())
        return;
    stop_requested_ = true;
    status_ = "Stopping\xE2\x80\xA6";
    worker_.stop();
}

void App::start_queue()
{
    if (busy())
        return;
    std::vector<std::string> problems;
    int waiting = 0;
    for (const Job &j : jobs_)
        if (j.state == JobState::Waiting)
        {
            ++waiting;
            for (const std::string &p : check_settings(s_, j.entry))
                add_unique(problems, p);
        }
    if (!waiting)
    {
        show_message("Nothing to convert", "Add a usermap first: drop its folder (or its map .ff) on the window, or use Add folder.");
        return;
    }
    if (!problems.empty())
    {
        std::string text;
        for (const std::string &p : problems)
            text += (text.empty() ? "" : "\n") + p;
        show_message("Before converting", text);
        return;
    }
    save();
    for (const std::string &note : advice(s_, encoder_.has_value()))
        add_log("note: " + note, KIND_NOTE);
    banner_.clear();
    queue_running_ = true;
    stop_requested_ = false;
    queue_done_ = queue_ok_ = 0;
    if (!start_next_job())
        queue_running_ = false;
}

void App::select_job(int index)
{
    if (index >= 0 && index < static_cast<int>(jobs_.size()))
        selected_ = jobs_[index].id;
}

void App::request_close()
{
    if (busy())
    {
        open_close_ = true;
        return;
    }
    save();
    quit_();
}

void App::show_message(const std::string &title, const std::string &text)
{
    message_title_ = title;
    message_text_ = text;
    open_message_ = true;
}

// -- adding things

void App::add_paths(const std::vector<fs::path> &paths)
{
    for (const fs::path &p : paths)
    {
        std::error_code ec;
        std::string name = utf8_of(p);
        std::wstring ext = lower(p.extension().wstring());
        bool folder = fs::is_directory(p, ec);
        if (!folder && !fs::is_regular_file(p, ec))
        {
            add_log("warning: not found: " + name, KIND_WARNING);
            continue;
        }
        if (!folder && ext == L".ff" && fastfile_kind(p) == 2)
        {
            add_unique(s_.console_zones, name);
            add_log("Xbox 360 fastfile added: " + name, KIND_NOTE);
            continue;
        }
        if (folder && console_folder(p))
        {
            add_unique(s_.console_zones, name);
            add_log("Xbox 360 fastfiles added: " + name, KIND_NOTE);
            continue;
        }
        if (!folder && ext == L".iwd")
        {
            add_unique(s_.iwds, name);
            add_log("PC .iwd added: " + name, KIND_NOTE);
            continue;
        }
        if (!folder && picture_file(p))
        {
            if (Job *j = find_job(selected_))
            {
                j->entry.loading_image = name;
                add_log("loading picture of " + j->map_name + ": " + name, KIND_NOTE);
            }
            else
                add_log("warning: select a map first to give it this loading picture: " + name, KIND_WARNING);
            continue;
        }
        if (!folder && ext != L".ff")
        {
            add_log("warning: " + name + ": t4ff takes usermap folders, map fastfiles (.ff), Xbox 360 fastfiles and .iwd files", KIND_WARNING);
            continue;
        }
        std::string error;
        if (add_usermap(p, error))
            continue;
        // a folder of usermaps (the PC game's usermaps folder): each of them
        int added = 0;
        if (folder)
        {
            std::vector<fs::path> subfolders;
            for (const auto &e : fs::directory_iterator(p, ec))
                if (e.is_directory(ec))
                    subfolders.push_back(e.path());
            std::sort(subfolders.begin(), subfolders.end());
            std::string ignored;
            for (const fs::path &sub : subfolders)
                added += add_usermap(sub, ignored);
        }
        if (added)
            add_log("added the " + std::to_string(added) + " usermaps of " + name, KIND_NOTE);
        else if (folder && has_iwds(p))
        {
            add_unique(s_.iwds, name);
            add_log("PC .iwd folder added: " + name, KIND_NOTE);
        }
        else
            add_log("warning: not a usermap: " + error, KIND_WARNING);
    }
}

bool App::add_usermap(const fs::path &path, std::string &error)
{
    std::string name = utf8_of(path);
    Usermap m;
    try
    {
        m = find_usermap(path);
    }
    catch (const std::exception &e)
    {
        error = displayable(e.what());
        return false;
    }
    for (const Job &j : jobs_)
        if (j.entry.input == name && j.state == JobState::Waiting)
            return true; // waiting already
    Job j;
    j.id = next_id_++;
    j.entry.input = name;
    j.map_name = m.name;
    std::vector<std::string> parts{"map"};
    if (m.patch)
        parts.push_back("patch");
    if (m.mod)
        parts.push_back("mod");
    if (m.load)
        parts.push_back("load zone");
    if (!m.localized.empty())
        parts.push_back(std::to_string(m.localized.size()) + (m.localized.size() == 1 ? " language zone" : " language zones"));
    if (!m.iwds.empty())
        parts.push_back(std::to_string(m.iwds.size()) + " .iwd");
    for (const std::string &part : parts)
        j.files += (j.files.empty() ? "" : ", ") + part;
    jobs_.push_back(std::move(j));
    if (!selected_)
        selected_ = jobs_.back().id;
    return true;
}

void App::add_usermap_folders()
{
    fs::path start = jobs_.empty() ? fs::path() : path_of(jobs_.back().entry.input).parent_path();
    add_paths(pick_folders(window_, L"PC usermap folders (with <map>.ff, mod.ff, .iwd)", true, start));
}

void App::add_map_fastfiles()
{
    fs::path start = jobs_.empty() ? fs::path() : path_of(jobs_.back().entry.input);
    add_paths(pick_files(window_, L"PC map fastfiles", {{L"Fastfiles", L"*.ff"}, {L"All files", L"*.*"}}, true, start));
}

void App::inspect_fastfile()
{
    auto chosen = pick_files(window_, L"Fastfile to inspect (PC or Xbox 360)", {{L"Fastfiles", L"*.ff"}, {L"All files", L"*.*"}}, false);
    if (!chosen.empty())
        run_task(TOOL_TASK, {"info", utf8_of(chosen[0])}, "Reading\xE2\x80\xA6");
}

void App::update_menu()
{
    // gui.update_menu: CoD Xe's own custom maps list, from a menu zone among the Xbox 360 fastfiles given
    // (CoD Xenon's 0.3.0), else t4ff's list on CoD Xenon's 0.2.0 menu
    fs::path start = path_of(s_.output) / L"_codxe" / L"t4";
    auto chosen = pick_folders(window_, L"The _codxe\\t4 folder the game reads (with zone and usermaps)", false, start);
    if (chosen.empty())
        return;
    std::vector<std::string> args{"menu", utf8_of(chosen[0])};
    for (const std::string &z : s_.console_zones)
        args.insert(args.end(), {"--menu-zone", z});
    if (run_task(TOOL_TASK, args, "Updating the menu\xE2\x80\xA6"))
        tool_done_ = [this](int code) {
            if (code == 0)
                show_message("Custom Maps menu", "Custom Maps in the Nazi Zombies menu now shows every map of the usermaps folder.\n\n"
                                                 "See the log for which list: CoD Xe's own (CoD Xe r351 or later) or t4ff's (the CoD Xe build "
                                                 "with the usermaps list).");
        };
}

void App::rewrite_streams()
{
    auto chosen = pick_folders(window_, L"Folder of converted maps (their streamed sounds, .xma)", true, path_of(s_.output));
    if (chosen.empty())
        return;
    std::vector<std::string> args{"streams"};
    for (const fs::path &p : chosen)
        args.push_back(utf8_of(p));
    run_task(TOOL_TASK, args, "Rewriting streamed sounds\xE2\x80\xA6");
}

void App::set_up()
{
    // gui.set_up: find, install and test xma2encode.exe; find FFmpeg
    std::vector<std::string> args{"setup"};
    if (!s_.xma_encoder.empty())
        args.insert(args.end(), {"--xma2encode", s_.xma_encoder});
    setup_test_ok_ = false;
    if (run_task(TOOL_TASK, args, "Setting up\xE2\x80\xA6"))
        tool_done_ = [this](int) {
            check_dependencies();
            std::string text = std::string("FFmpeg: ") + (ffmpeg_ ? "found" : "missing (pictures are scaled without it)") + "\nxma2encode.exe: " +
                               (setup_test_ok_ ? "installed and working" : "not found (sounds will not be encoded)");
            if (!setup_test_ok_)
                text += "\n\nxma2encode.exe comes with Microsoft's Xbox developer kits and cannot be downloaded automatically. Put it (or a .zip "
                        "with it) in your Downloads folder, or choose it in the advanced mode's Sounds options, then set up again.";
            show_message("Set up", text);
        };
}

// -- drawing

void App::frame()
{
    refresh_advice();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("t4ff", nullptr, flags);
    menu_bar();
    float status_h = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    float body_h = ImGui::GetContentRegionAvail().y - status_h;
    if (s_.advanced)
    {
        float width = std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, em() * 22, em() * 30);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.panel);
        ImGui::BeginChild("options", ImVec2(width, body_h), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();
        options_panel();
        if (options_scroll_ >= 0)
            ImGui::SetScrollY(options_scroll_ * ImGui::GetScrollMaxY());
        ImGui::EndChild();
        ImGui::SameLine(0, em() * 0.9f);
    }
    ImGui::BeginChild("main", ImVec2(0, body_h));
    main_panel();
    ImGui::EndChild();
    status_bar();
    popups();
    ImGui::End();
}

void App::menu_bar()
{
    if (!ImGui::BeginMenuBar())
        return;
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Add usermap folders\xE2\x80\xA6"))
            add_usermap_folders();
        if (ImGui::MenuItem("Add map fastfiles\xE2\x80\xA6"))
            add_map_fastfiles();
        ImGui::Separator();
        std::error_code ec;
        if (ImGui::MenuItem("Open the output folder", nullptr, false, !s_.output.empty() && fs::is_directory(path_of(s_.output), ec)))
            open_folder(path_of(s_.output));
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            request_close();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools"))
    {
        if (ImGui::MenuItem("Inspect a fastfile\xE2\x80\xA6", nullptr, false, !busy()))
            inspect_fastfile();
        if (ImGui::MenuItem("Update the Custom Maps menu\xE2\x80\xA6", nullptr, false, !busy()))
            update_menu();
        tooltip("Makes Custom Maps in the Nazi Zombies menu list every map of a _codxe\\t4\\usermaps folder (with CoD Xenon's menu zone).");
        if (ImGui::MenuItem("Rewrite streamed sounds\xE2\x80\xA6", nullptr, false, !busy()))
            rewrite_streams();
        tooltip("Rewrites the streamed sounds (.xma) of maps converted before in the game's layout.");
        ImGui::Separator();
        if (ImGui::MenuItem("Set up xma2encode and FFmpeg\xE2\x80\xA6", nullptr, false, !busy()))
            set_up();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Simple", nullptr, !s_.advanced))
            s_.advanced = false;
        if (ImGui::MenuItem("Advanced", nullptr, s_.advanced))
            s_.advanced = true;
        ImGui::Separator();
        if (ImGui::BeginMenu("Theme"))
        {
            for (const char *t : {"system", "dark", "light"})
            {
                std::string label = t;
                label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                if (ImGui::MenuItem(label.c_str(), nullptr, s_.theme == t))
                    s_.theme = t;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About t4ff"))
            open_about_ = true;
        ImGui::EndMenu();
    }
    // the mode, at the right end
    const char *modes[] = {"Simple", "Advanced"};
    float w0 = ImGui::CalcTextSize(modes[0]).x + em(), w1 = ImGui::CalcTextSize(modes[1]).x + em();
    ImGui::SameLine(ImGui::GetWindowWidth() - w0 - w1 - ImGui::GetStyle().WindowPadding.x - ImGui::GetStyle().ItemSpacing.x);
    for (int i = 0; i < 2; ++i)
    {
        bool on = s_.advanced == (i == 1);
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Text, palette.accent);
        ImGui::PushFont(on ? fonts.bold : fonts.ui);
        if (ImGui::Selectable(modes[i], on, 0, ImVec2(i ? w1 - em() * 0.5f : w0 - em() * 0.5f, 0)))
            s_.advanced = i == 1;
        ImGui::PopFont();
        if (on)
            ImGui::PopStyleColor();
        if (i == 0)
            ImGui::SameLine();
    }
    ImGui::EndMenuBar();
}

void App::main_panel()
{
    const ImGuiStyle &style = ImGui::GetStyle();
    // a low window: fewer rows of the list, so the log keeps some room
    bool low = ImGui::GetContentRegionAvail().y < em() * 36;
    // 1: the usermaps
    {
        const char *add_folder = "Add folder\xE2\x80\xA6", *add_ff = "Add .ff\xE2\x80\xA6", *clear = "Clear finished";
        bool finished = std::any_of(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.state != JobState::Waiting && j.state != JobState::Running; });
        float buttons = button_width(add_folder) + button_width(add_ff) + style.ItemSpacing.x + (finished ? button_width(clear) + style.ItemSpacing.x : 0);
        section_title(1, "Usermaps", "their folders or map fastfiles; drop them here", buttons);
        right_aligned(buttons);
        if (finished)
        {
            if (ImGui::Button(clear))
            {
                jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                           [](const Job &j) { return j.state != JobState::Waiting && j.state != JobState::Running; }),
                            jobs_.end());
                if (!find_job(selected_))
                    selected_ = 0;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(add_folder))
            add_usermap_folders();
        ImGui::SameLine();
        if (ImGui::Button(add_ff))
            add_map_fastfiles();
        float row = ImGui::GetFrameHeight() + style.CellPadding.y * 2;
        int rows = std::clamp(static_cast<int>(jobs_.size()), low ? 2 : 3, low ? 3 : 6);
        queue_table(row * (rows + 1) + 2);
    }
    ImGui::Dummy(ImVec2(0, em() * 0.15f));
    console_section();
    ImGui::Dummy(ImVec2(0, em() * 0.15f));
    output_section();
    ImGui::Dummy(ImVec2(0, em() * 0.3f));
    action_row();
    log_panel();
}

void App::queue_table(float height)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    if (jobs_.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.panel);
        ImGui::BeginChild("empty", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();
        const char *line1 = "Drop usermap folders or map fastfiles (.ff) here";
        const char *line2 = "A usermap folder holds <map>.ff and, often, mod.ff, <map>_patch.ff and .iwd files.";
        ImVec2 size = ImGui::GetWindowSize();
        float total = ImGui::GetTextLineHeightWithSpacing() * 2;
        ImGui::SetCursorPos(ImVec2((size.x - ImGui::CalcTextSize(line1).x) * 0.5f, (size.y - total) * 0.5f));
        ImGui::PushFont(fonts.bold);
        colored(palette.dim, line1);
        ImGui::PopFont();
        ImGui::SetCursorPosX(std::max(style.WindowPadding.x, (size.x - ImGui::CalcTextSize(line2).x) * 0.5f));
        colored(palette.dim, line2);
        ImGui::EndChild();
        return;
    }
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchProp;
    const char *open = "Open", *again = "Again", *remove = "\xC3\x97";
    float actions = button_width(open) + button_width(again) + button_width(remove) + style.ItemSpacing.x * 2;
    if (!ImGui::BeginTable("queue", 5, flags, ImVec2(0, height)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch, 1.35f);
    ImGui::TableSetupColumn("Found", ImGuiTableColumnFlags_WidthStretch, 1.15f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, em() * 4.2f);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, actions);
    ImGui::TableHeadersRow();
    int remove_id = 0;
    for (size_t i = 0; i < jobs_.size(); ++i)
    {
        Job &j = jobs_[i];
        ImGui::PushID(j.id);
        ImGui::TableNextRow(0, ImGui::GetFrameHeight());
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable("##row", selected_ == j.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, ImGui::GetFrameHeight())))
        {
            selected_ = j.id;
            if (j.state != JobState::Waiting)
                scroll_to_line_ = j.log_start; // its lines in the log
        }
        ImGui::SameLine(0, 0);
        state_dot(j.state);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted(j.map_name.c_str());
        ImGui::PopFont();
        tooltip(j.entry.input + (j.title.empty() ? "" : "\nIn the map lists: " + j.title));
        if (!j.title.empty() && j.title != j.map_name)
        {
            // its name in the map lists, once the conversion found it
            ImGui::SameLine();
            colored(palette.dim, j.title);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        colored(palette.dim, j.files);
        tooltip(j.entry.input);

        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        switch (j.state)
        {
        case JobState::Waiting: colored(palette.dim, "Waiting"); break;
        case JobState::Running: {
            std::string text = "Converting";
            if (total_ > 0)
                text += " \xC2\xB7 " + std::to_string(static_cast<long long>(done_) * 100 / total_) + "%";
            if (j.conversions > 1)
                text += " (conversion " + std::to_string(j.conversions) + ")";
            colored(palette.accent, text);
            break;
        }
        case JobState::Done: {
            std::string text = j.memory.empty() ? "Done" : "Done \xC2\xB7 " + j.memory;
            colored(palette.good, text);
            if (j.warnings)
            {
                ImGui::SameLine();
                colored(palette.dim, std::to_string(j.warnings) + (j.warnings == 1 ? " warning" : " warnings"));
            }
            tooltip("Written to " + utf8_of(j.out_dir) + (j.conversions > 1 ? "\nConverted " + std::to_string(j.conversions) + " times to fit the memory target." : ""));
            break;
        }
        case JobState::Failed:
            colored(palette.bad, "Failed");
            tooltip(j.last_error);
            break;
        case JobState::Stopped: colored(palette.warn, "Stopped"); break;
        }

        ImGui::TableSetColumnIndex(3);
        ImGui::AlignTextToFramePadding();
        if (j.state == JobState::Running)
            colored(palette.dim, seconds_text(std::chrono::duration<double>(Clock::now() - task_started_).count()));
        else if (j.seconds > 0)
            colored(palette.dim, seconds_text(j.seconds));

        ImGui::TableSetColumnIndex(4);
        if (j.state == JobState::Done)
        {
            if (ImGui::Button(open))
            {
                std::error_code ec;
                open_folder(fs::is_directory(j.out_dir, ec) ? j.out_dir : path_of(s_.output));
            }
            tooltip(utf8_of(j.out_dir));
            ImGui::SameLine();
        }
        if (j.state == JobState::Done || j.state == JobState::Failed || j.state == JobState::Stopped)
        {
            if (ImGui::Button(again))
                j.state = JobState::Waiting;
            tooltip("Convert it again with the next Convert");
            ImGui::SameLine();
        }
        if (j.state != JobState::Running)
        {
            if (ImGui::Button(remove))
                remove_id = j.id;
            tooltip("Remove from the list");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove_id)
    {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job &j) { return j.id == remove_id; }), jobs_.end());
        if (selected_ == remove_id)
            selected_ = 0;
    }
}

void App::console_section()
{
    section_title(2, "Xbox 360 fastfiles", "for the shaders and the game's own assets", 0);
    path_list(
        "console", s_.console_zones, console_selected_, 3, "Best: the _codxe\\t4 folder of CoD Xenon's extracted zip",
        [this] { return pick_folders(window_, L"Xbox 360 fastfiles: CoD Xenon's _codxe\\t4 folder, or a folder of console .ff files", true); },
        [this] { return pick_files(window_, L"Xbox 360 fastfiles", {{L"Fastfiles", L"*.ff"}, {L"All files", L"*.*"}}, true); });
    for (const std::string &note : advice_)
        if (note.rfind("No xma2encode", 0) != 0)
            wrapped(note, &palette.warn);
}

void App::output_section()
{
    const char *browse = "Browse\xE2\x80\xA6", *open = "Open";
    const ImGuiStyle &style = ImGui::GetStyle();
    section_title(3, "Output folder", "a _codxe folder is made inside, to copy to the console", 0);
    float buttons = button_width(browse) + button_width(open) + style.ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - style.ItemSpacing.x);
    ImGui::InputTextWithHint("##output", "Choose where the converted maps go", &s_.output);
    ImGui::SameLine();
    if (ImGui::Button(browse))
    {
        auto chosen = pick_folders(window_, L"Output folder (a _codxe folder is created inside)", false, path_of(s_.output));
        if (!chosen.empty())
            s_.output = utf8_of(chosen[0]);
    }
    ImGui::SameLine();
    std::error_code ec;
    fs::path out = path_of(s_.output);
    bool exists = !s_.output.empty() && fs::is_directory(out, ec);
    ImGui::BeginDisabled(!exists);
    if (ImGui::Button(open))
        open_folder(fs::is_directory(out / L"_codxe", ec) ? out / L"_codxe" : out);
    ImGui::EndDisabled();
    // set in the advanced mode, they still apply in this one: said here
    if (s_.codxe_settings && !s_.advanced)
        wrapped(std::string("CoD Xe's settings (codxe.json) are written with each map") +
                    (s_.codxe_start_map ? ", and the game starts the map converted last" : "") + ": see the advanced mode.",
                &palette.note);
}

void App::action_row()
{
    const ImGuiStyle &style = ImGui::GetStyle();
    int waiting = static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.state == JobState::Waiting; }));
    std::string label = waiting > 1 ? "Convert " + std::to_string(waiting) + " maps" : "Convert";
    float height = std::round(ImGui::GetFrameHeight() * 1.45f);
    ImGui::BeginDisabled(busy() || jobs_.empty());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, style.FrameRounding * 1.3f);
    if (accent_button((label + "###convert").c_str(), ImVec2(std::max(em() * 10, button_width(label.c_str()) + em()), height)))
        start_queue();
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    if (busy())
    {
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(em() * 5, height)))
            stop();
    }
    ImGui::SameLine(0, em());
    // what runs: the step, its count and a bar
    ImGui::BeginGroup();
    float width = ImGui::GetContentRegionAvail().x;
    float text_h = ImGui::GetTextLineHeight();
    float bar_h = std::round(em() * 0.32f);
    float gap = std::max(0.0f, (height - text_h - bar_h - style.ItemSpacing.y * 0.5f) * 0.5f);
    ImGui::Dummy(ImVec2(0, std::max(0.0f, gap - style.ItemSpacing.y)));
    std::string text = status_;
    if (busy())
    {
        if (const Job *j = [&]() -> const Job * {
                for (const Job &q : jobs_)
                    if (q.id == running_task_)
                        return &q;
                return nullptr;
            }())
        {
            text = j->map_name;
            // of the maps of this run (those added meanwhile too)
            int total = queue_done_ + 1 +
                        static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &q) { return q.state == JobState::Waiting; }));
            if (total > 1)
                text += " (" + std::to_string(queue_done_ + 1) + " of " + std::to_string(total) + ")";
            text += " \xE2\x80\x94 ";
            text += step_.empty() ? "starting\xE2\x80\xA6" : step_;
        }
        else if (!step_.empty())
            text = step_;
        if (total_ > 0)
            text += ": " + std::to_string(done_) + "/" + std::to_string(total_) + " (" + std::to_string(static_cast<long long>(done_) * 100 / total_) +
                    "%)";
        else if (!step_.empty())
            text += "\xE2\x80\xA6";
    }
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + width, ImGui::GetCursorScreenPos().y + text_h), true);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopClipRect();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y * 0.5f);
    if (busy())
        thin_bar(total_ > 0 ? static_cast<float>(done_) / static_cast<float>(total_) : -1.0f, width, bar_h);
    else
        ImGui::Dummy(ImVec2(width, bar_h));
    ImGui::EndGroup();

    if (!banner_.empty())
    {
        ImGui::Dummy(ImVec2(0, em() * 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(palette.good.x, palette.good.y, palette.good.z, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(palette.good.x, palette.good.y, palette.good.z, 0.45f));
        ImGui::BeginChild("banner", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        const char *open = "Open it", *close = "\xC3\x97";
        float buttons = button_width(open) + button_width(close) + style.ItemSpacing.x;
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - buttons - style.ItemSpacing.x);
        ImGui::TextUnformatted(banner_.c_str());
        ImGui::PopTextWrapPos();
        right_aligned(buttons);
        if (ImGui::Button(open))
            open_folder(banner_folder_);
        ImGui::SameLine();
        if (ImGui::Button(close))
            banner_.clear();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }
}

void App::log_panel()
{
    const ImGuiStyle &style = ImGui::GetStyle();
    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    const char *copy = "Copy", *clear = "Clear";
    ImGui::PushFont(fonts.bold);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Log");
    ImGui::PopFont();
    right_aligned(button_width(copy) + button_width(clear) + style.ItemSpacing.x);
    if (ImGui::Button(copy))
    {
        std::string all;
        for (const LogLine &l : log_)
            all += l.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    tooltip("Copy the whole log");
    ImGui::SameLine();
    if (ImGui::Button(clear))
    {
        log_.clear();
        for (Job &j : jobs_)
            j.log_start = 0;
    }
    float height = std::max(ImGui::GetContentRegionAvail().y, em() * 6);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.panel);
    ImGui::BeginChild("log", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    ImGui::PushFont(fonts.mono);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, std::round(em() * 0.18f)));
    float line_h = ImGui::GetTextLineHeightWithSpacing();
    bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log_.size()), line_h);
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const LogLine &l = log_[static_cast<size_t>(i)];
            const ImVec4 *c = nullptr;
            switch (l.kind)
            {
            case KIND_NOTE: c = &palette.note; break;
            case KIND_WARNING: c = &palette.warn; break;
            case KIND_ERROR: c = &palette.bad; break;
            case KIND_COMMAND: c = &palette.dim; break;
            case KIND_GOOD: c = &palette.good; break;
            default: break;
            }
            if (c)
                ImGui::PushStyleColor(ImGuiCol_Text, *c);
            ImGui::TextUnformatted(l.text.empty() ? " " : l.text.c_str());
            if (c)
                ImGui::PopStyleColor();
        }
    // at the end, it follows the new lines; scrolled up (or sent to a map's lines), it stays there
    if (scroll_to_line_)
    {
        ImGui::SetScrollY(static_cast<float>(*scroll_to_line_) * line_h);
        scroll_to_line_.reset();
    }
    else if (follow)
        ImGui::SetScrollHereY(1.0f);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::EndChild();
}

void App::status_bar()
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x - ImGui::GetStyle().WindowPadding.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x + ImGui::GetStyle().WindowPadding.x, p.y),
                color(palette.line));
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.25f));
    auto item = [&](const char *name, const std::optional<fs::path> &found, const char *missing) {
        float h = ImGui::GetFrameHeight();
        ImVec2 q = ImGui::GetCursorScreenPos();
        float r = std::round(h * 0.14f);
        ImVec4 c = !deps_checked_ ? palette.dim : found ? palette.good : palette.bad;
        dl->AddCircleFilled(ImVec2(q.x + r, q.y + h * 0.5f), r, color(c), 12);
        ImGui::Dummy(ImVec2(r * 2, h));
        ImGui::SameLine(0, em() * 0.4f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        ImGui::SameLine(0, em() * 0.4f);
        std::string text = !deps_checked_ ? "looking\xE2\x80\xA6" : found ? utf8_of(*found) : missing;
        colored(palette.dim, text);
        tooltip(text);
    };
    item("FFmpeg", ffmpeg_, "missing: pictures are scaled without it");
    ImGui::SameLine(0, em() * 1.5f);
    std::optional<fs::path> encoder = encoder_;
    if (!s_.xma_encoder.empty())
        encoder = path_of(s_.xma_encoder);
    item("xma2encode.exe", s_.no_sounds ? std::optional<fs::path>(path_of("not used: sounds are skipped")) : encoder,
         "not found: sounds are not encoded");
    const char *setup = "Set up\xE2\x80\xA6";
    right_aligned(button_width(setup));
    ImGui::BeginDisabled(busy());
    if (ImGui::Button(setup))
        set_up();
    ImGui::EndDisabled();
    tooltip("Find (and install) xma2encode.exe and FFmpeg, and test the encoder");
}

void App::selected_map_options()
{
    Job *j = find_job(selected_);
    ImGui::PushFont(fonts.bold);
    ImGui::SeparatorText("Selected map");
    ImGui::PopFont();
    if (!j)
    {
        wrapped("Select a map in the list for its name in the map lists, its loading picture and its command line.", &palette.dim);
        return;
    }
    ImGui::PushFont(fonts.bold);
    ImGui::TextUnformatted(j->map_name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    colored(palette.dim, j->files);
    field("Name in the lists", "The map's name in the map lists and on its title card (--name). Empty: from the map's own files.");
    ImGui::InputTextWithHint("##name", "from the map's files", &j->entry.name);
    field("Loading picture", "A picture for the loading screen (--loading-image). Empty: the map's own loading screen.");
    const char *pick = "\xE2\x80\xA6";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button_width(pick) - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##picture", "the map's own", &j->entry.loading_image);
    ImGui::SameLine();
    if (ImGui::Button(pick))
    {
        auto chosen = pick_files(window_, L"Loading screen picture",
                                 {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds;*.webp;*.iwi"}, {L"All files", L"*.*"}}, false,
                                 path_of(j->entry.loading_image.empty() ? j->entry.input : j->entry.loading_image));
        if (!chosen.empty())
            j->entry.loading_image = utf8_of(chosen[0]);
    }
    std::string command = "t4ff-cli " + command_text(convert_args(s_, j->entry));
    ImGui::PushStyleColor(ImGuiCol_Text, palette.dim);
    bool open = ImGui::TreeNodeEx("Command line", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    tooltip("The same conversion from a terminal (t4ff-cli.exe)");
    if (open)
    {
        ImGui::PushFont(fonts.mono);
        wrapped(command, &palette.dim);
        ImGui::PopFont();
        if (ImGui::Button("Copy##command"))
            ImGui::SetClipboardText(command.c_str());
        ImGui::TreePop();
    }
}

void App::options_panel()
{
    selected_map_options();

    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    ImGui::PushFont(fonts.bold);
    ImGui::SeparatorText("Memory and textures");
    ImGui::PopFont();
    bool automatic = budget_text(s_.texture_budget) == "auto";
    field("Texture budget", "Texture memory in MiB. Automatic: as much texture quality as the memory target leaves (CoD Xenon's maps use 148-220 MiB).");
    if (ImGui::RadioButton("Automatic", automatic))
        s_.texture_budget = "auto";
    ImGui::SameLine();
    if (ImGui::RadioButton("Fixed", !automatic))
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%g", budget_fixed_);
        s_.texture_budget = budget_text(buf);
    }
    if (!automatic)
    {
        field("  MiB", "0: no limit");
        if (ImGui::InputDouble("##budget", &budget_fixed_, 8, 32, "%g"))
        {
            budget_fixed_ = std::max(0.0, budget_fixed_);
            char buf[32];
            snprintf(buf, sizeof buf, "%g", budget_fixed_);
            s_.texture_budget = budget_text(buf);
        }
    }
    field("Memory target", "Main memory the map's zone may use, in MiB. A console has about 222 MiB free for a map; more loads in Xenia only.");
    const double lo = 64, hi = 512;
    if (ImGui::SliderScalar("##memory", ImGuiDataType_Double, &s_.memory_target, &lo, &hi, "%.0f MiB"))
        s_.memory_target = std::round(std::clamp(s_.memory_target, lo, 1024.0));
    if (s_.memory_target > FREE_CONSOLE_MIB)
        wrapped("Over the memory a console has free for a map: the map will load in Xenia only.", &palette.warn);
    int size = index_of(TEXTURE_SIZE_VALUES, 5, s_.max_texture_size);
    field("Largest texture", "Bigger textures are scaled down to this size.");
    if (ImGui::Combo("##size", &size, TEXTURE_SIZES, 5))
        s_.max_texture_size = TEXTURE_SIZE_VALUES[size];
    check("No mip maps", &s_.no_mips, "Drops every mip level: about 25% less memory, but textures shimmer at a distance.");
    check("Keep uncompressed textures", &s_.no_compress, "Textures uncompressed on PC stay uncompressed (they are DXT compressed otherwise).");
    check("Keep the mip tails", &s_.keep_mip_tail,
          "Keep every texture's mip levels of 16 texels or less when the map is over its memory target (they are dropped otherwise).");

    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    ImGui::PushFont(fonts.bold);
    ImGui::SeparatorText("Texture streaming");
    ImGui::PopFont();
    check("Stream textures (images.pak)", &s_.stream_textures,
          "The textures of models and world surfaces keep their top mip level in the map's images.pak and load it near the player.");
    ImGui::BeginDisabled(!s_.stream_textures);
    field("PC stock textures", "MiB the PC versions of stock textures may add to the fastfile (--upgrade-budget, default 96).");
    if (ImGui::InputDouble("##upgrade", &s_.upgrade_budget, 8, 32, "%g MiB"))
        s_.upgrade_budget = std::max(0.0, s_.upgrade_budget);
    field("Deep streaming", "Images that stream two mip levels at once: all, or their names separated by commas (--deep-stream).");
    ImGui::InputTextWithHint("##deep", "all, or image names", &s_.deep_stream);
    field("Streaming distance", "How far around a surface its streamed textures load, a share of the disc linker's (--stream-growth). 0: the default.");
    if (ImGui::InputDouble("##growth", &s_.stream_growth, 0.1, 0.5, "%g"))
        s_.stream_growth = std::max(0.0, s_.stream_growth);
    check("Deep streamed textures keep a quarter", &s_.keep_quarter,
          "When the map is over its memory target, deep streamed textures keep a quarter of their size (an eighth otherwise).");
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    ImGui::PushFont(fonts.bold);
    ImGui::SeparatorText("Sounds");
    ImGui::PopFont();
    check("Skip sounds", &s_.no_sounds, "Do not convert sounds: the map references the console's.");
    ImGui::BeginDisabled(s_.no_sounds);
    field("xma2encode.exe", "The XMA encoder of the Xbox 360 XDK. Empty: the one found on this computer.");
    const char *pick = "\xE2\x80\xA6##encoder";
    std::string hint = encoder_ ? "found: " + utf8_of(*encoder_) : "not found";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button_width("\xE2\x80\xA6") - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##encoder", hint.c_str(), &s_.xma_encoder);
    ImGui::SameLine();
    if (ImGui::Button(pick))
    {
        auto chosen = pick_files(window_, L"xma2encode.exe (Xbox 360 XDK), or a .zip with it",
                                 {{L"Programs and zips", L"*.exe;*.zip"}, {L"All files", L"*.*"}}, false, path_of(s_.xma_encoder));
        if (!chosen.empty())
            s_.xma_encoder = utf8_of(chosen[0]);
    }
    field("XMA quality", "xma2encode's quality, 1-100 (default 60).");
    ImGui::SliderInt("##quality", &s_.xma_quality, 1, 100);
    s_.xma_quality = std::clamp(s_.xma_quality, 1, 100);
    int rate = index_of(SOUND_RATE_VALUES, 5, s_.sound_rate);
    field("Loaded sound rate", "The highest sample rate of loaded (in memory) sounds.");
    if (ImGui::Combo("##rate", &rate, SOUND_RATES, 5))
        s_.sound_rate = SOUND_RATE_VALUES[rate];
    int stream = index_of(STREAM_RATE_VALUES, 5, s_.stream_rate);
    field("Streamed sound rate", "Streamed sounds above this rate are resampled.");
    if (ImGui::Combo("##stream", &stream, STREAM_RATES, 5))
        s_.stream_rate = STREAM_RATE_VALUES[stream];
    check("Mono loaded sounds", &s_.mono_sounds, "Downmix loaded (in memory) sounds to mono.");
    ImGui::SameLine(em() * 11.5f);
    check("Mono streamed sounds", &s_.mono_streams, "Downmix streamed sounds to mono.");
    field("Loaded sounds", "The loaded sounds the map may have (default 1500; 0: no limit).");
    if (ImGui::InputInt("##loaded", &s_.max_loaded_sounds, 50, 250))
        s_.max_loaded_sounds = std::max(0, s_.max_loaded_sounds);
    field("Loaded sound memory", "Memory of the loaded sounds in MiB (default 32; 0: no limit).");
    if (ImGui::InputDouble("##soundmem", &s_.loaded_sound_memory, 4, 16, "%g MiB"))
        s_.loaded_sound_memory = std::max(0.0, s_.loaded_sound_memory);
    ImGui::EndDisabled();
    field("Threads", "Sounds encoded and compression threads at a time (0: one per processor).");
    int cpus = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    ImGui::SliderInt("##jobs", &s_.jobs, 0, cpus, s_.jobs ? "%d" : "one per processor");
    s_.jobs = std::clamp(s_.jobs, 0, 256);

    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    ImGui::PushFont(fonts.bold);
    ImGui::SeparatorText("The map's files");
    ImGui::PopFont();
    check("Skip mod.ff", &s_.no_mod, "Do not merge the usermap's mod.ff (and its language zones) into the map's fastfile.");
    ImGui::SameLine(em() * 11.5f);
    check("Skip _patch.ff", &s_.no_patch, "Do not merge the usermap's <map>_patch.ff into the map's fastfile.");
    check("Write the loading screen (_load.ff)", &s_.load_zone, "Write <map>_load.ff, the loading screen, as CoD Xenon's maps have.");
    check("CoD Xe's t4 layout", &s_.t4_layout, "Write _codxe\\t4\\usermaps\\<map>, CoD Xe's newer layout (otherwise _codxe\\usermaps\\<map>).");
    ImGui::PushStyleColor(ImGuiCol_Text, s_.allow_unverified ? palette.warn : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    check("Convert unverified assets", &s_.allow_unverified, "Also convert assets whose console layout is not verified: the game may crash.");
    ImGui::PopStyleColor();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Extra PC .iwd files");
    tooltip("Images and sounds the map takes from the PC game: e.g. the PC game's main folder.");
    path_list(
        "iwds", s_.iwds, iwd_selected_, 3, "e.g. the PC game's main folder",
        [this] { return pick_folders(window_, L"Folders of PC .iwd files (e.g. the PC game's main folder)", true); },
        [this] { return pick_files(window_, L"PC .iwd files", {{L"IWD archives", L"*.iwd"}, {L"All files", L"*.*"}}, true); });

    codxe_options();
}

void App::codxe_options()
{
    ImGui::Dummy(ImVec2(0, em() * 0.2f));
    ImGui::PushFont(fonts.bold);
    ImGui::SeparatorText("CoD Xe (codxe.json)");
    ImGui::PopFont();
    check("Write CoD Xe's settings with each map", &s_.codxe_settings,
          "After each map converted, these settings go into codxe.json in the output's _codxe folder, which CoD Xe reads when the game "
          "starts. The file's other settings stay as they are.");
    ImGui::BeginDisabled(!s_.codxe_settings);
    check("Start the map when the game starts", &s_.codxe_start_map,
          "startup_command: the game goes straight into the map converted last, every time it starts, until this is turned off and the "
          "settings written again (Write now, or the next map converted).");
    ImGui::SameLine();
    const char *commands[] = {"devmap", "map"};
    int command = s_.codxe_start_command == "map" ? 1 : 0;
    ImGui::SetNextItemWidth(std::max(em() * 5.5f, ImGui::GetContentRegionAvail().x));
    ImGui::BeginDisabled(!s_.codxe_start_map);
    if (ImGui::Combo("##startcommand", &command, commands, 2))
        s_.codxe_start_command = commands[command];
    tooltip("devmap: with cheats and developer commands, as the tests used; map: as the menus start it.");
    ImGui::EndDisabled();
    check("Console log (log_console)", &s_.codxe_log_console,
          "The game's console and script errors go to the debug output: Xenia's xenia.log, or xbWatson on a console.");
    check("Thread watch (thread_watch)", &s_.codxe_thread_watch,
          "With the console log: logs the game's longjmps, and what each thread runs when the level or its loading stops making progress "
          "(a map that hangs or crashes without an error).");
    check("Dump the scripts (dump_rawfile)", &s_.codxe_dump_rawfile, "The scripts the game loads are written to _codxe\\dump.");
    check("Dump the map's entities (dump_map_ents)", &s_.codxe_dump_map_ents, "The map's entity string is written to _codxe\\dump.");
    field("Active mod", "The mod CoD Xe loads: a folder of _codxe\\t4\\mods, e.g. mod_menu. Empty: left as the file has it.");
    ImGui::InputTextWithHint("##activemod", "unchanged", &s_.codxe_active_mod);
    // where it goes, and whether settings there stay
    std::error_code ec;
    fs::path config = codxe_config_path(path_of(s_.output), s_.t4_layout);
    if (s_.output.empty())
        wrapped("Choose an output folder: codxe.json goes into its _codxe folder.", &palette.dim);
    else if (fs::exists(config, ec))
        wrapped(utf8_of(config) + " (its other settings are kept)", &palette.dim);
    else
        wrapped(utf8_of(config) + " is new: copied to the console, it replaces the codxe.json there (give the active mod you use).", &palette.warn);
    const Job *j = find_job(selected_);
    bool can = !s_.output.empty() && (!s_.codxe_start_map || j);
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Write now"))
        write_codxe(j ? j->map_name : std::string());
    ImGui::EndDisabled();
    tooltip(s_.codxe_start_map ? "Writes these settings now, starting the selected map (" + (j ? j->map_name : std::string("select one")) + ")."
                               : std::string("Writes these settings now; a startup command the file has is removed."));
    ImGui::EndDisabled();
}

void App::popups()
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 center = vp->GetCenter();
    if (open_message_)
    {
        ImGui::OpenPopup("###message");
        open_message_ = false;
    }
    if (open_close_)
    {
        ImGui::OpenPopup("Quit t4ff?###close");
        open_close_ = false;
    }
    if (open_about_)
    {
        ImGui::OpenPopup("About t4ff###about");
        open_about_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(em() * 22, 0), ImVec2(em() * 34, FLT_MAX));
    if (ImGui::BeginPopupModal((message_title_ + "###message").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushTextWrapPos(em() * 32);
        ImGui::TextUnformatted(message_text_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, em() * 0.3f));
        if (accent_button("OK", ImVec2(em() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit t4ff?###close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextUnformatted("A conversion is running. Quit anyway? It stops.");
        ImGui::Dummy(ImVec2(0, em() * 0.3f));
        if (accent_button("Quit", ImVec2(em() * 6, 0)))
        {
            ImGui::CloseCurrentPopup();
            stop_requested_ = true;
            worker_.stop();
            save();
            quit_();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About t4ff###about", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::PushFont(fonts.title);
        ImGui::TextUnformatted("t4ff");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(em() * 30);
        ImGui::TextUnformatted("Converts World at War usermaps from the PC for the Xbox 360, for CoD Xe.");
        ImGui::Dummy(ImVec2(0, em() * 0.2f));
        colored(palette.dim, "Free software under the GNU General Public License, version 3: the structure layouts and zone code commands built "
                             "into it come from OpenAssetTools (GPL-3.0). It includes zlib (zlib license), Dear ImGui (MIT) and minimp3 (CC0).");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, em() * 0.3f));
        if (accent_button("OK", ImVec2(em() * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
} // namespace t4ff::gui
