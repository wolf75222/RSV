#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api/arrays.hpp"
#include "api/cluster.hpp"
#include "api/slurmjobs.hpp"
#include "api/usage.hpp"
#include "components/cluster_view.hpp"
#include "components/debug_view.hpp"
#include "components/footer.hpp"
#include "components/gpu_view.hpp"
#include "components/monitor_view.hpp"
#include "components/history_view.hpp"
#include "components/jobdetails.hpp"
#include "components/joblist.hpp"
#include "components/log_view.hpp"
#include "components/plan_view.hpp"
#include "components/quota_view.hpp"
#include "components/theme.hpp"
#include "components/title.hpp"

using namespace ftxui;

namespace {

constexpr const char* VERSION = "2.2.0";

// The signal that asked RSV to end (kill, hangup), handled once FTXUI's loop has returned: see main()
volatile std::sig_atomic_t quit_signal = 0;

extern "C" void recordQuitSignal(int signal) {
    quit_signal = signal;
}

/* Colors
- FTXUI decides between 24 bit and 256 colors itself, when its loop starts. TERM=xterm-256color makes it
  pick 256 colors before it looks at anything else, and over SSH nothing else reaches ROMEO anyway
  (COLORTERM and TERM_PROGRAM aren't forwarded).
- So RSV picks 24 bit colors, which nearly every terminal used to reach ROMEO has: the theme's RGB colors
  stay as they are instead of being rounded to the 256 palette.
- Except: NO_COLOR (no colors), the Linux console and old GNU screen (FTXUI's choice), Apple's Terminal
  before macOS 26 when run on the Mac (256)
- --color or RSV_COLOR forces a mode: truecolor, 256, 16, or auto (FTXUI's choice)
*/
std::optional<Terminal::Color> chooseColors(std::string mode) {
    if (mode.empty())
        if (const char* env = std::getenv("RSV_COLOR"); env && *env) mode = env;
    mode = util::lower(mode);

    if (mode == "truecolor" || mode == "24bit" || mode == "24-bit") return Terminal::Color::TrueColor;
    if (mode == "256") return Terminal::Color::Palette256;
    if (mode == "16") return Terminal::Color::Palette16;
    if (mode == "auto") return std::nullopt;

    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return util::lower(v ? v : "");
    };
    if (!env("NO_COLOR").empty()) return std::nullopt;
    const std::string term = env("TERM");
    if (term.empty() || term == "dumb" || term == "linux" || term == "vt100" || term == "vt220") return std::nullopt;
    if (util::startsWith(term, "screen") && env("TMUX").empty()) return Terminal::Color::Palette256;
    if (env("TERM_PROGRAM").find("apple_terminal") != std::string::npos) return Terminal::Color::Palette256;
    return Terminal::Color::TrueColor;
}

// Runs tasks one after the other on its own thread. A task that throws is dropped, the thread goes on.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() { stop(); }

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            tasks_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
                if (stop_) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                task();
            } catch (...) {
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false;
    std::thread thread_;   // last: started once the rest is ready
};

std::string base64(const std::string& in) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(chars[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

struct Options {
    int refresh = 15;
    std::string color;
    bool mouse = true;
};

const char* USAGE =
    "Usage: rsv [options]\n"
    "  A terminal viewer of your Slurm jobs: where they run, their logs, why they wait, what they used.\n"
    "\n"
    "  --refresh N       read the queue every N seconds (default 15, at least 5)\n"
    "  --color MODE      truecolor, 256, 16 or auto (default: truecolor, see docs/terminals.md)\n"
    "  --no-mouse        leave the mouse to the terminal (select text with it)\n"
    "  -V, --version     print the version\n"
    "  -h, --help        print this help\n"
    "\n"
    "  The same as environment variables: RSV_REFRESH, RSV_COLOR, RSV_MOUSE=0\n"
    "  Keys: ? in RSV. The project: https://github.com/wolf75222/RSV\n";

// The command line, over the environment variables
std::optional<Options> parseOptions(int argc, char* argv[], int& exit_code) {
    Options o;
    if (const char* v = std::getenv("RSV_REFRESH"); v && *v) o.refresh = static_cast<int>(util::toLong(v, 15));
    if (const char* v = std::getenv("RSV_MOUSE"); v && std::string(v) == "0") o.mouse = false;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&](const std::string& name) -> std::optional<std::string> {
            if (util::startsWith(a, name + "=")) return a.substr(name.size() + 1);
            if (a == name && i + 1 < argc) return std::string(argv[++i]);
            return std::nullopt;
        };
        if (a == "-h" || a == "--help") {
            std::cout << USAGE;
            exit_code = 0;
            return std::nullopt;
        }
        if (a == "-V" || a == "--version") {
            std::cout << "rsv " << VERSION << "\n";
            exit_code = 0;
            return std::nullopt;
        }
        if (a == "--no-mouse") {
            o.mouse = false;
        } else if (auto v = value("--refresh")) {
            o.refresh = static_cast<int>(util::toLong(*v, -1));
            if (o.refresh <= 0) {
                std::cerr << "rsv: --refresh needs a number of seconds\n";
                exit_code = 2;
                return std::nullopt;
            }
        } else if (auto c = value("--color")) {
            o.color = *c;
        } else {
            std::cerr << "rsv: unknown option " << a << "\n\n" << USAGE;
            exit_code = 2;
            return std::nullopt;
        }
    }
    o.refresh = std::max(5, o.refresh);
    return o;
}

/* The application: its state lives on the UI thread
- Slurm commands run on the `slurm` worker, log files are read on the `files` one: the UI never waits for them
- A worker gives its result back with post(), which runs it on the UI thread and redraws
*/
class Viewer {
public:
    Viewer(ScreenInteractive& screen, Options opts) : screen_(screen), opts_(std::move(opts)) {
        user_ = api::currentUser();
        char host[256] = {0};
        gethostname(host, sizeof host - 1);
        who_ = user_ + "@" + std::string(host).substr(0, std::string(host).find('.'));
        history_.reload = [this] { loadHistory(); };
        cluster_.reload = [this] { loadCluster(false); };
        cluster_.async = [this](std::function<std::function<void()>()> work) {
            slurm_.post([this, work = std::move(work)] {
                std::function<void()> done;
                try {
                    done = work();
                } catch (const std::exception&) {
                }
                if (done) post(std::move(done));
            });
        };
        cluster_.refresh_seconds = opts_.refresh;
        quota_.reload = [this] { loadQuota(); };
        quota_.user = user_;
        interval_ = opts_.refresh;
    }

    void start() { refresh(); }

    // Stops the workers (and the commands they run) before the rest of the Viewer goes away
    void stop() {
        api::stopping() = true;
        gpu_stream_.stop();
        monitor_worker_.stop();
        slurm_.stop();
        files_.stop();
    }

    // Called every 250 ms by the timer thread, on the UI thread
    void tick() {
        const auto now = Clock::now();
        if (!refreshing_ && now - last_refresh_ >= std::chrono::seconds(interval_)) refresh();
        if (view_ == View::Logs && logs_.follow && !log_loading_ && now - last_log_read_ >= std::chrono::seconds(2)) loadLog();
        if (view_ == View::Partitions && !cluster_.loading && now - cluster_time_ >= std::chrono::seconds(interval_))
            loadCluster(false);
        if (view_ == View::Monitor) sampleMonitor();
        if (view_ == View::None) requestPreview();
        if (!message_.empty() && now > message_until_) {
            message_.clear();
            screen_.PostEvent(Event::Custom);
        }
    }

    Element render();
    bool onEvent(Event e);

private:
    using Clock = std::chrono::steady_clock;
    enum class View { None, Help, Logs, Debug, History, Partitions, Quota, Confirm, Gpu, Plan, Script, Monitor };

    // A row of the list: a job, an array (its tasks folded under it), or one of its tasks while it is open
    struct Row {
        const api::Job* job = nullptr;         // the job or the task; nullptr: the array's own row
        const ui::JobArray* array = nullptr;   // the array of the row
        bool last = false;                     // the last task under its array (└)
        std::string key() const { return job ? ui::rowKey(*job) : array->shownId(); }
    };

    // What c (cancel) or H (hold, release) is about to do: shown first, done on y
    struct Action {
        enum Kind { Cancel, Hold, Release } kind = Cancel;
        struct Target {
            std::string id;      // for the command: 812345, 812402 (a whole array)
            std::string shown;   // as in the list: 812402_*
            std::string what;    // job, array
            std::string name, state, about;
            std::string waiting_of;   // an array whose waiting tasks are meant: they are read again at the last moment
        };
        std::vector<Target> targets;
        int left_out = 0;        // marked rows it doesn't apply to (ended, or not waiting for H)
    };

    // ---------------------------------------------------------------- threads

    // From a worker: runs `fn` on the UI thread, then draws
    void post(std::function<void()> fn) {
        screen_.Post(std::move(fn));
        screen_.PostEvent(Event::Custom);
    }

