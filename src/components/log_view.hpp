#pragma once

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
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

// ---------------------------------------------------------------- error lines, search

namespace logtext {

inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool digit(char c) { return c >= '0' && c <= '9'; }

// `word` (lowercase ASCII) at `i` in `s`, in any case
inline bool at(const std::string& s, size_t i, const char* word) {
    for (size_t k = 0; word[k]; k++)
        if (i + k >= s.size() || lowerAscii(s[i + k]) != word[k]) return false;
    return true;
}

inline bool wordStart(const std::string& s, size_t i) { return i == 0 || !letter(s[i - 1]); }

inline bool wholeWord(const std::string& s, size_t i, size_t n) {
    return wordStart(s, i) && (i + n >= s.size() || !letter(s[i + n]));
}

/* "error" or "exception" at `i` (`n` letters), when it reports one
- A word (error:, ERROR, 3 errors) or the end of a name (RuntimeError, NullPointerException), not -Werror,
  stderr or a longer name (error_rate, errorbar, error-free, error.log)
- Not a count of none: "0 errors", "no error", "Errors: 0"
- Not a measure: "error=3.2e-05", "L2 error: 0.004", what numerical codes print at each step
*/
inline bool reportsError(const std::string& s, size_t i, size_t n) {
    const bool camel = i > 0 && s[i] >= 'A' && s[i] <= 'Z' && s[i - 1] >= 'a' && s[i - 1] <= 'z';
    if (!wordStart(s, i) && !camel) return false;

    size_t b = i;
    while (b > 0 && s[b - 1] == ' ') b--;
    if (b < i && b > 0) {
        if (s[b - 1] == '0' && (b == 1 || !digit(s[b - 2]))) return false;
        if (b >= 2 && at(s, b - 2, "no") && (b == 2 || !letter(s[b - 3]))) return false;
    }

    size_t a = i + n;
    if (a < s.size() && lowerAscii(s[a]) == 's') a++;
    if (a < s.size() && (letter(s[a]) || digit(s[a]) || s[a] == '_')) return false;
    if (a + 1 < s.size() && (s[a] == '-' || s[a] == '.') && letter(s[a + 1])) return false;
    while (a < s.size() && s[a] == ' ') a++;
    if (a < s.size() && (s[a] == ':' || s[a] == '=')) a++;
    while (a < s.size() && s[a] == ' ') a++;
    if (a + 1 < s.size() && (s[a] == '-' || s[a] == '+') && (digit(s[a + 1]) || s[a + 1] == '.')) a++;
    if (a >= s.size() || !(digit(s[a]) || (s[a] == '.' && a + 1 < s.size() && digit(s[a + 1])))) return true;
    bool real = false, zero = true;
    for (; a < s.size(); a++) {
        const char c = s[a];
        if (digit(c)) zero = zero && c == '0';
        else if (c == '.' || c == 'e' || c == 'E') real = true;
        else if (!((c == '-' || c == '+') && (s[a - 1] == 'e' || s[a - 1] == 'E'))) break;
    }
    return !real && !zero;
}

}  // namespace logtext

/* A line that looks like an error: drawn in red in the log, and at the end of the output under the details
- error and Exception (see reportsError), Traceback, Segmentation fault, SIGSEGV, OOM and out of memory, bad_alloc,
  core dumped, FATAL, abort (Aborted, MPI_Abort), Killed, and slurmstepd's CANCELLED AT and DUE TO TIME LIMIT
- One pass, no allocation: every line of a 2 MiB log goes through it at each reading
*/
inline bool isErrorLine(const std::string& s) {
    using namespace logtext;
    for (size_t i = 0; i < s.size(); i++) {
        switch (lowerAscii(s[i])) {
            case 'e':
                if ((at(s, i, "error") && reportsError(s, i, 5)) || (at(s, i, "exception") && reportsError(s, i, 9)))
                    return true;
                break;
            case 't':
                if (at(s, i, "traceback") && wordStart(s, i)) return true;
                break;
            case 's':
                if (at(s, i, "segmentation fault") || at(s, i, "segfault") || at(s, i, "sigsegv")) return true;
                break;
            case 'o':
                if ((at(s, i, "oom") && wholeWord(s, i, 3)) || at(s, i, "out of memory") || at(s, i, "out-of-memory"))
                    return true;
                break;
            case 'b':
                if (at(s, i, "bad_alloc")) return true;
                break;
            case 'c':
                if (at(s, i, "core dumped") || at(s, i, "cancelled at")) return true;
                break;
            case 'd':
                if (at(s, i, "due to time limit")) return true;
                break;
            case 'f':
                if (at(s, i, "fatal") && wordStart(s, i)) return true;
                break;
            case 'k':
                if (at(s, i, "killed") && wholeWord(s, i, 6)) return true;
                break;
            case 'a':
                if (at(s, i, "abort") && wordStart(s, i)) return true;
                break;
            default: break;
        }
    }
    return false;
}

