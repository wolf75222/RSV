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

#include "api/cluster.hpp"
#include "api/slurmjobs.hpp"
#include "api/usage.hpp"
#include "components/cluster_view.hpp"
#include "components/debug_view.hpp"
#include "components/footer.hpp"
#include "components/gpu_view.hpp"
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

constexpr const char* VERSION = "2.0.0";

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
        cluster_.refresh_seconds = opts_.refresh;
        quota_.reload = [this] { loadQuota(); };
        quota_.user = user_;
    }

    void start() { refresh(); }

    // Stops the workers (and the commands they run) before the rest of the Viewer goes away
    void stop() {
        api::stopping() = true;
        slurm_.stop();
        files_.stop();
    }

    // Called every 250 ms by the timer thread, on the UI thread
    void tick() {
        const auto now = Clock::now();
        if (!refreshing_ && now - last_refresh_ >= std::chrono::seconds(opts_.refresh)) refresh();
        if (view_ == View::Logs && logs_.follow && !log_loading_ && now - last_log_read_ >= std::chrono::seconds(2)) loadLog();
        if (view_ == View::Partitions && !cluster_.loading && now - cluster_time_ >= std::chrono::seconds(opts_.refresh))
            loadCluster(false);
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
    enum class View { None, Help, Logs, Debug, History, Partitions, Quota, Cancel, Gpu, Plan };

    // ---------------------------------------------------------------- threads

    // From a worker: runs `fn` on the UI thread, then draws
    void post(std::function<void()> fn) {
        screen_.Post(std::move(fn));
        screen_.PostEvent(Event::Custom);
    }

    void refresh() {
        if (refreshing_) return;
        refreshing_ = true;
        last_refresh_ = Clock::now();
        slurm_.post([this, user = user_] {
            api::Fetched<std::vector<api::Job>> r;
            try {
                r = api::fetchJobs(user);
                if (!r.error.empty()) {
                    const std::string controller = api::controllerProblem();
                    if (!controller.empty()) r.error += " · " + controller;
                }
            } catch (const std::exception& e) {
                r.error = std::string("squeue: ") + e.what();
            }
            post([this, r = std::move(r)]() mutable { onJobs(std::move(r)); });
        });
    }

    void onJobs(api::Fetched<std::vector<api::Job>> r) {
        refreshing_ = false;
        last_refresh_ = Clock::now();
        if (!r.error.empty()) {
            jobs_error_ = r.error;   // the last list stays on screen
            return;
        }
        jobs_error_.clear();
        jobs_ = std::move(r.data);
        jobs_time_ = std::time(nullptr);
        jobs_clock_ = Clock::now();

        // Forget the details of jobs that left the queue
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
        requestDetails(false);
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
        if (!force && when != details_time_.end() && Clock::now() - when->second < std::chrono::seconds(opts_.refresh - 1)) return;
        if (!force && details_pending_ == id) return;
        details_pending_ = id;

        const uint64_t generation = ++details_generation_;
        const bool active = j->active();
        const bool pending = j->pending();
        const std::string shown_id = j->id;
        slurm_.post([this, id, shown_id, generation, active, pending] {
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
            post([this, id, d = std::move(d), n = std::move(n), live = std::move(live), rank]() mutable {
                if (live.error.empty() && live.data.steps > 0) live_[id] = live.data;
                else live_.erase(id);
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
                if (r.error.empty()) {
                    cluster_.data = std::move(r.data);
                    cluster_.updated = std::time(nullptr);
                }
                if (opening) cluster_.extra = std::move(extra.data);
            });
        });
    }

    // The nodes the user's jobs run on, marked in the cluster view
    std::set<std::string> myNodes() const {
        std::set<std::string> out;
        for (const auto& j : jobs_)
            if (j.active())
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
                for (const auto& a : api::fetchAssocs(user).data)
                    if (a.max_submit != 0 && a.max_jobs != 0 &&
                        std::find(accounts.begin(), accounts.end(), a.account) == accounts.end())
                        accounts.push_back(a.account);
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

    void cancelSelected() {
        // As squeue shows it: 812402_[3-40] cancels only these pending tasks. A throttle (812402_[3-40%4]) isn't an id.
        std::string id = cancel_.id;
        const size_t percent = id.find('%');
        const size_t bracket = id.find(']', percent == std::string::npos ? 0 : percent);
        if (percent != std::string::npos && bracket != std::string::npos) id.erase(percent, bracket - percent);
        view_ = View::None;
        showMessage("Cancelling job " + id + "...");
        slurm_.post([this, id] {
            std::string error;
            try {
                error = api::cancelJob(id);
            } catch (const std::exception& e) {
                error = std::string("scancel: ") + e.what();
            }
            post([this, id, error] {
                if (error.empty()) showMessage("Job " + id + " cancelled");
                else showMessage(error, true);
                refreshing_ = false;
                refresh();
            });
        });
    }

    // ---------------------------------------------------------------- list

    // The jobs shown: filtered, then sorted
    std::vector<const api::Job*> rows() const {
        std::vector<const api::Job*> out;
        const std::string f = util::lower(filter_);
        for (const auto& j : jobs_) {
            if (!f.empty()) {
                const std::string hay = util::lower(j.id + " " + j.name + " " + j.state + " " + api::shortState(j.state) +
                                                    " " + j.partition + " " + j.account + " " + j.reason);
                if (hay.find(f) == std::string::npos) continue;
            }
            out.push_back(&j);
        }

        auto group = [](const api::Job* j) { return j->active() ? 0 : j->pending() ? 1 : 2; };
        auto number = [](const api::Job* j) { return util::toLong(j->id, 0); };
        auto left = [this](const api::Job* j) {
            if (j->active() && j->limit > 0) return j->limit - liveUsed(*j);
            if (j->pending() && j->start > 0) return static_cast<long>(j->start - std::time(nullptr)) + (1L << 40);
            return 1L << 50;
        };
        std::stable_sort(out.begin(), out.end(), [&](const api::Job* a, const api::Job* b) {
            switch (sort_) {
                case 1: return number(a) != number(b) ? number(a) > number(b) : a->id > b->id;
                case 2: return util::lower(a->name) < util::lower(b->name);
                case 3: return left(a) < left(b);
                default:
                    if (group(a) != group(b)) return group(a) < group(b);
                    return number(a) != number(b) ? number(a) < number(b) : a->id < b->id;
            }
        });
        return out;
    }

    static const char* sortName(int s) {
        static const char* names[] = {"state", "newest", "name", "time left"};
        return names[s % 4];
    }

    const api::Job* selectedJob() const {
        for (const auto& j : jobs_)
            if (j.id == selected_id_) return &j;
        return nullptr;
    }

    // The selection follows the job (its id) through refreshes and sorts; when it leaves, the row below takes it
    int selectedIndex(const std::vector<const api::Job*>& list) {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i]->id == selected_id_) return selected_pos_ = static_cast<int>(i);
        if (list.empty()) return -1;
        selected_pos_ = std::clamp(selected_pos_, 0, static_cast<int>(list.size()) - 1);
        selected_id_ = list[static_cast<size_t>(selected_pos_)]->id;
        details_offset_ = 0;
        requestDetails(false);
        return selected_pos_;
    }

    void select(const std::vector<const api::Job*>& list, int index) {
        if (list.empty()) return;
        index = std::clamp(index, 0, static_cast<int>(list.size()) - 1);
        if (list[static_cast<size_t>(index)]->id == selected_id_) return;
        selected_pos_ = index;
        selected_id_ = list[static_cast<size_t>(index)]->id;
        details_offset_ = 0;
        requestDetails(false);
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
    void copy(const std::string& s, const std::string& what) {
        std::cout << "\033]52;c;" << base64(s) << "\a" << std::flush;
        showMessage("Copied " + what + ": " + s);
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
    ui::PlanView plan_;
    api::Job cancel_;
    uint64_t log_generation_ = 0;
    bool log_loading_ = false;
    Clock::time_point last_log_read_ = Clock::now();

    std::string message_;
    bool message_error_ = false;
    Clock::time_point message_until_ = Clock::now();

    // Last: they stop (and join) first when the Viewer goes away, while the rest is still there
    Worker slurm_;
    Worker files_;
};

Element Viewer::render() {
    const Dimensions term = Terminal::Size();
    const int W = term.dimx, H = term.dimy;
    if (W < 40 || H < 10)
        return vbox({filler(), text("RSV: terminal too small") | bold | hcenter,
                     text(std::to_string(W) + "×" + std::to_string(H) + ", 40×10 at least") | dim | hcenter, filler()});

    const auto list = rows();
    const int sel = selectedIndex(list);
    const api::Job* job = sel >= 0 ? list[static_cast<size_t>(sel)] : nullptr;
    const std::time_t now = std::time(nullptr);

    /* Layout
    - Wide: the list on the left, as wide as its names need, between 34 columns and 45% of the screen
    - Narrow (< 90 columns): the list above, the details below
    */
    const bool side = W >= 90;
    const int body_h = H - 2;
    const int id_w = ui::idWidth(list);
    size_t name_w = 10;
    for (const auto* j : list) name_w = std::max(name_w, util::width(j->name));
    const int ideal = ui::STATE_WIDTH + id_w + 1 + static_cast<int>(std::min<size_t>(name_w, 36)) + 1 + ui::TIME_WIDTH + 2;
    const int list_w = side ? std::clamp(ideal, 34, std::max(34, W * 45 / 100)) : W;
    const int list_h = side ? body_h : std::clamp(static_cast<int>(list.size()) + 3, 5, std::max(5, body_h / 2));
    const int details_w = side ? W - list_w : W;
    const int details_h = side ? body_h : body_h - list_h;

    // Job list, scrolled to keep the selection in view
    list_page_ = std::max(1, list_h - 3);
    if (sel >= 0) {
        if (sel < list_offset_) list_offset_ = sel;
        if (sel >= list_offset_ + list_page_) list_offset_ = sel - list_page_ + 1;
    }
    list_offset_ = std::clamp(list_offset_, 0, std::max(0, static_cast<int>(list.size()) - list_page_));
    const int row_w = list_w - 2;
    Elements list_rows = {ui::jobListHeader(row_w, id_w)};
    if (list.empty()) {
        if (jobs_time_ == 0 && jobs_error_.empty())
            list_rows.push_back(text(" Reading squeue...") | dim);
        else
            list_rows.push_back(ui::emptyList(!filter_.empty(), jobs_time_ == 0 ? jobs_error_ : ""));
    } else {
        for (int i = list_offset_; i < std::min(static_cast<int>(list.size()), list_offset_ + list_page_); i++) {
            const auto* j = list[static_cast<size_t>(i)];
            list_rows.push_back(ui::jobRow(*j, row_w, id_w, liveUsed(*j), i == sel, now));
        }
    }
    std::string list_title = "Jobs";
    if (!filter_.empty() || editing_filter_) list_title += " /" + filter_ + (editing_filter_ ? "_" : "");
    std::string list_right = list.size() > static_cast<size_t>(list_page_)
                                 ? std::to_string(sel + 1) + "/" + std::to_string(list.size())
                                 : std::to_string(list.size());
    if (sort_) list_right = std::string("by ") + sortName(sort_) + " · " + list_right;
    Element list_panel = ui::panel(util::cut(list_title, static_cast<size_t>(std::max(4, list_w - 16))),
                                   vbox(std::move(list_rows)) | flex, editing_filter_ ? ui::theme::yellow() : ui::theme::accent(),
                                   text(list_right)) |
                         size(WIDTH, EQUAL, list_w) | size(HEIGHT, EQUAL, list_h) | reflect(list_box_);

    // Details of the selected job
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
                    info.push_back(text(util::cut(last[i], static_cast<size_t>(std::max(1, details_w - 4)))) | color(ui::theme::gray()));
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
    Element base = vbox({
        ui::title(who_, jobs_, jobs_time_, refreshing_, jobs_error_, W),
        body,
        ui::footer(W, editing_filter_ ? "Filter: type, Enter to keep it, Esc to clear it" : message_, message_error_),
    });

    // Overlays
    Element over;
    const int ow = std::min(W - 2, 140), oh = H - 2;   // the margin takes the 2 others
    switch (view_) {
        case View::Help: over = ui::helpView(W - 2, opts_.refresh); break;
        case View::Logs: over = logs_.render(ow, oh); break;
        case View::Debug: over = debug_.render(std::min(W - 2, 120), oh); break;
        case View::History: over = history_.render(std::min(W - 2, 130), oh); break;
        case View::Partitions: over = cluster_.render(std::min(W - 2, 140), oh, myNodes()); break;
        case View::Quota: over = quota_.render(W - 2, jobs_); break;
        case View::Gpu: over = gpu_.render(W - 2); break;
        case View::Plan: over = plan_.render(W - 2); break;
        case View::Cancel: {
            Elements lines = {
                hbox({text("Cancel job "), text(cancel_.id) | bold, text(" ?")}),
                text(util::cut(cancel_.name, 60)) | dim,
                hbox({text(cancel_.state) | color(ui::stateColor(cancel_.state)),
                      text(cancel_.active() ? " on " + std::to_string(cancel_.nodes) + (cancel_.nodes == 1 ? " node" : " nodes") + ", for " + util::duration(liveUsed(cancel_))
                                            : "")}),
                text(""),
                hbox({text("y") | bold | color(ui::theme::red()), text(" cancel it    "), text("n") | bold, text(" / Esc keep it") | dim}),
            };
            if (cancel_.id.find("_[") != std::string::npos)
                lines.insert(lines.begin() + 2, text("(only these pending tasks of the array)") | dim);
            over = ui::panel("scancel", vbox(std::move(lines)) | size(WIDTH, GREATER_THAN, 40), ui::theme::red());
            break;
        }
        case View::None: break;
    }
    if (!over) return base;
    // A blank margin around the view: its frame would otherwise merge with the frames behind it (┴, ╭╭)
    return dbox({base, over | borderEmpty | clear_under | center});
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
            close = !e.is_mouse();
            break;
        case View::Logs: handled = logs_.onEvent(e, close); break;
        case View::Debug: handled = debug_.onEvent(e, close); break;
        case View::History: handled = history_.onEvent(e, close); break;
        case View::Partitions: handled = cluster_.onEvent(e, close); break;
        case View::Quota: handled = quota_.onEvent(e, close); break;
        case View::Gpu: handled = gpu_.onEvent(e, close); break;
        case View::Plan: handled = plan_.onEvent(e, close); break;
        case View::Cancel:
            if (e == Event::Character('y') || e == Event::Character('Y')) {
                cancelSelected();
                return true;
            }
            close = !e.is_mouse();
            break;
        case View::None: return false;
    }
    if (close) {
        if (view_ == View::Cancel) showMessage("Not cancelled");
        view_ = View::None;
    }
    (void)handled;
    return true;   // a view takes every key: nothing reaches the list behind it
}