    /* The queue: your jobs, or your accounts' in the project view (o)
    - The arrays' tasks ended long ago come from sacct: once a minute, or at once when a new array shows up
    - squeue's time is measured: a slow one is asked less often (see onJobs)
    */
    void refresh() {
        if (refreshing_) return;
        refreshing_ = true;
        last_refresh_ = Clock::now();
        const bool past_due = Clock::now() - array_past_time_ >= std::chrono::minutes(1);
        const std::set<std::string> known = array_asked_;
        slurm_.post([this, user = user_, accounts = accounts_, project = project_, scope = scope_, past_due, known]() mutable {
            api::Fetched<std::vector<api::Job>> r;
            api::Fetched<std::map<std::string, std::vector<api::TaskState>>> past;
            std::set<std::string> asked;
            double took = 0;
            try {
                // The project view's accounts, the first time: those of yours that can run jobs
                if (project && accounts.empty()) accounts = api::runAccounts(user);
                const auto start = Clock::now();
                r = project ? api::fetchAccountJobs(accounts) : api::fetchJobs(user);
                took = std::chrono::duration<double>(Clock::now() - start).count();
                if (!r.error.empty()) {
                    const std::string controller = api::controllerProblem();
                    if (!controller.empty()) r.error += " · " + controller;
                } else {
                    std::set<std::string> arrays;
                    for (const auto& j : r.data)
                        if (const std::string a = api::arrayOf(j.id); !a.empty()) arrays.insert(a);
                    const bool new_array = std::any_of(arrays.begin(), arrays.end(), [&](const std::string& a) { return !known.count(a); });
                    if (!arrays.empty() && (past_due || new_array)) {
                        past = api::fetchArrayTasks(arrays, project);
                        asked = arrays;
                    }
                }
            } catch (const std::exception& e) {
                r.error = std::string("squeue: ") + e.what();
            }
            post([this, scope, took, accounts, asked = std::move(asked), r = std::move(r), past = std::move(past)]() mutable {
                if (scope != scope_) return;   // read before o switched between your jobs and the project's
                if (project_ && accounts_.empty()) accounts_ = accounts;
                // A failed sacct is asked again in a minute, not at every refresh
                if (!asked.empty()) {
                    array_past_time_ = Clock::now();
                    array_asked_.insert(asked.begin(), asked.end());
                    array_past_error_ = past.error;
                    if (past.error.empty()) array_past_ = std::move(past.data);
                }
                onJobs(std::move(r), took);
            });
        });
    }

    void onJobs(api::Fetched<std::vector<api::Job>> r, double took) {
        refreshing_ = false;
        last_refresh_ = Clock::now();
        /* Adaptive: a squeue that takes 2 s is asked every 20 s at most, never more often than --refresh; a failure
           (often a timeout from a loaded slurmctld) doubles the wait, up to 2 min. Back to normal when Slurm is. */
        const int longest = std::max(120, opts_.refresh);
        if (!r.error.empty()) interval_ = std::min(longest, std::max(interval_, opts_.refresh) * 2);
        else interval_ = std::clamp(static_cast<int>(took * 10 + 0.5), opts_.refresh, longest);
        cluster_.refresh_seconds = interval_;
        if (!r.error.empty()) {
            jobs_error_ = r.error;   // the last list stays on screen
            return;
        }
        jobs_error_.clear();
        jobs_ = std::move(r.data);
        jobs_time_ = std::time(nullptr);
        jobs_clock_ = Clock::now();
        buildArrays();

        // Forget the details of jobs that left the queue, their marks, their arrays
        auto listed = [&](const std::string& id) {
            return std::any_of(jobs_.begin(), jobs_.end(), [&](const api::Job& j) { return j.job_id == id; });
        };
        auto prune = [&](auto& by_job) {
            for (auto it = by_job.begin(); it != by_job.end();) it = listed(it->first) ? std::next(it) : by_job.erase(it);
        };
        prune(details_);
        prune(details_error_);
        prune(details_time_);
        prune(previews_);
        prune(preview_time_);
        prune(live_);
        prune(ranks_);
        for (auto it = marked_.begin(); it != marked_.end();) it = jobByKey(*it) || arrayByKey(*it) ? std::next(it) : marked_.erase(it);
        for (auto it = open_arrays_.begin(); it != open_arrays_.end();)
            it = arrayByKey(*it + "_*") ? std::next(it) : open_arrays_.erase(it);
        for (auto it = array_past_.begin(); it != array_past_.end();)
            it = arrayByKey(it->first + "_*") ? std::next(it) : array_past_.erase(it);
        for (auto it = array_asked_.begin(); it != array_asked_.end();)
            it = arrayByKey(*it + "_*") ? std::next(it) : array_asked_.erase(it);
        requestDetails(false);
    }

    // The arrays in the queue: their tasks under one row, with what sacct remembers of the others
    void buildArrays() {
        arrays_.clear();
        std::map<std::string, size_t> at;
        for (const auto& j : jobs_) {
            const std::string a = api::arrayOf(j.id);
            if (a.empty()) continue;
            auto it = at.find(a);
            if (it == at.end()) {
                it = at.emplace(a, arrays_.size()).first;
                arrays_.emplace_back();
                arrays_.back().id = a;
            }
            arrays_[it->second].tasks.push_back(&j);
        }
        for (auto& a : arrays_) {
            const auto past = array_past_.find(a.id);
            a.past_known = past != array_past_.end();
            a.progress = api::progressOf(a.tasks, a.past_known ? &past->second : nullptr);
        }
    }

    const api::Job* jobByKey(const std::string& key) const {
        for (const auto& j : jobs_)
            if (ui::rowKey(j) == key) return &j;
        return nullptr;
    }

    // In the project view the queue is everyone's: the limits (u) and the marks on the map (p) are about yours
    std::vector<api::Job> ownJobs() const {
        std::vector<api::Job> out;
        for (const auto& j : jobs_)
            if (j.user.empty() || j.user == user_) out.push_back(j);
        return out;
    }

    const ui::JobArray* arrayByKey(const std::string& key) const {
        for (const auto& a : arrays_)
            if (a.shownId() == key) return &a;
        return nullptr;
    }