// The lines that look like errors, in order
inline std::vector<int> errorLines(const std::vector<std::string>& lines) {
    std::vector<int> out;
    for (size_t i = 0; i < lines.size(); i++)
        if (isErrorLine(lines[i])) out.push_back(static_cast<int>(i));
    return out;
}

// Where `needle` (lowercase) is in `s` from `from`, ASCII letters in any case; npos when it isn't
inline size_t findNoCase(const std::string& s, const std::string& needle, size_t from = 0) {
    if (needle.empty() || needle.size() > s.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= s.size(); i++) {
        if (logtext::lowerAscii(s[i]) != needle[0]) continue;
        size_t k = 1;
        while (k < needle.size() && logtext::lowerAscii(s[i + k]) == needle[k]) k++;
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

// ---------------------------------------------------------------- reading

struct LogData {
    std::string path;
    std::vector<std::string> lines;   // ready to draw (see util::sanitize)
    std::vector<int> errors;          // the lines that look like errors (see isErrorLine)
    uintmax_t size = 0;
    std::time_t mtime = 0;
    bool cut = false;                 // only the end of a big file was read
    std::string error;
};

/* The end of a log file: its last `max_bytes` at most (2 MiB, tens of thousands of lines)
- A log of several GB is never read whole, the partial first line is dropped
- Runs on a worker thread: a slow file system can't freeze the UI (the error lines are found there too)
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
    d.errors = errorLines(d.lines);
    return d;
}

// ---------------------------------------------------------------- view

/* Log viewer (l): the job's output and error files, followed like tail -f
- Following: the view stays at the end and the file is read again every 2 s. Scrolling up stops it, G or f again.
- / searches (any case), n and N go to the next and previous match; e and E to the next and previous error line.
  A jump stops following, and puts the line a third of the way down, with what led to it above.
- Enter of an empty search searches again what was searched last
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

    bool prompting = false;     // / typed: the keys go to the search
    std::string typing;         // the search being typed
    std::string query;          // the search done, as typed
    std::vector<int> matches;   // the lines where it is
    int match_line = -1;        // the match jumped to last
    int error_line = -1;        // the error line jumped to last

    const std::string& path() const { return paths[which]; }

    void setData(LogData d) {
        const bool at_end = follow;
        data = std::move(d);
        loading = false;
        if (at_end) offset = maxOffset();
        offset = std::clamp(offset, 0, maxOffset());
        findMatches();
        const int n = static_cast<int>(data.lines.size());
        if (match_line >= n) match_line = -1;
        if (error_line >= n) error_line = -1;
    }

    int maxOffset() const { return std::max(0, static_cast<int>(data.lines.size()) - page); }

    void scroll(int delta) {
        offset = std::clamp(offset + delta, 0, maxOffset());
        if (delta < 0) follow = false;
    }

    void findMatches() {
        matches.clear();
        const std::string needle = util::lower(query);
        if (needle.empty()) return;
        for (size_t i = 0; i < data.lines.size(); i++)
            if (findNoCase(data.lines[i], needle) != std::string::npos) matches.push_back(static_cast<int>(i));
    }

    bool visible(int line) const { return line >= offset && line < offset + page; }

    // The line jumped to: a third of the way down, following stopped
    void show(int line) {
        follow = false;
        offset = std::clamp(line - page / 3, 0, maxOffset());
    }

    /* The next (or previous) line of `list` after `last`, around the end: from `last` while it is on screen, else
       from the top of the screen. -1 when the list is empty.
    */
    int step(const std::vector<int>& list, int last, bool forward) const {
        if (list.empty()) return -1;
        const int base = visible(last) ? last : forward ? offset - 1 : offset;
        if (forward) {
            const auto it = std::upper_bound(list.begin(), list.end(), base);
            return it == list.end() ? list.front() : *it;
        }
        const auto it = std::lower_bound(list.begin(), list.end(), base);
        return it == list.begin() ? list.back() : *std::prev(it);
    }

    void jumpMatch(bool forward) {
        match_line = step(matches, match_line, forward);
        if (match_line >= 0) show(match_line);
    }

    void jumpError(bool forward) {
        error_line = step(data.errors, error_line, forward);
        if (error_line >= 0) show(error_line);
    }

    // Enter: the nearest match, on the screen or below it, else the last one above it
    void search() {
        prompting = false;
        if (!typing.empty()) query = typing;
        typing.clear();
        findMatches();
        match_line = -1;
        if (matches.empty()) return;
        const auto it = std::lower_bound(matches.begin(), matches.end(), offset);
        match_line = it == matches.end() ? matches.back() : *it;
        show(match_line);
    }

    bool onSearchKey(const Event& e) {
        if (e == Event::Escape) {
            prompting = false;
            typing.clear();
        } else if (e == Event::Return) {
            search();
        } else if (e == Event::Backspace) {
            if (!typing.empty()) {
                size_t i = typing.size() - 1;
                while (i > 0 && (static_cast<unsigned char>(typing[i]) & 0xC0) == 0x80) i--;   // a whole UTF-8 character
                typing.erase(i);
            }
        } else if (e.is_character()) {
            if (typing.size() < 100) typing += e.character();
        }
        return true;   // nothing else while typing: q, l and the others are letters of the search
    }

    // true when handled; `close` is set to close the view
    bool onEvent(Event e, bool& close) {
        if (prompting) return onSearchKey(e);
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
        else if (e == Event::Character('/')) {
            prompting = true;
            typing.clear();
        } else if (e == Event::Character('n')) jumpMatch(true);
        else if (e == Event::Character('N')) jumpMatch(false);
        else if (e == Event::Character('e')) jumpError(true);
        else if (e == Event::Character('E')) jumpError(false);
        else if (e == Event::Character('f')) {
            follow = !follow;
            if (follow) offset = maxOffset();
        } else if (e == Event::Character('r')) {
            if (reload) reload();
        } else if (e == Event::Character('y')) {
            if (copy && !path().empty()) copy(path());
        } else if (e == Event::Tab || e == Event::TabReverse) {
            if (!paths[1].empty()) {
                which = 1 - which;
                data = LogData{};
                offset = 0;
                hscroll = 0;
                follow = true;
                matches.clear();
                match_line = error_line = -1;
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

    // The text of a line, the matches of the search on a yellow background
    static Element marked(const std::string& shown, const std::string& needle) {
        size_t at = findNoCase(shown, needle);
        if (at == std::string::npos) return text(shown);
        Elements parts;
        size_t from = 0;
        for (; at != std::string::npos; at = findNoCase(shown, needle, from)) {
            if (at > from) parts.push_back(text(shown.substr(from, at - from)));
            parts.push_back(text(shown.substr(at, needle.size())) | bgcolor(theme::yellow()) | color(Color::Black));
            from = at + needle.size();
        }
        if (from < shown.size()) parts.push_back(text(shown.substr(from)));
        return hbox(std::move(parts));
    }

    // The bottom line: the search being typed, or the keys that fit in `width`, the most useful first
    Element bottomLine(int width) const {
        if (prompting)
            return hbox({text("/") | bold | color(theme::accent()), text(typing), text("_") | dim, filler(),
                         text("Enter search  Esc cancel") | dim});

        struct Hint {
            Element e;
            int width, rank;   // the lower ranks stay when there isn't room for all
        };
        std::vector<Hint> hints;
        auto add = [&](const std::string& key, const std::string& label, int rank, Element shown = nullptr) {
            hints.push_back({shown ? shown : keyHint(key, label), static_cast<int>(util::width(key) + util::width(label)) + 3, rank});
        };
        add("j/k", "scroll", 2);
        add("g/G", "top/end", 4);
        if (query.empty()) {
            add("/", "search", 1);
        } else {
            const auto it = std::find(matches.begin(), matches.end(), match_line);
            const std::string where = matches.empty() ? "no match"
                                      : it == matches.end()
                                          ? std::to_string(matches.size()) + (matches.size() == 1 ? " match" : " matches")
                                          : std::to_string(it - matches.begin() + 1) + "/" + std::to_string(matches.size());
            add("n/N", where, 0,
                hbox({text("n/N") | bold | color(theme::accent()),
                      text(" " + where + "  ") | (matches.empty() ? color(theme::yellow()) : dim)}));
            add("/", "search", 5);
        }
        if (!data.errors.empty()) {
            const size_t n = data.errors.size();
            const std::string label = std::to_string(n) + (n == 1 ? " error" : " errors");
            add("e/E", label, 1, hbox({text("e/E") | bold | color(theme::accent()), text(" " + label + "  ") | color(theme::red())}));
        }
        add("f", follow ? "pause" : "follow", 3);
        add("←/→", "sideways", 6);
        add("y", "copy path", 5);
        if (!paths[1].empty()) add("Tab", "output/errors", 2);
        add("q", "close", 0);

        // Drop the least useful while it doesn't fit, keep the order
        int total = 0;
        for (const auto& h : hints) total += h.width;
        for (int rank = 6; rank > 0 && total > width; rank--)
            for (auto& h : hints)
                if (h.rank == rank && h.e && total > width) {
                    total -= h.width;
                    h.e = nullptr;
                }
        Elements out;
        for (auto& h : hints)
            if (h.e) out.push_back(h.e);
        return hbox(std::move(out));
    }

    Element render(int width, int height) {
        const int inner = std::max(10, width - 4);
        page = std::max(1, height - 6);

        Elements tabs = {which == 0 ? text(" output ") | inverted | bold : text(" output ") | dim};
        if (!paths[1].empty()) tabs.push_back(which == 1 ? text(" errors ") | inverted | bold : text(" errors ") | dim);
        std::string info, errors;
        if (!data.error.empty() || data.path.empty()) info = loading ? "reading..." : "";
        else info = util::bytes(data.size) + " · " + std::to_string(data.lines.size()) + " lines" + (data.cut ? " (end)" : "");
        if (data.error.empty() && !data.errors.empty())
            errors = std::to_string(data.errors.size()) + (data.errors.size() == 1 ? " error" : " errors");
        // The path gets what the tabs, the sizes and the state leave
        const int beside = 8 * static_cast<int>(tabs.size()) + 2 + static_cast<int>(util::width(info)) +
                           (errors.empty() ? 0 : 3 + static_cast<int>(errors.size())) + 2 + (follow ? 9 : 6) + 1;
        tabs.push_back(text("  "));
        tabs.push_back(text(util::cutMiddle(shortPath(path()), static_cast<size_t>(std::max(10, inner - beside)))) |
                       color(theme::cyan()) | fileLink(path()));
        tabs.push_back(filler());
        tabs.push_back(text(info) | dim);
        if (!errors.empty()) {
            tabs.push_back(text(" · ") | dim);
            tabs.push_back(text(errors) | color(theme::red()));
        }
        tabs.push_back(text("  "));
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
            const std::string needle = util::lower(query);
            auto e = std::lower_bound(data.errors.begin(), data.errors.end(), first);
            for (int i = first; i < std::min(static_cast<int>(data.lines.size()), first + page); i++) {
                const std::string& l = data.lines[static_cast<size_t>(i)];
                const bool error = e != data.errors.end() && *e == i;
                if (error) ++e;
                std::string shown = hscroll > 0 ? util::dropStart(l, static_cast<size_t>(hscroll)) : l;
                shown = util::cut(shown, static_cast<size_t>(text_width));
                Element number = text(util::padLeft(std::to_string(i + 1), static_cast<size_t>(digits)) + " ");
                if (i == match_line || i == error_line) number = number | bold | color(error ? theme::red() : theme::accent());
                else number = error ? number | color(theme::red()) : number | dim;
                Element body = needle.empty() ? text(shown) : marked(shown, needle);
                lines.push_back(hbox({number, error ? body | color(theme::red()) : body}));
            }
            // A scroll bar needs every line: draw the window alone, with its position in the title instead
        }
        while (static_cast<int>(lines.size()) < page) lines.push_back(text(""));

        const int total = static_cast<int>(data.lines.size());
        const std::string where = total > page ? std::to_string(std::min(total, offset + page) * 100 / std::max(1, total)) + "%" : "";
        return panel(title, vbox({hbox(std::move(tabs)), separator() | dim, vbox(std::move(lines)) | flex,
                                  separator() | dim, bottomLine(inner)}) |
                                size(WIDTH, EQUAL, width - 2) | size(HEIGHT, EQUAL, height - 2),
                     theme::accent(), text(where));
    }
};

}  // namespace ui