bool Viewer::mainEvent(Event& e) {
    const auto list = rows();
    const int sel = selectedIndex(list);
    const api::Job* job = sel >= 0 ? list[static_cast<size_t>(sel)] : nullptr;

    if (e.is_mouse()) {
        Mouse& m = e.mouse();
        const bool wheel_down = m.button == Mouse::WheelDown, wheel_up = m.button == Mouse::WheelUp;
        if (details_box_.Contain(m.x, m.y) && (wheel_down || wheel_up)) {
            details_offset_ = std::max(0, details_offset_ + (wheel_down ? 3 : -3));
        } else if (list_box_.Contain(m.x, m.y)) {
            if (wheel_down || wheel_up) select(list, sel + (wheel_down ? 1 : -1));
            else if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
                const int row = m.y - list_box_.y_min - 2;   // border and header
                if (row >= 0 && row < list_page_) select(list, list_offset_ + row);
            }
        }
        return true;
    }

    if (e == Event::Character('q') || e == Event::Escape) {
        if (e == Event::Escape && !filter_.empty()) {
            filter_.clear();
            return true;
        }
        screen_.Exit();
        return true;
    }
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
    } else if (e == Event::Character('?') || e == Event::Character('h')) view_ = View::Help;
    else if (e == Event::Character('a')) {
        view_ = View::History;
        // sacct can take seconds: what was read less than a minute ago is shown as it is (r reads again)
        if (history_.jobs.empty() || !history_.error.empty() || Clock::now() - history_time_ > std::chrono::minutes(1))
            loadHistory();
    } else if (e == Event::Character('p')) {
        view_ = View::Partitions;
        cluster_.offset = 0;
        loadCluster(true);
    } else if (e == Event::Character('u')) {
        view_ = View::Quota;
        loadQuota();
    } else if (e == Event::Character('t')) {
        openPlan();
    } else if (!job) {
        return e.is_character();
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
    } else if (e == Event::Character('c') || e == Event::Delete) {
        if (job->finished()) {
            showMessage("Job " + job->id + " has already ended", true);
        } else {
            cancel_ = *job;
            view_ = View::Cancel;
        }
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