    /* Details of the selected job (scontrol), read again once per refresh
    - A job counts as fresh only once its answer is there: moving fast through the list skips jobs, they are read
      when the selection comes back to them
    - One request at a time for the same job, however often the screen is drawn
    */
    void requestDetails(bool force) {
        const api::Job* j = selectedJob();
        if (!j) return;
        const std::string id = j->job_id;
        const auto when = details_time_.find(id);
        if (!force && when != details_time_.end() && Clock::now() - when->second < std::chrono::seconds(interval_ - 1)) return;
        if (!force && details_pending_ == id) return;
        details_pending_ = id;

        const uint64_t generation = ++details_generation_;
        const bool active = j->active();
        const bool pending = j->pending();
        const int cpus = j->cpus;
        const std::string shown_id = j->id;
        slurm_.post([this, id, shown_id, generation, active, pending, cpus] {
            // Moving fast through the list: only the job where it stops is read
            if (generation != details_generation_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (generation != details_generation_) return;

            api::Fetched<api::JobDetails> d;
            api::Fetched<std::vector<api::NodeInfo>> n;
            api::Fetched<api::LiveUse> live;
            api::Rank rank;
            try {
                d = api::fetchDetails(id);
                const std::string list = d.data.get("NodeList");
                if (d.error.empty() && active && !list.empty() && list != "(null)") n = api::fetchNodes(list);
                if (active) live = api::fetchLiveUse(id);
                if (pending) {
                    const auto all = api::fetchPriorities();
                    rank = api::rankOf(all.data, id);
                    if (rank.of == 0) rank = api::rankOf(all.data, shown_id);
                }
            } catch (const std::exception& e) {
                d.error = std::string("scontrol: ") + e.what();
            }
            post([this, id, cpus, d = std::move(d), n = std::move(n), live = std::move(live), rank]() mutable {
                if (live.error.empty() && live.data.steps > 0) {
                    const auto before = live_.find(id);
                    if (before != live_.end()) api::follow(live.data, before->second, cpus);
                    live_[id] = live.data;
                } else {
                    live_.erase(id);
                }
                if (rank.of > 0) ranks_[id] = rank;
                else ranks_.erase(id);
                details_time_[id] = Clock::now();   // an error too: asked again at the next refresh, not in a loop
                if (details_pending_ == id) details_pending_.clear();
                if (!d.error.empty()) {
                    details_error_[id] = d.error;
                } else {
                    details_error_.erase(id);
                    details_[id] = std::move(d.data);
                }
                for (auto& node : n.data) nodes_[node.name] = std::move(node);
                if (view_ == View::Debug && debug_job_ == id) fillDebug();
            });
        });
        requestPreview();
    }

    /* The last lines of the selected job's output, shown under its details
    - Read again every 5 s at most, and only when the file changed (size or date)
    */
    void requestPreview() {
        const api::Job* j = selectedJob();
        if (!j || j->pending()) return;
        const std::string id = j->job_id;
        const auto when = preview_time_.find(id);
        if (when != preview_time_.end() && Clock::now() - when->second < std::chrono::seconds(5)) return;
        preview_time_[id] = Clock::now();

        const std::string path = ui::logPaths(*j, detailsFor(*j)).first;
        if (path.empty()) return;
        const auto old = previews_.find(id);
        const bool same = old != previews_.end() && old->second.path == path;
        const uintmax_t old_size = same ? old->second.size : UINTMAX_MAX;
        const std::time_t old_mtime = same ? old->second.mtime : 0;
        files_.post([this, id, path, old_size, old_mtime] {
            struct stat st{};
            if (old_size != UINTMAX_MAX && stat(path.c_str(), &st) == 0 && static_cast<uintmax_t>(st.st_size) == old_size &&
                st.st_mtime == old_mtime)
                return;
            ui::LogData data = ui::readLogTail(path, 64u << 10);
            if (data.lines.size() > 200) data.lines.erase(data.lines.begin(), data.lines.end() - 200);
            post([this, id, data = std::move(data)]() mutable { previews_[id] = std::move(data); });
        });
    }

    void loadLog() {
        const std::string path = logs_.path();
        const uint64_t generation = ++log_generation_;
        log_loading_ = true;
        last_log_read_ = Clock::now();
        const uintmax_t old_size = logs_.data.path == path ? logs_.data.size : UINTMAX_MAX;
        const std::time_t old_mtime = logs_.data.mtime;
        files_.post([this, path, generation, old_size, old_mtime] {
            // Following: the file is only read again when it changed
            struct stat st{};
            if (old_size != UINTMAX_MAX && stat(path.c_str(), &st) == 0 && static_cast<uintmax_t>(st.st_size) == old_size &&
                st.st_mtime == old_mtime) {
                post([this, generation] {
                    if (generation == log_generation_) log_loading_ = false;
                });
                return;
            }
            ui::LogData data = ui::readLogTail(path);
            post([this, generation, data = std::move(data)]() mutable {
                if (generation != log_generation_) return;
                log_loading_ = false;
                if (view_ == View::Logs) logs_.setData(std::move(data));
            });
        });
    }

    void loadHistory() {
        history_.loading = true;
        history_time_ = Clock::now();
        slurm_.post([this, user = user_, days = history_.days] {
            api::Fetched<std::vector<api::HistoryJob>> r;
            try {
                r = api::fetchHistory(user, days);
            } catch (const std::exception& e) {
                r.error = std::string("sacct: ") + e.what();
            }
            post([this, r = std::move(r)]() mutable { history_.setData(std::move(r.data), r.error); });
        });
    }

    // `opening`: sdiag and the partition rules too, which change slowly
    void loadCluster(bool opening) {
        cluster_.loading = true;
        cluster_time_ = Clock::now();
        slurm_.post([this, opening] {
            api::Fetched<api::Cluster> r;
            api::Fetched<api::ClusterExtra> extra;
            try {
                r = api::fetchCluster();
                if (opening) extra = api::fetchClusterExtra();
            } catch (const std::exception& e) {
                r.error = std::string("sinfo: ") + e.what();
            }
            post([this, opening, r = std::move(r), extra = std::move(extra)]() mutable {
                cluster_.loading = false;
                cluster_.error = r.error;
                cluster_.data.ping = r.data.ping;
                if (r.error.empty()) cluster_.setData(std::move(r.data));
                if (opening) cluster_.extra = std::move(extra.data);
            });
        });
    }

    // The nodes the user's jobs run on, marked in the cluster view
    std::set<std::string> myNodes() const {
        std::set<std::string> out;
        for (const auto& j : jobs_)
            if (j.active() && (j.user.empty() || j.user == user_))
                for (const auto& h : api::expandHostlist(j.nodelist)) out.insert(h);
        return out;
    }

    void loadQuota() {
        quota_.loading = true;
        refresh();
        slurm_.post([this, user = user_] {
            api::Fetched<std::vector<api::Assoc>> r;
            api::Fetched<std::vector<api::Share>> shares;
            api::Fetched<std::vector<api::DiskQuota>> disks;
            try {
                r = api::fetchAssocs(user);
                shares = api::fetchShares(user);
                disks = api::fetchDiskQuotas(user);
            } catch (const std::exception& e) {
                r.error = std::string("sacctmgr: ") + e.what();
            }
            post([this, r = std::move(r), shares = std::move(shares), disks = std::move(disks)]() mutable {
                quota_.loading = false;
                quota_.error = r.error;
                if (r.error.empty()) quota_.assocs = std::move(r.data);
                quota_.shares = std::move(shares.data);
                quota_.disks = std::move(disks.data);
                quota_.disk_error = disks.error;
            });
        });
    }

    // GPUs of a running job: nvidia-smi in the job itself, only when asked (n)
    void openGpus(const api::Job& j) {
        gpu_ = ui::GpuView{};
        gpu_.title = "GPUs · job " + j.id;
        const std::string id = j.job_id, nodelist = j.nodelist;
        const int per_node = j.nodes > 0 ? j.gpus / j.nodes : j.gpus;
        gpu_.reload = [this, id, nodelist, per_node] {
            slurm_.post([this, id, nodelist, per_node] {
                api::Fetched<std::vector<api::GpuUse>> r;
                try {
                    r = api::fetchGpus(id, nodelist, per_node);
                } catch (const std::exception& e) {
                    r.error = std::string("srun: ") + e.what();
                }
                post([this, r = std::move(r)]() mutable {
                    gpu_.loading = false;
                    gpu_.gpus = std::move(r.data);
                    gpu_.error = r.error;
                });
            });
        };
        view_ = View::Gpu;
        gpu_.reload();
    }

    /* A running job, live (m): its CPUs and memory (sstat every 2 s), its GPUs (one srun step with nvidia-smi --loop-ms)
    - sstat runs on a worker of its own: a slow squeue on `slurm_` doesn't leave holes in the curves
    - Nothing is read once the view is closed, and the GPU step ends with it
    */
    void openMonitor(const api::Job& j) {
        closeMonitor();
        monitor_ = ui::MonitorView{};
        monitor_.title = "Live · job " + j.id + " " + util::cut(j.name, 40);
        monitor_.job_id = j.job_id;
        monitor_.state = j.state;
        monitor_.partition = j.partition;
        monitor_.cpus = std::max(1, j.cpus);
        monitor_.gpus = std::max(0, j.gpus);
        monitor_.nodes = std::max(1, j.nodes);
        monitor_.mem_limit_mib = j.mem_mib;
        monitor_.started = j.start;
        monitor_.limit = j.limit;
        monitor_.nodelist = j.nodelist;
        monitor_nodelist_ = j.nodelist;
        monitor_.reload = [this] {
            if (monitor_.ended) return;
            monitor_sstat_time_ = {};
            if (monitor_.gpus > 0 && !gpu_stream_.running()) startGpuStream();
            sampleMonitor();
        };
        view_ = View::Monitor;
        if (monitor_.gpus > 0) startGpuStream();
        sampleMonitor();
    }

    void closeMonitor() {
        monitor_generation_++;   // what is still on its way is dropped
        monitor_loading_ = false;
        gpu_stream_.stop();
    }

    // Every 250 ms while the view is open: sstat when 2 s have passed, and whether the job still runs
    void sampleMonitor() {
        const api::Job* j = nullptr;
        for (const auto& k : jobs_)
            if (k.job_id == monitor_.job_id) j = &k;
        if (!j || !j->active()) {
            if (j) monitor_.state = j->state;
            if (!monitor_.ended) closeMonitor();
            monitor_.ended = true;
            return;
        }
        if (monitor_loading_ || Clock::now() - monitor_sstat_time_ < std::chrono::seconds(2)) return;
        monitor_loading_ = true;
        monitor_sstat_time_ = Clock::now();
        // The GPU step once it runs: found by its name, then left out of the job's steps
        const bool find_own = monitor_.gpus > 0 && monitor_.own_step.empty() && gpu_stream_.running();
        monitor_worker_.post([this, generation = monitor_generation_, id = monitor_.job_id, find_own] {
            api::Fetched<std::vector<api::StepUse>> r;
            std::string own;
            try {
                if (find_own) own = api::stepNamed(id, api::liveStepName());
                r = api::fetchSteps(id);
            } catch (const std::exception& e) {
                r.error = std::string("sstat: ") + e.what();
            }
            const double at = ui::MonitorView::nowSeconds();
            post([this, generation, at, own, r = std::move(r)]() mutable {
                if (generation != monitor_generation_) return;
                monitor_loading_ = false;
                if (!own.empty()) monitor_.own_step = own;
                monitor_.setSteps(r.data, at, r.error);
            });
        });
    }

    // Each line of nvidia-smi is one GPU, given to the view on the UI thread
    void startGpuStream() {
        const auto hosts = api::expandHostlist(monitor_nodelist_);
        if (hosts.empty()) {
            monitor_.gpu_error = "the job has no node";
            return;
        }
        monitor_.gpu_waiting = true;
        monitor_.own_step.clear();   // a new step, found again by its name
        const int per_node = std::max(1, monitor_.gpus / monitor_.nodes);
        const uint64_t generation = monitor_generation_;
        gpu_stream_.start(
            api::gpuStreamArgs(monitor_.job_id, hosts.size(), per_node, 2000),
            [this, generation, hosts](const std::string& line) {
                api::GpuUse g;
                if (!api::parseNvidiaSmiLine(line, hosts, g)) return;
                post([this, generation, g] {
                    if (generation == monitor_generation_) monitor_.addGpu(g, ui::MonitorView::nowSeconds());
                });
            },
            [this, generation](const std::string& error) {
                if (error.empty()) return;
                post([this, generation, error] {
                    if (generation != monitor_generation_) return;
                    monitor_.gpu_waiting = false;
                    monitor_.gpu_error = error + " (r: start it again)";
                });
            });
    }

    // When would a job start (t): the form starts from the user's accounts, the partitions and the kinds of nodes
    void openPlan() {
        view_ = View::Plan;
        plan_.result = api::TestRun{};
        plan_.running = false;
        plan_.run = [this](const api::TestRequest& q) {
            slurm_.post([this, q] {
                api::TestRun t;
                try {
                    t = api::testOnly(q);
                } catch (const std::exception& e) {
                    t.message = std::string("sbatch: ") + e.what();
                }
                post([this, t] {
                    plan_.running = false;
                    plan_.result = t;
                });
            });
        };
        slurm_.post([this, user = user_] {
            std::vector<std::string> accounts, partitions, features;
            try {
                accounts = api::runAccounts(user);
                const api::RunResult info = api::run({"sinfo", "-h", "-o", "%P|%f"}, 15);
                for (const auto& line : util::lines(info.out)) {
                    const auto f = util::split(line, '|');
                    if (f.size() < 2) continue;
                    std::string p = util::trim(f[0]);
                    if (!p.empty() && p.back() == '*') p.pop_back();
                    if (!p.empty() && std::find(partitions.begin(), partitions.end(), p) == partitions.end()) partitions.push_back(p);
                    for (const auto& feature : util::split(util::trim(f[1]), ','))
                        if (!feature.empty() && feature != "(null)" && feature != "N/A" &&
                            std::find(features.begin(), features.end(), feature) == features.end())
                            features.push_back(feature);
                }
            } catch (const std::exception&) {
            }
            post([this, accounts, partitions, features] { plan_.setChoices(accounts, partitions, features); });
        });
        if (plan_.fields.empty()) plan_.setChoices({}, {}, {});
    }

    static bool held(const api::Job& j) { return util::startsWith(j.reason, "JobHeld"); }


    std::string about(const api::Job& j) const {
        if (j.active()) return "on " + std::to_string(j.nodes) + (j.nodes == 1 ? " node" : " nodes") + ", for " + util::duration(liveUsed(j));
        if (j.id.find("_[") != std::string::npos) return "only these " + std::to_string(api::taskCount(j.id)) + " waiting tasks";
        if (j.pending()) return "waiting: " + j.reason;
        return "";
    }

    /* Why H can't work on ROMEO (tried on its Slurm 23.11 in October 2026)
    - Its submit plugin refuses any change of a job that doesn't give a time limit: scontrol hold, uhold and release
      give none, and are refused ("Requested time limit is invalid")
    - A hold goes through with one (scontrol update TimeLimit=... Priority=0), but nothing releases the job then: a
      user can only release with scontrol release. RSV doesn't hold jobs that way: they would stay held.
    */
    static std::string holdRefused() {
        return "ROMEO refuses hold and release to users (no time limit in the change): cancel (c) and submit again";
    }

    static std::string about(const ui::JobArray& a) {
        std::string s;
        if (a.progress.running) s = std::to_string(a.progress.running) + " running";
        if (a.progress.pending) s += (s.empty() ? "" : ", ") + std::to_string(a.progress.pending) + " waiting";
        return s.empty() ? "" : "its tasks: " + s;
    }

    /* c and H act on the marked rows, else on the selected one, after a confirmation that lists them
    - c: jobs not ended yet, whole arrays (scancel 812402), the waiting tasks of an array (812402_[3-40])
    - H: holds the waiting ones (they keep their place but don't start), or releases them when all are held already.
      A running job can't be paused by its user (scontrol suspend is for admins).
    */
    void openAction(Action::Kind kind, const Row* selected) {
        if (kind != Action::Cancel && hold_refused_) {
            showMessage(holdRefused(), true);
            return;
        }
        std::vector<std::string> keys(marked_.begin(), marked_.end());
        if (keys.empty() && selected) keys.push_back(selected->key());
        if (kind != Action::Cancel) {
            bool any = false, all_held = true;
            for (const auto& k : keys) {
                const api::Job* w = jobByKey(k);
                if (!w)
                    if (const ui::JobArray* a = arrayByKey(k)) w = a->waiting();
                if (w && w->pending()) {
                    any = true;
                    all_held = all_held && held(*w);
                }
            }
            kind = any && all_held ? Action::Release : Action::Hold;
        }
        Action a;
        a.kind = kind;
        for (const auto& k : keys) {
            if (const api::Job* j = jobByKey(k)) {
                const bool fits = kind == Action::Cancel ? !j->finished() : j->pending() && held(*j) == (kind == Action::Release);
                if (!fits) {
                    a.left_out++;
                    continue;
                }
                // The waiting tasks of an array (812402_[159-200]): which ones they are now is read at the last moment
                const std::string waiting = j->id.find("_[") != std::string::npos ? api::arrayOf(j->id) : "";
                a.targets.push_back({api::plainId(j->id), j->id, "job", j->name, j->state, about(*j), waiting});
            } else if (const ui::JobArray* arr = arrayByKey(k)) {
                const api::Job* w = arr->waiting();
                const bool fits = kind == Action::Cancel ? arr->progress.running + arr->progress.pending > 0
                                                         : w && held(*w) == (kind == Action::Release);
                if (!fits) {
                    a.left_out++;
                    continue;
                }
                // Cancel: the whole array. Hold: only its waiting tasks, the running ones can't be held.
                const std::string how = kind == Action::Cancel ? about(*arr)
                                                               : std::to_string(arr->progress.pending) + " waiting tasks, the running ones go on";
                a.targets.push_back({arr->id, arr->shownId(), "array", arr->first().name, arr->state(), how,
                                     kind == Action::Cancel ? "" : arr->id});
            }
        }
        if (a.targets.empty()) {
            showMessage(kind == Action::Cancel ? "Nothing to cancel: already ended" : "Nothing to hold: H acts on waiting jobs", true);
            return;
        }
        action_ = std::move(a);
        view_ = View::Confirm;
    }

    void runAction() {
        const Action a = action_;
        view_ = View::None;
        const std::string what = a.targets.size() == 1
                                     ? (a.targets[0].what == "array" ? "Array " + a.targets[0].id : "Job " + a.targets[0].shown)
                                     : std::to_string(a.targets.size()) + " jobs";
        const char* doing = a.kind == Action::Cancel ? "Cancelling " : a.kind == Action::Hold ? "Holding " : "Releasing ";
        showMessage(doing + util::lower(what) + "...");
        slurm_.post([this, kind = a.kind, targets = a.targets, what] {
            /* The waiting tasks of an array as they are now, not as the last refresh saw them: tasks started since
               then would be cancelled or held with them. scancel --state=PENDING picks them itself. */
            std::string error;
            try {
                std::vector<std::string> ids, arrays;
                for (const auto& t : targets) {
                    if (t.waiting_of.empty()) ids.push_back(t.id);
                    else arrays.push_back(t.waiting_of);
                }
                if (kind == Action::Cancel) {
                    if (!ids.empty()) error = api::cancelJobs(ids);
                    for (const auto& array : arrays)
                        if (error.empty()) error = api::cancelJobs({"--state=PENDING", array});
                } else {
                    for (const auto& array : arrays)
                        for (const auto& w : api::waitingRecords(array)) ids.push_back(w);
                    error = ids.empty() ? "Nothing waits any more" : api::holdJobs(ids, kind == Action::Hold);
                }
            } catch (const std::exception& e) {
                error = std::string(kind == Action::Cancel ? "scancel: " : "scontrol: ") + e.what();
            }
            post([this, kind, what, error] {
                // ROMEO's submit plugin refuses every change without a time limit: hold and release among them
                if (kind != Action::Cancel && error.find("time limit is invalid") != std::string::npos) {
                    hold_refused_ = true;
                    showMessage(holdRefused(), true);
                    refreshing_ = false;
                    refresh();
                    return;
                }
                if (error.empty()) {
                    showMessage(what + (kind == Action::Cancel ? " cancelled" : kind == Action::Hold ? " held" : " released"));
                    marked_.clear();
                } else {
                    showMessage(std::string(kind == Action::Cancel ? "Not cancelled: " : kind == Action::Hold ? "Not held: " : "Not released: ") + error,
                                true);
                }
                refreshing_ = false;
                refresh();
            });
        });
    }

    // ---------------------------------------------------------------- list

    // For the sort by time left: running jobs by their time left, then pending ones by their expected start
    long leftOf(const api::Job& j) const {
        if (j.active() && j.limit > 0) return j.limit - liveUsed(j);
        if (j.pending() && j.start > 0) return static_cast<long>(j.start - std::time(nullptr)) + (1L << 40);
        return 1L << 50;
    }

    // The rows shown: filtered, then sorted; the tasks of the open arrays under them
    std::vector<Row> rows() const {
        const std::string f = util::lower(filter_);
        auto matches = [&](const api::Job& j) {
            if (f.empty()) return true;
            const std::string hay = util::lower(j.id + " " + j.name + " " + j.state + " " + api::shortState(j.state) + " " +
                                                j.partition + " " + j.account + " " + j.reason + " " + j.user);
            return hay.find(f) != std::string::npos;
        };
        std::vector<Row> top;
        for (const auto& j : jobs_)
            if (api::arrayOf(j.id).empty() && matches(j)) top.push_back({&j, nullptr, false});
        for (const auto& a : arrays_)
            if (std::any_of(a.tasks.begin(), a.tasks.end(), [&](const api::Job* t) { return matches(*t); })) top.push_back({nullptr, &a, false});

        auto state = [](const Row& r) { return r.job ? r.job->state : r.array->state(); };
        auto group = [&](const Row& r) { return api::isActive(state(r)) ? 0 : api::isPending(state(r)) ? 1 : 2; };
        auto number = [](const Row& r) { return util::toLong(r.job ? r.job->id : r.array->id, 0); };
        auto id = [](const Row& r) { return r.job ? r.job->id : r.array->id; };
        auto name = [](const Row& r) { return util::lower(r.job ? r.job->name : r.array->first().name); };
        auto left = [this](const Row& r) {
            if (r.job) return leftOf(*r.job);
            long best = 1L << 50;
            for (const api::Job* t : r.array->tasks) best = std::min(best, leftOf(*t));
            return best;
        };
        std::stable_sort(top.begin(), top.end(), [&](const Row& a, const Row& b) {
            switch (sort_) {
                case 1: return number(a) != number(b) ? number(a) > number(b) : id(a) > id(b);
                case 2: return name(a) < name(b);
                case 3: return left(a) < left(b);
                default:
                    if (group(a) != group(b)) return group(a) < group(b);
                    return number(a) != number(b) ? number(a) < number(b) : id(a) < id(b);
            }
        });

        std::vector<Row> out;
        for (const Row& r : top) {
            out.push_back(r);
            if (r.job || !open_arrays_.count(r.array->id)) continue;
            // Its tasks: running, then waiting, then ended, each by its index
            std::vector<const api::Job*> tasks = r.array->tasks;
            std::stable_sort(tasks.begin(), tasks.end(), [](const api::Job* a, const api::Job* b) {
                const int ga = a->active() ? 0 : a->pending() ? 1 : 2, gb = b->active() ? 0 : b->pending() ? 1 : 2;
                if (ga != gb) return ga < gb;
                return api::taskIndex(a->id) < api::taskIndex(b->id);
            });
            for (size_t i = 0; i < tasks.size(); i++) out.push_back({tasks[i], r.array, i + 1 == tasks.size()});
        }
        return out;
    }

    static const char* sortName(int s) {
        static const char* names[] = {"state", "newest", "name", "time left"};
        return names[s % 4];
    }

    // The job (or task) of the selected row; nullptr on an array's own row
    const api::Job* selectedJob() const { return jobByKey(selected_id_); }

    // The selection follows its row (its key) through refreshes and sorts; when it leaves, the row below takes it
    int selectedIndex(const std::vector<Row>& list) {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].key() == selected_id_) return selected_pos_ = static_cast<int>(i);
        if (list.empty()) return -1;
        selected_pos_ = std::clamp(selected_pos_, 0, static_cast<int>(list.size()) - 1);
        selected_id_ = list[static_cast<size_t>(selected_pos_)].key();
        details_offset_ = 0;
        requestDetails(false);
        return selected_pos_;
    }

    void select(const std::vector<Row>& list, int index) {
        if (list.empty()) return;
        index = std::clamp(index, 0, static_cast<int>(list.size()) - 1);
        if (list[static_cast<size_t>(index)].key() == selected_id_) return;
        selected_pos_ = index;
        selected_id_ = list[static_cast<size_t>(index)].key();
        details_offset_ = 0;
        requestDetails(false);
    }

    // The last column of an array: the soonest end of its running tasks, else when the others should start
    std::string arrayTime(const ui::JobArray& a, std::time_t now) const {
        long best = -1;
        for (const api::Job* t : a.tasks)
            if (t->active() && t->limit > 0) {
                const long l = std::max(0L, t->limit - liveUsed(*t));
                if (best < 0 || l < best) best = l;
            }
        if (best >= 0) return util::duration(best);
        if (const api::Job* w = a.waiting()) return ui::timeColumn(*w, 0, now);
        return "";
    }

    // squeue's run time, plus the time since it was read: the clocks keep going between refreshes
    long liveUsed(const api::Job& j) const {
        if (!j.active() || j.used < 0) return j.used;
        return j.used + static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - jobs_clock_).count());
    }

    const api::JobDetails* detailsFor(const api::Job& j) const {
        const auto it = details_.find(j.job_id);
        return it == details_.end() ? nullptr : &it->second;
    }

    void showMessage(const std::string& m, bool error = false) {
        message_ = m;
        message_error_ = error;
        message_until_ = Clock::now() + std::chrono::seconds(error ? 8 : 4);
    }

    /* Copy through the terminal (OSC 52): it reaches the clipboard of the computer in front of the user, over SSH
    - xclip/xsel would copy on the login node, where there is no display
    */
    void copy(const std::string& s, const std::string& what, const std::string& shown = "") {
        std::cout << "\033]52;c;" << base64(s) << "\a" << std::flush;
        showMessage("Copied " + what + ": " + (shown.empty() ? s : shown));
    }

    void fillDebug() {
        const auto it = details_.find(debug_job_);
        if (it != details_.end()) debug_.setText(it->second.raw);
        const auto err = details_error_.find(debug_job_);
        debug_.error = err == details_error_.end() ? "" : err->second;
        if (it == details_.end() && !debug_.error.empty()) debug_.loading = false;
    }

    // ---------------------------------------------------------------- views

    void openLogs(const api::Job& j) {
        const auto [out, err] = ui::logPaths(j, detailsFor(j));
        logs_ = ui::LogView{};
        logs_.title = "Log · " + j.id + " " + util::cut(j.name, 40);
        logs_.paths[0] = out;
        logs_.paths[1] = err;
        logs_.follow = true;
        logs_.loading = true;
        logs_.reload = [this] { loadLog(); };
        logs_.copy = [this](const std::string& p) { copy(p, "path"); };
        view_ = View::Logs;
        loadLog();
    }

    void openDebug(const api::Job& j) {
        debug_ = ui::DebugView{};
        debug_.title = "scontrol show job " + j.job_id;
        debug_job_ = j.job_id;
        view_ = View::Debug;
        fillDebug();
        requestDetails(true);
    }

    // The script it was submitted with (b), as Slurm keeps it
    void openScript(const api::Job& j) {
        script_ = ui::DebugView{};
        script_.script = true;
        const uint64_t generation = ++script_generation_;
        script_.title = "Script · job " + j.id;
        view_ = View::Script;
        script_.copy = [this] {
            std::string all;
            for (const auto& l : script_.lines) all += l + "\n";
            copy(all, "the script", std::to_string(script_.lines.size()) + " lines");
        };
        slurm_.post([this, generation, id = j.job_id, array = api::arrayOf(j.id), ended = j.finished()] {
            api::Fetched<std::string> r;
            try {
                r = api::fetchScript(id, array, ended);
            } catch (const std::exception& e) {
                r.error = std::string("scontrol: ") + e.what();
            }
            post([this, generation, r = std::move(r)] {
                if (view_ != View::Script || generation != script_generation_) return;   // another job's, asked before
                if (r.error.empty()) script_.setText(r.data);
                script_.loading = false;
                script_.error = r.error;
            });
        });
    }

    /* The project view (o): everyone's jobs in your accounts, read-only
    - It starts at once: the accounts are read by its first refresh, so nothing can happen in between
    */
    void toggleProject() { setScope(!project_); }

    void setScope(bool project) {
        project_ = project;
        scope_++;
        jobs_.clear();
        arrays_.clear();
        jobs_time_ = 0;
        jobs_error_.clear();
        marked_.clear();
        selected_id_.clear();
        selected_pos_ = 0;
        list_offset_ = 0;
        refreshing_ = false;
        refresh();
        showMessage(project ? "Project view, read-only: your accounts' jobs (o: yours)" : "Your jobs");
    }

    std::string accountList() const {
        std::string s;
        for (const auto& a : accounts_) s += (s.empty() ? "" : " ") + a;
        return s;
    }

    Element confirmView() const;
    bool mainEvent(Event& e);
    bool filterEvent(Event& e);
    bool viewEvent(Event& e);

    // ---------------------------------------------------------------- state

    ScreenInteractive& screen_;
    Options opts_;
    std::string user_, who_;

    std::vector<api::Job> jobs_;
    std::string jobs_error_;
    std::time_t jobs_time_ = 0;
    Clock::time_point jobs_clock_ = Clock::now();
    Clock::time_point last_refresh_ = Clock::now();
    bool refreshing_ = false;

    std::map<std::string, api::JobDetails> details_;           // by Slurm job id
    std::map<std::string, std::string> details_error_;
    std::map<std::string, Clock::time_point> details_time_;
    std::map<std::string, api::NodeInfo> nodes_;
    std::atomic<uint64_t> details_generation_{0};
    std::string details_pending_;                               // the job whose details were asked last, until they come
    std::map<std::string, ui::LogData> previews_;              // the end of each job's output
    std::map<std::string, api::LiveUse> live_;                  // what a running job uses now (sstat)
    std::map<std::string, api::Rank> ranks_;                    // a pending job's place in the queue (sprio)
    std::map<std::string, Clock::time_point> preview_time_;
    std::vector<ui::JobArray> arrays_;                          // the arrays in jobs_, their tasks under one row
    std::map<std::string, std::vector<api::TaskState>> array_past_;   // what sacct remembers of their tasks
    std::string array_past_error_;
    std::set<std::string> array_asked_;                         // the arrays sacct was asked about, answered or not
    Clock::time_point array_past_time_{};                       // sacct is read once a minute at most
    std::set<std::string> open_arrays_;                         // arrays shown with their tasks (Enter)
    std::set<std::string> marked_;                              // rows marked with Space: job ids, 812402_*
    bool project_ = false;                                      // o: everyone's jobs in your accounts
    bool hold_refused_ = false;                                 // Slurm refused H once: it says why at once now
    int help_offset_ = 0;                                       // the help scrolls on a short screen
    std::vector<std::string> accounts_;
    uint64_t scope_ = 0;                                        // one more at each switch between the two
    int interval_ = 15;                                         // seconds between two readings of the queue

    std::string selected_id_;
    int selected_pos_ = 0;
    int list_offset_ = 0;
    int details_offset_ = 0;
    int list_page_ = 10;
    int details_page_ = 10;
    int sort_ = 0;
    std::string filter_;
    bool editing_filter_ = false;
    Box list_box_, details_box_;

    View view_ = View::None;
    ui::LogView logs_;
    ui::DebugView debug_;
    std::string debug_job_;
    ui::HistoryView history_;
    Clock::time_point history_time_ = Clock::now();
    ui::ClusterView cluster_;
    Clock::time_point cluster_time_ = Clock::now();
    ui::QuotaView quota_;
    ui::GpuView gpu_;
    ui::MonitorView monitor_;
    std::string monitor_nodelist_;
    uint64_t monitor_generation_ = 0;
    bool monitor_loading_ = false;
    Clock::time_point monitor_sstat_time_{};
    ui::PlanView plan_;
    ui::DebugView script_;
    uint64_t script_generation_ = 0;
    Action action_;
    uint64_t log_generation_ = 0;
    bool log_loading_ = false;
    Clock::time_point last_log_read_ = Clock::now();

    std::string message_;
    bool message_error_ = false;
    Clock::time_point message_until_ = Clock::now();

    // Last: they stop (and join) first when the Viewer goes away, while the rest is still there
    Worker slurm_;
    Worker files_;
    Worker monitor_worker_;      // sstat for the live view only
    api::LineStream gpu_stream_;   // its GPU step: stopped before the rest
};

