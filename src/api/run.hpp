#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace api {

struct RunResult {
    bool started = false;    // false: the program couldn't be started (not installed...)
    bool timed_out = false;
    int status = -1;         // exit status, 128 + n when killed by signal n
    std::string out;
    std::string err;

    bool ok() const { return started && !timed_out && status == 0; }

    // What went wrong, in one line: "squeue: timed out after 15 s", "scancel: error: ...", ""
    std::string error(const std::string& name) const {
        if (!started) return name + ": " + (err.empty() ? "cannot be run" : err);
        if (timed_out) return name + ": no answer (timed out)";
        if (status == 0) return "";
        std::string first = err.empty() ? out : err;
        first = first.substr(0, first.find('\n'));
        if (first.empty()) first = name + ": failed (exit status " + std::to_string(status) + ")";
        return first;
    }
};

// Set when the program quits: running commands are stopped at once instead of waiting for their answer
inline std::atomic<bool>& stopping() {
    static std::atomic<bool> flag{false};
    return flag;
}

namespace detail {

inline bool makePipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

// Waits for the process to end, at most `ms` milliseconds. True if it ended (and was reaped).
inline bool waitFor(pid_t pid, int& status, int ms) {
    for (int waited = 0;; waited += 10) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return true;
        if (waited >= ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace detail

/* Runs a program (found in PATH) and returns its output
- No shell: the arguments are passed as they are, so a job name or a path can't run anything
- stdin is /dev/null: a command that prompts (sacctmgr, scancel -i) can't take over the terminal of the UI
- Stopped after `timeout_seconds` (TERM, then KILL 2 s later), with all its children: a slurmctld that
  doesn't answer can't hang RSV. Also stopped at once when stopping() is set (the user quits).
*/
inline RunResult run(const std::vector<std::string>& args, int timeout_seconds = 15) {
    RunResult result;
    if (args.empty()) return result;

    int out_pipe[2], err_pipe[2];
    if (!detail::makePipe(out_pipe)) {
        result.err = std::strerror(errno);
        return result;
    }
    if (!detail::makePipe(err_pipe)) {
        result.err = std::strerror(errno);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);   // nothing else from RSV (the terminal) is inherited
#endif

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int s : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH, SIGCHLD})
        sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setpgroup(&attr, 0);   // its own process group: killed with its children
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;  // macOS: same as closefrom above
#endif
    posix_spawnattr_setflags(&attr, flags);

    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (rc != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        result.err = rc == ENOENT ? "command not found" : std::strerror(rc);
        return result;
    }
    result.started = true;

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 1000000);
    constexpr size_t MAX_OUTPUT = 64 << 20;

    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    std::string* sinks[2] = {&result.out, &result.err};
    bool stop = false;
    char buffer[65536];

    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        if (stopping() || clock::now() >= deadline || result.out.size() > MAX_OUTPUT) {
            result.timed_out = !stopping();
            stop = true;
            break;
        }
        const int n = poll(fds, 2, 100);
        if (n < 0 && errno != EINTR) break;
        for (int i = 0; i < 2 && n > 0; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t got = read(fds[i].fd, buffer, sizeof buffer);
            if (got > 0) {
                sinks[i]->append(buffer, static_cast<size_t>(got));
            } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
                close(fds[i].fd);
                fds[i].fd = -1;   // poll skips negative descriptors
            }
        }
    }
    for (auto& f : fds)
        if (f.fd >= 0) close(f.fd);

    int status = 0;
    bool ended = false;
    if (!stop) {
        // Output closed: the program is ending. It still gets the time left to do it.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        ended = detail::waitFor(pid, status, static_cast<int>(std::max<long long>(0, left)));
        if (!ended) result.timed_out = true;
    }
    if (!ended) {
        kill(-pid, SIGTERM);
        if (!detail::waitFor(pid, status, stopping() ? 200 : 2000)) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }

    if (WIFEXITED(status)) result.status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.status = 128 + WTERMSIG(status);
    return result;
}

}  // namespace api
