#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gui/settings.h"
#include "gui/worker.h"

// The converter window (the Python t4ff's gui.py, with a queue): the usermaps to convert, the Xbox 360
// fastfiles and the output folder; in the advanced mode every option of the conversion. Each command
// runs in a worker process (worker.h) whose output fills the log and whose @progress lines the bar.
namespace t4ff::gui
{
enum class JobState
{
    Waiting,
    Running,
    Done,
    Failed,
    Stopped,
};

struct Job
{
    int id = 0;
    MapEntry entry;
    std::string map_name, files; // what find_usermap found
    JobState state = JobState::Waiting;
    std::string memory, title, last_error; // from its output
    int warnings = 0, conversions = 0;
    double seconds = 0;
    fs::path out_dir;
    size_t log_start = 0; // its first line in the log
};

struct LogLine
{
    std::string text;
    uint8_t kind = 0;
};

class App
{
  public:
    App(void *window, Settings settings, fs::path settings_file, std::function<void()> wake, std::function<void()> quit);
    ~App();

    // what the workers reported: called every time round the loop, also while the window is minimized
    void update();
    // the window's content (between ImGui::NewFrame and ImGui::Render)
    void frame();
    // usermaps (folders, map fastfiles), Xbox 360 fastfiles, .iwd files or a picture, dropped on the
    // window or given on its command line: each goes where it belongs
    void add_paths(const std::vector<fs::path> &paths);
    // a usermap to the list (false and error when it is none)
    bool add_usermap(const fs::path &path, std::string &error);
    // the window's close button: asks first while something runs
    void request_close();
    void start_queue();
    void select_job(int index);
    // the options panel scrolled to this share of its height (screenshots of its lower sections)
    void scroll_options(float share)
    {
        options_scroll_ = share;
    }
    void save();

    bool busy() const
    {
        return running_task_ >= 0;
    }
    bool animating() const
    {
        return busy() || !deps_checked_;
    }
    bool settled() const // nothing runs and the dependencies are known (for screenshots)
    {
        return !busy() && deps_checked_ && !queue_running_;
    }
    bool want_dark() const;
    Settings &settings()
    {
        return s_;
    }

  private:
    struct Event
    {
        enum Kind
        {
            Line,
            Exit,
            Log,
            Deps,
        } kind;
        int task = 0;
        std::string text;
        int code = 0;
        std::optional<fs::path> ffmpeg, encoder;
    };

    // -- running commands
    void post(Event e);
    bool run_task(int task, const std::vector<std::string> &args, const std::string &label);
    void on_line(int task, std::string text);
    void on_exit(int task, int code);
    bool start_next_job();
    void stop();
    void write_codxe(const std::string &map);
    void check_dependencies();
    Job *find_job(int id);

    // -- the commands of the menus
    void add_usermap_folders();
    void add_map_fastfiles();
    void inspect_fastfile();
    void update_menu();
    void rewrite_streams();
    void set_up();

    // -- drawing
    void menu_bar();
    void main_panel();
    void queue_table(float height);
    void console_section();
    void output_section();
    void action_row();
    void log_panel();
    void options_panel();
    void selected_map_options();
    void codxe_options();
    void status_bar();
    void popups();
    void show_message(const std::string &title, const std::string &text);

    void add_log(const std::string &text, uint8_t kind);
    void refresh_advice();

    void *window_;
    Settings s_;
    fs::path settings_file_;
    std::function<void()> wake_, quit_;

    std::vector<Job> jobs_;
    int next_id_ = 1, selected_ = 0;
    bool queue_running_ = false, stop_requested_ = false;
    int queue_done_ = 0, queue_ok_ = 0; // the maps this run of the list finished, and converted

    Worker worker_;
    int running_task_ = -1; // a job's id, TOOL_TASK, or -1
    std::function<void(int)> tool_done_;
    std::string status_, step_;
    int done_ = 0, total_ = 0;
    std::chrono::steady_clock::time_point task_started_;
    bool setup_test_ok_ = false;

    std::mutex inbox_lock_;
    std::vector<Event> inbox_;

    std::thread deps_thread_;
    bool deps_checked_ = false;
    std::optional<fs::path> ffmpeg_, encoder_;

    std::vector<LogLine> log_;
    std::optional<size_t> scroll_to_line_;

    std::vector<std::string> advice_;
    std::string advice_key_;
    int console_selected_ = -1, iwd_selected_ = -1;
    double budget_fixed_ = 96;
    float options_scroll_ = -1;

    std::string banner_;
    fs::path banner_folder_;

    std::string message_title_, message_text_;
    bool open_message_ = false, open_close_ = false, open_about_ = false;
};
} // namespace t4ff::gui