Element Viewer::render() {
    const Dimensions term = Terminal::Size();
    const int W = term.dimx, H = term.dimy;
    if (W < 40 || H < 10)
        return vbox({filler(), text("RSV: terminal too small") | bold | hcenter,
                     text(std::to_string(W) + "×" + std::to_string(H) + ", 40×10 at least") | dim | hcenter, filler()});

    const auto list = rows();
    const int sel = selectedIndex(list);
    const Row* row = sel >= 0 ? &list[static_cast<size_t>(sel)] : nullptr;
    const api::Job* job = row ? row->job : nullptr;
    const ui::JobArray* array = row && !row->job ? row->array : nullptr;
    const std::time_t now = std::time(nullptr);

    /* Layout
    - Wide: the list on the left, as wide as its names need, between 34 columns and 45% of the screen
    - Narrow (< 90 columns): the list above, the details below
    */
    const bool side = W >= 90;
    const int body_h = H - 2;
    ui::ListColumns cols;
    size_t id_w = 5, name_w = 10, user_w = 4;
    for (const Row& r : list) {
        id_w = std::max(id_w, util::width(r.key()));
        // An array's name shares its column with the bar and the count of its tasks
        name_w = std::max(name_w, r.job ? util::width(r.job->name) + (r.array ? 2 : 0) : util::width(r.array->first().name) + 20);
        if (project_) user_w = std::max(user_w, util::width(r.job ? r.job->user : r.array->first().user));
    }
    cols.id = static_cast<int>(std::min<size_t>(id_w, 16));
    cols.user = project_ ? static_cast<int>(std::min<size_t>(user_w, 10)) : 0;
    const int ideal = ui::MARK_WIDTH + ui::STATE_WIDTH + cols.id + 1 + (cols.user ? cols.user + 1 : 0) +
                      static_cast<int>(std::min<size_t>(name_w, 40)) + 1 + ui::TIME_WIDTH + 2;
    const int list_w = side ? std::clamp(ideal, 34, std::max(34, W * 45 / 100)) : W;
    const int list_h = side ? body_h : std::clamp(static_cast<int>(list.size()) + 3, 5, std::max(5, body_h / 2));
    const int details_w = side ? W - list_w : W;
    const int details_h = side ? body_h : body_h - list_h;
    cols.width = list_w - 2;

    // Job list, scrolled to keep the selection in view
    list_page_ = std::max(1, list_h - 3);
    if (sel >= 0) {
        if (sel < list_offset_) list_offset_ = sel;
        if (sel >= list_offset_ + list_page_) list_offset_ = sel - list_page_ + 1;
    }
    list_offset_ = std::clamp(list_offset_, 0, std::max(0, static_cast<int>(list.size()) - list_page_));
    Elements list_rows = {ui::jobListHeader(cols)};
    if (list.empty()) {
        if (jobs_time_ == 0 && jobs_error_.empty())
            list_rows.push_back(text(" Reading squeue...") | dim);
        else
            list_rows.push_back(ui::emptyList(!filter_.empty(), jobs_time_ == 0 ? jobs_error_ : ""));
    } else {
        for (int i = list_offset_; i < std::min(static_cast<int>(list.size()), list_offset_ + list_page_); i++) {
            const Row& r = list[static_cast<size_t>(i)];
            const bool marked = marked_.count(r.key()) > 0;
            if (r.job)
                list_rows.push_back(ui::jobRow(*r.job, cols, liveUsed(*r.job), i == sel, now, marked, r.array ? (r.last ? "└ " : "├ ") : ""));
            else
                list_rows.push_back(ui::arrayRow(*r.array, cols, arrayTime(*r.array, now), open_arrays_.count(r.array->id) > 0, i == sel, marked));
        }
    }
    std::string list_title = project_ ? "Project" : "Jobs";
    if (!filter_.empty() || editing_filter_) list_title += " /" + filter_ + (editing_filter_ ? "_" : "");
    std::string list_right = list.size() > static_cast<size_t>(list_page_)
                                 ? std::to_string(sel + 1) + "/" + std::to_string(list.size())
                                 : std::to_string(list.size());
    if (sort_) list_right = std::string("by ") + sortName(sort_) + " · " + list_right;
    Element list_panel = ui::panel(util::cut(list_title, static_cast<size_t>(std::max(4, list_w - 16))),
                                   vbox(std::move(list_rows)) | flex, editing_filter_ ? ui::theme::yellow() : ui::theme::accent(),
                                   text(list_right)) |
                         size(WIDTH, EQUAL, list_w) | size(HEIGHT, EQUAL, list_h) | reflect(list_box_);

    // Details of the selected job, or array
    Elements info;
    std::string details_title = "Details";
    if (job) {
        details_title = "Job " + job->id;
        const auto err = details_error_.find(job->job_id);
        const auto live = live_.find(job->job_id);
        const auto rank = ranks_.find(job->job_id);
        info = ui::jobDetails(*job, detailsFor(*job), nodes_, liveUsed(*job), live == live_.end() ? nullptr : &live->second,
                              rank == ranks_.end() ? nullptr : &rank->second, details_w - 4,
                              err == details_error_.end() ? "" : err->second);
    } else if (array) {
        details_title = "Array " + array->id;
        info = ui::arrayDetails(*array, details_w - 4, array_past_error_);
    } else if (!jobs_error_.empty() && jobs_time_ == 0) {
        info = {paragraph(jobs_error_) | color(ui::theme::red())};
    } else if (jobs_time_ == 0) {
        info = {text("Reading squeue...") | dim};
    }
    /* The details scroll by lines (J/K, wheel): node cells are several lines high, a frame shows the right ones
    - FTXUI centers the frame on the focus: aiming at the middle of the window puts `details_offset_` at its top
    */
    details_page_ = std::max(1, details_h - 2);
    auto height = [](const Elements& lines) {
        Element e = vbox(lines);
        e->ComputeRequirement();
        return e->requirement().min_y;
    };

    // The end of its output, in the room left
    if (job) {
        const auto p = previews_.find(job->job_id);
        const int room = details_page_ - height(info) - 2;
        if (p != previews_.end() && p->second.error.empty() && room >= 3) {
            std::vector<std::string> last = p->second.lines;
            while (!last.empty() && util::trim(last.back()).empty()) last.pop_back();
            if (!last.empty()) {
                info.push_back(text(""));
                info.push_back(hbox({text("Output") | bold, text("  last lines · l for the whole log") | dim}));
                const size_t n = std::min(last.size(), static_cast<size_t>(room));
                for (size_t i = last.size() - n; i < last.size(); i++)
                    info.push_back(text(util::cut(last[i], static_cast<size_t>(std::max(1, details_w - 4)))) |
                                   color(ui::isErrorLine(last[i]) ? ui::theme::red() : ui::theme::gray()));
            }
        }
    }
    const int details_total = height(info);
    details_offset_ = std::clamp(details_offset_, 0, std::max(0, details_total - details_page_));
    Element details_content = vbox(std::move(info));
    if (details_total > details_page_) {
        // A blank line above and below: the frames of cut node cells would merge with the panel's (┴)
        const int page = std::max(1, details_page_ - 2);
        details_offset_ = std::clamp(details_offset_, 0, std::max(0, details_total - page));
        details_content = vbox({text(""), details_content | focusPosition(0, details_offset_ + (page - 1) / 2) |
                                              vscroll_indicator | yframe | flex,
                                text("")});
    }
    Element details_panel = ui::panel(details_title, details_content | flex, ui::theme::gray()) |
                            size(WIDTH, EQUAL, details_w) | size(HEIGHT, EQUAL, details_h) | reflect(details_box_);

    Element body = side ? hbox({list_panel, details_panel}) : vbox({list_panel, details_panel});
    const std::string slow = interval_ > opts_.refresh ? "Slurm slow: every " + std::to_string(interval_) + " s" : "";
    Element base = vbox({
        ui::title(project_ ? who_ + " · project " + accountList() : who_, jobs_, jobs_time_, refreshing_, jobs_error_, W, slow),
        body,
        ui::footer(W, editing_filter_ ? "Filter: type, Enter to keep it, Esc to clear it" : message_, message_error_,
                   project_ ? 0 : static_cast<int>(marked_.size())),
    });

    // Overlays
    Element over;
    const int ow = std::min(W - 2, 140), oh = H - 2;   // the margin takes the 2 others
    switch (view_) {
        case View::Help: over = ui::helpView(W - 2, oh, opts_.refresh, help_offset_); break;
        case View::Logs: over = logs_.render(ow, oh); break;
        case View::Debug: over = debug_.render(std::min(W - 2, 120), oh); break;
        case View::Script: over = script_.render(std::min(W - 2, 120), oh); break;
        case View::History: over = history_.render(std::min(W - 2, 130), oh); break;
        case View::Partitions: over = cluster_.render(std::min(W - 2, 140), oh, myNodes()); break;
        case View::Quota: over = quota_.render(W - 2, oh, project_ ? ownJobs() : jobs_); break;
        case View::Gpu: over = gpu_.render(W - 2, oh); break;
        case View::Monitor: over = monitor_.render(W - 2, oh); break;
        case View::Plan: over = plan_.render(W - 2); break;
        case View::Confirm: over = confirmView(); break;
        case View::None: break;
    }
    if (!over) return base;
    // A blank margin around the view: its frame would otherwise merge with the frames behind it (┴, ╭╭)
    return dbox({base, over | borderEmpty | clear_under | center});
}

