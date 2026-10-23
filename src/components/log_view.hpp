#pragma once

#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "jobdetails.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

struct LogData {
    std::string path;
    std::vector<std::string> lines;   // ready to draw (see util::sanitize)
    uintmax_t size = 0;
    std::time_t mtime = 0;
    bool cut = false;                 // only the end of a big file was read
    std::string error;
};

/* The end of a log file: its last `max_bytes` at most (2 MiB, tens of thousands of lines)
- A log of several GB is never read whole, the partial first line is dropped
- Runs on a worker thread: a slow file system can't freeze the UI
*/
inline LogData readLogTail(const std::string& path, size_t max_bytes = 2u << 20) {
    LogData d;
    d.path = path;
    if (path.empty()) {
        d.error = "Slurm gives no file for this output";
        return d;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        d.error = errno == ENOENT ? "Not created yet (the job hasn't started, or writes elsewhere)" : std::strerror(errno);
        return d;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        d.error = "Not a regular file";
        close(fd);
        return d;
    }
    d.size = static_cast<uintmax_t>(st.st_size);
    d.mtime = st.st_mtime;

    const off_t start = st.st_size > static_cast<off_t>(max_bytes) ? st.st_size - static_cast<off_t>(max_bytes) : 0;
    std::string data;
    data.resize(static_cast<size_t>(st.st_size - start));
    size_t got = 0;
    while (got < data.size()) {
        const ssize_t n = pread(fd, &data[got], data.size() - got, start + static_cast<off_t>(got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    close(fd);
    data.resize(got);

    size_t from = 0;
    if (start > 0) {
        d.cut = true;
        const size_t nl = data.find('\n');
        from = nl == std::string::npos ? data.size() : nl + 1;
    }
    while (from < data.size()) {
        size_t end = data.find('\n', from);
        if (end == std::string::npos) end = data.size();
        d.lines.push_back(util::sanitize(data.substr(from, end - from)));
        from = end + 1;
    }
    return d;
}

/* Log viewer (l): the job's output and error files, followed like tail -f
- Following: the view stays at the end and the file is read again every 2 s. Scrolling up stops it, G or f again.
*/
struct LogView {
    std::string title;
    std::string paths[2];       // output, errors (empty when they are the same file)
    int which = 0;
    LogData data;
    bool loading = false;
    bool follow = true;
    int offset = 0;             // first line shown
    int hscroll = 0;            // first column shown
    int page = 20;              // lines shown at the last draw
    std::function<void()> reload;
    std::function<void(const std::string&)> copy;

    const std::string& path() const { return paths[which]; }

    void setData(LogData d) {
        const bool at_end = follow;
        data = std::move(d);
        loading = false;
        if (at_end) offset = maxOffset();
        offset = std::clamp(offset, 0, maxOffset());
    }

    int maxOffset() const { return std::max(0, static_cast<int>(data.lines.size()) - page); }

    void scroll(int delta) {
        offset = std::clamp(offset + delta, 0, maxOffset());
        if (delta < 0) follow = false;
    }

    // true when handled; `close` is set to close the view
    bool onEvent(Event e, bool& close) {
        if (e == Event::Escape || e == Event::Character('q') || e == Event::Character('l')) {
            close = true;
            return true;
        }
        if (e == Event::ArrowDown || e == Event::Character('j')) scroll(1);
        else if (e == Event::ArrowUp || e == Event::Character('k')) scroll(-1);
        else if (e == Event::PageDown || e == Event::Character(' ') || e == Event::CtrlD) scroll(std::max(1, page - 2));
        else if (e == Event::PageUp || e == Event::Character('b') || e == Event::CtrlU) scroll(-std::max(1, page - 2));
        else if (e == Event::Home || e == Event::Character('g')) scroll(-(1 << 30));
        else if (e == Event::End || e == Event::Character('G')) {
            follow = true;
            offset = maxOffset();
        } else if (e == Event::ArrowRight) hscroll += 8;
        else if (e == Event::ArrowLeft) hscroll = std::max(0, hscroll - 8);
        else if (e == Event::Character('f')) {
            follow = !follow;
            if (follow) offset = maxOffset();
        } else if (e == Event::Character('r')) {
            if (reload) reload();
        } else if (e == Event::Character('y')) {
            if (copy && !path().empty()) copy(path());
        } else if (e == Event::Tab || e == Event::TabReverse || e == Event::Character('e')) {
            if (!paths[1].empty()) {
                which = 1 - which;
                data = LogData{};
                offset = 0;
                hscroll = 0;
                follow = true;
                if (reload) reload();
            }
        } else if (e.is_mouse()) {
            if (e.mouse().button == Mouse::WheelDown) scroll(3);
            else if (e.mouse().button == Mouse::WheelUp) scroll(-3);
        } else {
            return false;
        }
        return true;
    }

    Element render(int width, int height) {
        const int inner = std::max(10, width - 4);
        page = std::max(1, height - 6);

        Elements tabs = {which == 0 ? text(" output ") | inverted | bold : text(" output ") | dim};
        if (!paths[1].empty()) tabs.push_back(which == 1 ? text(" errors ") | inverted | bold : text(" errors ") | dim);
        std::string info;
        if (!data.error.empty() || data.path.empty()) info = loading ? "reading..." : "";
        else info = util::bytes(data.size) + " · " + std::to_string(data.lines.size()) + " lines" + (data.cut ? " (end)" : "");
        tabs.push_back(text("  " + util::cutMiddle(shortPath(path()), static_cast<size_t>(std::max(10, inner - 40)))) | color(theme::cyan()));
        tabs.push_back(filler());
        tabs.push_back(text(info + "  ") | dim);
        tabs.push_back(follow ? text("following") | color(theme::green()) | bold : text("paused") | dim);

        Elements lines;
        if (!data.error.empty()) {
            lines.push_back(text(""));
            lines.push_back(text(data.error) | color(theme::yellow()) | hcenter);
        } else if (data.lines.empty()) {
            lines.push_back(text(""));
            lines.push_back(text(loading ? "Reading..." : "Empty file") | dim | hcenter);
        } else {
            const int digits = static_cast<int>(std::to_string(data.lines.size()).size());
            const int text_width = std::max(1, inner - digits - 2);
            const int first = std::clamp(offset, 0, maxOffset());
            for (int i = first; i < std::min(static_cast<int>(data.lines.size()), first + page); i++) {
                const std::string& l = data.lines[static_cast<size_t>(i)];
                std::string shown = hscroll > 0 ? util::dropStart(l, static_cast<size_t>(hscroll)) : l;
                lines.push_back(hbox({text(util::padLeft(std::to_string(i + 1), static_cast<size_t>(digits)) + " ") | dim,
                                      text(util::cut(shown, static_cast<size_t>(text_width)))}));
            }
            // A scroll bar needs every line: draw the window alone, with its position in the title instead
        }
        while (static_cast<int>(lines.size()) < page) lines.push_back(text(""));

        Elements hints = keyHints({{"j/k", "scroll"}, {"g/G", "top/end"}, {"f", follow ? "pause" : "follow"},
                                   {"←/→", "sideways"}, {"y", "copy path"}});
        if (!paths[1].empty()) hints.push_back(keyHint("Tab", "output/errors"));
        hints.push_back(keyHint("q", "close"));

        const int total = static_cast<int>(data.lines.size());
        const std::string where = total > page ? std::to_string(std::min(total, offset + page) * 100 / std::max(1, total)) + "%" : "";
        return panel(title, vbox({hbox(std::move(tabs)), separator() | dim, vbox(std::move(lines)) | flex,
                                  separator() | dim, hbox(std::move(hints))}) |
                                size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2),
                     theme::accent(), text(where));
    }
};

}  // namespace ui