// What c or H is about to do, before y: one job as before, several as a list
Element Viewer::confirmView() const {
    const auto& t = action_.targets;
    const bool cancel = action_.kind == Action::Cancel;
    const std::string verb = cancel ? "Cancel" : action_.kind == Action::Hold ? "Hold" : "Release";
    Elements lines;
    if (t.size() == 1) {
        lines.push_back(hbox({text(verb + " " + t[0].what + " "), text(t[0].what == "array" ? api::arrayOf(t[0].shown) : t[0].shown) | bold, text(" ?")}));
        lines.push_back(text(util::cut(t[0].name, 60)) | dim);
        lines.push_back(hbox({text(t[0].state) | color(ui::stateColor(t[0].state)), text(t[0].about.empty() ? "" : "  " + t[0].about) | dim}));
    } else {
        const auto arrays = std::count_if(t.begin(), t.end(), [](const Action::Target& x) { return x.what == "array"; });
        const auto jobs = static_cast<long>(t.size()) - arrays;
        auto some = [](long n, const std::string& what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); };
        const std::string what = arrays && jobs ? some(arrays, "array") + " and " + some(jobs, "job") : arrays ? some(arrays, "array") : some(jobs, "job");
        lines.push_back(text(verb + " " + what + " ?") | bold);
        lines.push_back(text(""));
        size_t id_w = 5;
        for (const auto& x : t) id_w = std::max(id_w, util::width(x.shown));
        for (size_t i = 0; i < t.size() && i < 12; i++)
            lines.push_back(hbox({text(util::padRight(api::shortState(t[i].state), 3)) | color(ui::stateColor(t[i].state)) | bold,
                                  text(util::padRight(t[i].shown, id_w) + "  "), text(util::padRight(t[i].name, 24) + "  "),
                                  text(t[i].about) | dim}));
        if (t.size() > 12) lines.push_back(text("and " + std::to_string(t.size() - 12) + " more") | dim);
    }
    if (action_.left_out > 0)
        lines.push_back(text(std::to_string(action_.left_out) + " marked left out: " +
                             (cancel ? "already ended" : action_.kind == Action::Hold ? "not waiting, or held already" : "not held")) |
                        dim);
    lines.push_back(text(""));
    const std::string them = t.size() == 1 ? "it" : "them";
    lines.push_back(hbox({text("y") | bold | color(ui::theme::red()), text(" " + util::lower(verb) + " " + them + "    "), text("n") | bold,
                          text(" / Esc " + std::string(cancel ? "keep " : "leave ") + them) | dim}));
    const std::string command = cancel ? "scancel" : action_.kind == Action::Hold ? "scontrol hold" : "scontrol release";
    return ui::panel(command, vbox(std::move(lines)) | size(WIDTH, GREATER_THAN, 40), cancel ? ui::theme::red() : ui::theme::yellow());
}

bool Viewer::onEvent(Event e) {
    if (e == Event::Custom) return false;
    if (e == Event::CtrlC) {   // like q, from any view (FTXUI would turn it into SIGINT, see main())
        screen_.Exit();
        return true;
    }
    if (e.is_mouse() && e.mouse().motion == Mouse::Moved) return true;
    if (view_ != View::None) return viewEvent(e);
    if (editing_filter_) return filterEvent(e);
    return mainEvent(e);
}

bool Viewer::filterEvent(Event& e) {
    if (e == Event::Escape) {
        filter_.clear();
        editing_filter_ = false;
    } else if (e == Event::Return || e == Event::ArrowDown || e == Event::ArrowUp || e == Event::Tab) {
        editing_filter_ = false;
    } else if (e == Event::Backspace) {
        if (!filter_.empty()) {
            size_t i = filter_.size() - 1;
            while (i > 0 && (static_cast<unsigned char>(filter_[i]) & 0xC0) == 0x80) i--;   // a whole UTF-8 character
            filter_.erase(i);
        }
    } else if (e.is_character()) {
        if (filter_.size() < 64) filter_ += e.character();
    } else {
        return e.is_mouse();
    }
    selected_pos_ = 0;
    list_offset_ = 0;
    return true;
}

bool Viewer::viewEvent(Event& e) {
    bool close = false;
    bool handled = true;
    switch (view_) {
        case View::Help:
            // The arrows scroll it on a short screen, any other key closes it
            if (!ui::scrollKey(e, help_offset_, Terminal::Size().dimy - 4)) close = !e.is_mouse();
            break;
        case View::Logs: handled = logs_.onEvent(e, close); break;
        case View::Debug: handled = debug_.onEvent(e, close); break;
        case View::History: handled = history_.onEvent(e, close); break;
        case View::Partitions: handled = cluster_.onEvent(e, close); break;
        case View::Quota: handled = quota_.onEvent(e, close); break;
        case View::Gpu: handled = gpu_.onEvent(e, close); break;
        case View::Monitor: handled = monitor_.onEvent(e, close); break;
        case View::Plan: handled = plan_.onEvent(e, close); break;
        case View::Script: handled = script_.onEvent(e, close); break;
        case View::Confirm:
            if (e == Event::Character('y') || e == Event::Character('Y')) {
                runAction();
                return true;
            }
            close = !e.is_mouse();
            break;
        case View::None: return false;
    }
    if (close) {
        if (view_ == View::Confirm) showMessage(action_.kind == Action::Cancel ? "Not cancelled" : "Nothing changed");
        if (view_ == View::Monitor) closeMonitor();
        view_ = View::None;
    }
    (void)handled;
    return true;   // a view takes every key: nothing reaches the list behind it
}

bool Viewer::mainEvent(Event& e) {
    const auto list = rows();
    const int sel = selectedIndex(list);
    const Row* row = sel >= 0 ? &list[static_cast<size_t>(sel)] : nullptr;
    const api::Job* job = row ? row->job : nullptr;
    const ui::JobArray* array = row && !row->job ? row->array : nullptr;

    if (e.is_mouse()) {
        Mouse& m = e.mouse();
        const bool wheel_down = m.button == Mouse::WheelDown, wheel_up = m.button == Mouse::WheelUp;
        if (details_box_.Contain(m.x, m.y) && (wheel_down || wheel_up)) {
            details_offset_ = std::max(0, details_offset_ + (wheel_down ? 3 : -3));
        } else if (list_box_.Contain(m.x, m.y)) {
            if (wheel_down || wheel_up) select(list, sel + (wheel_down ? 1 : -1));
            else if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
                const int r = m.y - list_box_.y_min - 2;   // border and header
                if (r >= 0 && r < list_page_) select(list, list_offset_ + r);
            }
        }
        return true;
    }

    if (e == Event::Character('q') || e == Event::Escape) {
        if (e == Event::Escape && !marked_.empty()) {
            marked_.clear();
            return true;
        }
        if (e == Event::Escape && !filter_.empty()) {
            filter_.clear();
            return true;
        }
        screen_.Exit();
        return true;
    }
    const bool writes = e == Event::Character(' ') || e == Event::Character('c') || e == Event::Delete || e == Event::Character('H');
    if (e == Event::ArrowDown || e == Event::Character('j')) select(list, sel + 1);
    else if (e == Event::ArrowUp || e == Event::Character('k')) select(list, sel - 1);
    else if (e == Event::PageDown || e == Event::CtrlD) select(list, sel + std::max(1, list_page_ - 1));
    else if (e == Event::PageUp || e == Event::CtrlU) select(list, sel - std::max(1, list_page_ - 1));
    else if (e == Event::Home || e == Event::Character('g')) select(list, 0);
    else if (e == Event::End || e == Event::Character('G')) select(list, static_cast<int>(list.size()) - 1);
    else if (e == Event::Character('J')) details_offset_ += 3;
    else if (e == Event::Character('K')) details_offset_ = std::max(0, details_offset_ - 3);
    else if (e == Event::Character('/')) editing_filter_ = true;
    else if (e == Event::Character('s')) {
        sort_ = (sort_ + 1) % 4;
        showMessage(std::string("Sorted by ") + sortName(sort_));
    } else if (e == Event::Character('r')) {
        refresh();
        requestDetails(true);
    } else if (e == Event::Character('?') || e == Event::Character('h')) {
        view_ = View::Help;
        help_offset_ = 0;
    }
    else if (e == Event::Character('a')) {
        view_ = View::History;
        // sacct can take seconds: what was read less than a minute ago is shown as it is (r reads again)
        if (history_.jobs.empty() || !history_.error.empty() || Clock::now() - history_time_ > std::chrono::minutes(1))
            loadHistory();
    } else if (e == Event::Character('p')) {
        view_ = View::Partitions;
        cluster_.offset = 0;
        cluster_.opened();
        loadCluster(true);
    } else if (e == Event::Character('u')) {
        view_ = View::Quota;
        quota_.offset = 0;
        loadQuota();
    } else if (e == Event::Character('t')) {
        openPlan();
    } else if (e == Event::Character('o')) {
        toggleProject();
    } else if (!row) {
        return e.is_character();
    } else if (project_ && (writes || e == Event::Character('n') || e == Event::Character('m'))) {
        showMessage("The project view is read-only: o for your own jobs", true);
    } else if (e == Event::Character(' ')) {
        // Marks the row (or unmarks it) and goes to the next one: several in a row are marked quickly
        if (!marked_.erase(row->key())) marked_.insert(row->key());
        select(list, sel + 1);
    } else if (e == Event::Character('c') || e == Event::Delete) {
        openAction(Action::Cancel, row);
    } else if (e == Event::Character('H')) {
        openAction(Action::Hold, row);
    } else if (e == Event::Character('b')) {
        openScript(job ? *job : array->first());
    } else if (array) {
        if (e == Event::Return || e == Event::ArrowRight) {
            if (!open_arrays_.erase(array->id)) open_arrays_.insert(array->id);
        } else if (e == Event::ArrowLeft) {
            open_arrays_.erase(array->id);
        } else if (e == Event::Character('y')) {
            copy(array->id, "array id");
        } else if (e == Event::Character('l') || e == Event::Character('d') || e == Event::Character('n') || e == Event::Character('m') ||
                   e == Event::Character('Y')) {
            showMessage("Enter shows the tasks of the array: each has its log and details");
        } else {
            return false;
        }
    } else if (e == Event::ArrowLeft && row->array) {
        // From a task back to its array, folded
        open_arrays_.erase(row->array->id);
        selected_id_ = row->array->shownId();
        details_offset_ = 0;
    } else if (e == Event::Character('l') || e == Event::Return) openLogs(*job);
    else if (e == Event::Character('d')) openDebug(*job);
    else if (e == Event::Character('y')) copy(job->id, "job id");
    else if (e == Event::Character('Y')) {
        const auto [out, err] = ui::logPaths(*job, detailsFor(*job));
        if (out.empty()) showMessage("No output file known for job " + job->id, true);
        else copy(out, "path");
    } else if (e == Event::Character('n')) {
        if (!job->active() || job->gpus == 0) showMessage("n shows the GPUs of a running job that has some", true);
        else openGpus(*job);
    } else if (e == Event::Character('m')) {
        if (!job->active()) showMessage("m shows a running job live: its CPUs, memory and GPUs", true);
        else openMonitor(*job);
    } else {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    int exit_code = 0;
    const auto opts = parseOptions(argc, argv, exit_code);
    if (!opts) return exit_code;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::cerr << "rsv: needs a terminal (stdin and stdout)\n";
        return 1;
    }

    // Set now, and again once FTXUI has made its own choice when its loop starts (see the renderer below)
    const auto colors = chooseColors(opts->color);
    if (colors) Terminal::SetColorSupport(*colors);

    /* Signals
    - FTXUI ends its loop on SIGINT, SIGTERM, SIGHUP and SIGQUIT, then raises the signal again with the handler that
      was there before it. With the default one RSV died inside the loop, and the Slurm command running then (in its
      own process group) was left behind. With this one, the loop returns, the workers stop their commands, then RSV
      ends by the same signal.
    - An ignored SIGCHLD is inherited through exec: children would be reaped by the system, and every command would
      look successful
    */
    std::signal(SIGCHLD, SIG_DFL);
    struct sigaction on_quit {};
    on_quit.sa_handler = recordQuitSignal;
    sigemptyset(&on_quit.sa_mask);
    for (int s : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaction(s, &on_quit, nullptr);

    auto screen = ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);   // Ctrl-C is a key that quits (Viewer::onEvent), not a signal
    if (!opts->mouse) screen.TrackMouse(false);

    Viewer app(screen, *opts);

    auto root = Renderer([&] {
        if (colors && Terminal::ColorSupport() != *colors) Terminal::SetColorSupport(*colors);
        // Mouse: clicks and wheel only. Reporting every move redraws the whole screen for each one, over SSH.
        if (opts->mouse) std::cout << "\033[?1003l" << std::flush;
        return app.render();
    });
    root = CatchEvent(root, [&](Event e) { return app.onEvent(e); });

    /* Timer and hangup watchdog
    - Every 250 ms: refresh when it is time, follow the log, redraw the clocks every 5 s
    - When the SSH connection drops, FTXUI keeps its loop running on a terminal that is gone and the program stays
      on the login node. The hangup shows on stdin: the loop is then asked to stop, like with q.
    */
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    bool running = true;
    std::thread timer([&] {
        auto last_redraw = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(timer_mutex);
        while (running) {
            // POLLIN asked: macOS only reports POLLHUP with it (Linux does in any case)
            pollfd input{STDIN_FILENO, POLLIN, 0};
            if (quit_signal || (poll(&input, 1, 0) > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL)))) {
                api::stopping() = true;
                screen.Exit();
                return;
            }
            screen.Post([&app] { app.tick(); });
            const auto now = std::chrono::steady_clock::now();
            if (now - last_redraw >= std::chrono::seconds(5)) {
                screen.PostEvent(Event::Custom);
                last_redraw = now;
            }
            timer_cv.wait_for(lock, std::chrono::milliseconds(250), [&] { return !running; });
        }
    });

    app.start();
    screen.Loop(root);

    {
        std::lock_guard<std::mutex> lock(timer_mutex);
        running = false;
    }
    timer_cv.notify_all();
    timer.join();
    app.stop();

    if (quit_signal) {
        std::signal(quit_signal, SIG_DFL);
        std::raise(quit_signal);
    }
    return 0;
}
