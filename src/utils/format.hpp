#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <ftxui/screen/string.hpp>

namespace util {

// ---------------------------------------------------------------- strings

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline bool startsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(sep, start);
        out.push_back(s.substr(start, pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

inline std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// A number, or `fallback` when the text isn't one (std::stoi throws, and a thrown exception ends the program)
inline long toLong(const std::string& s, long fallback = 0) {
    if (s.empty()) return fallback;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || errno == ERANGE) return fallback;
    return value;
}

inline double toDouble(const std::string& s, double fallback = 0) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : value;
}

// ---------------------------------------------------------------- display width

// Width on screen, in columns: CJK characters and most emoji take two (FTXUI's own measure)
inline size_t width(const std::string& s) {
    return static_cast<size_t>(std::max(0, ftxui::string_width(s)));
}

// Byte length of the UTF-8 character starting at `s[i]`
inline size_t charLength(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return std::min(len, s.size() - i);
}

// The longest start of `s` (whole characters) at most `cols` wide
inline std::string fitStart(const std::string& s, size_t cols) {
    std::string out;
    size_t used = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t len = charLength(s, i);
        const std::string ch = s.substr(i, len);
        const size_t w = width(ch);
        if (used + w > cols) break;
        out += ch;
        used += w;
        i += len;
    }
    return out;
}

// The longest end of `s` (whole characters) at most `cols` wide
inline std::string fitEnd(const std::string& s, size_t cols) {
    std::vector<std::string> chars;
    for (size_t i = 0; i < s.size(); i += charLength(s, i))
        chars.push_back(s.substr(i, charLength(s, i)));

    std::string out;
    size_t used = 0;
    for (size_t k = chars.size(); k-- > 0;) {
        const size_t w = width(chars[k]);
        if (used + w > cols) break;
        out = chars[k] + out;
        used += w;
    }
    return out;
}

// What is left after the first `cols` columns (horizontal scrolling)
inline std::string dropStart(const std::string& s, size_t cols) {
    size_t used = 0, i = 0;
    while (i < s.size() && used < cols) {
        const size_t len = charLength(s, i);
        used += width(s.substr(i, len));
        i += len;
    }
    return s.substr(i);
}

// Cuts to `cols` columns (not bytes, not characters: "数" takes two), ending with "…"
inline std::string cut(const std::string& s, size_t cols) {
    if (width(s) <= cols) return s;
    if (cols == 0) return "";
    return fitStart(s, cols - 1) + "…";
}

// Keeps the start and the end (the file name) of a long path: /home/demo/…/lbm/lbm-812345.out
inline std::string cutMiddle(const std::string& s, size_t cols) {
    if (width(s) <= cols) return s;
    if (cols < 8) return cut(s, cols);
    const size_t keep_end = (cols - 1) * 2 / 3;
    const size_t keep_start = cols - 1 - keep_end;
    return fitStart(s, keep_start) + "…" + fitEnd(s, keep_end);
}

// Exactly `cols` columns: cut, or padded with spaces on the right (or on the left)
inline std::string padRight(const std::string& s, size_t cols) {
    std::string out = cut(s, cols);
    const size_t w = width(out);
    if (w < cols) out.append(cols - w, ' ');
    return out;
}

inline std::string padLeft(const std::string& s, size_t cols) {
    std::string out = cut(s, cols);
    const size_t w = width(out);
    if (w < cols) out.insert(0, cols - w, ' ');
    return out;
}

/* A log line made safe to draw
- Escape sequences (colors, cursor moves) are removed: they would move FTXUI's cursor
- A carriage return keeps what was written last (progress bars), tabs become spaces
- Other control characters and invalid UTF-8 become '?'
*/
inline std::string sanitize(const std::string& in) {
    std::string s = in;
    if (!s.empty() && s.back() == '\r') s.pop_back();
    const size_t cr = s.rfind('\r');
    if (cr != std::string::npos) s = s.substr(cr + 1);

    std::string out;
    size_t col = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {
            // CSI: ESC [ ... final byte in 0x40-0x7e ; OSC: ESC ] ... BEL or ESC backslash ; others: 2 bytes
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
                i++;
            } else if (i + 1 < s.size() && s[i + 1] == ']') {
                i += 2;
                while (i < s.size() && s[i] != '\a' && !(s[i] == 0x1b && i + 1 < s.size() && s[i + 1] == '\\')) i++;
                i += (i < s.size() && s[i] == 0x1b) ? 2 : 1;
            } else {
                i += 2;
            }
            continue;
        }
        if (c == '\t') {
            const size_t spaces = 8 - col % 8;
            out.append(spaces, ' ');
            col += spaces;
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            out += '?';
            col++;
            i++;
            continue;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
            col++;
            i++;
            continue;
        }
        // UTF-8: check the length and the continuation bytes
        const size_t len = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        bool valid = len > 0 && i + len <= s.size();
        for (size_t k = 1; valid && k < len; k++)
            valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (!valid) {
            out += '?';
            col++;
            i++;
            continue;
        }
        // Invisible characters: FTXUI gives them a column, terminals none, and the rest of the line would shift
        // (zero-width spaces and joiners, direction marks, BOM, variation selectors, soft hyphen)
        uint32_t cp = len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (cp == 0xAD || cp == 0x34F || cp == 0x61C || (cp >= 0x180B && cp <= 0x180F) || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
            cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE01EF) || (cp >= 0x80 && cp < 0xA0)) {
            i += len;
            continue;
        }
        const std::string ch = s.substr(i, len);
        out += ch;
        col += width(ch);
        i += len;
    }
    return out;
}

// ---------------------------------------------------------------- durations and dates

constexpr long UNKNOWN = -1;
constexpr long UNLIMITED = -2;

/* A Slurm duration in seconds
- Without days: "30" (minutes, like --time), "12:34" (min:s), "1:02:03" (h:min:s), "00:11.237" (sacct, min:s.ms)
- With days: "2-12" (h), "2-12:30" (h:min), "2-22:34:00" (h:min:s)
- UNLIMITED (or INFINITE) for no limit, UNKNOWN for "N/A", "INVALID", "" and anything else
*/
inline long parseDuration(const std::string& text) {
    const std::string s = trim(text);
    if (s == "UNLIMITED" || s == "INFINITE" || s == "Partition_Limit") return UNLIMITED;
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) return UNKNOWN;

    long days = 0;
    std::string rest = s;
    const size_t dash = rest.find('-');
    const bool with_days = dash != std::string::npos;
    if (with_days) {
        days = toLong(rest.substr(0, dash), -1);
        rest = rest.substr(dash + 1);
    }
    const size_t dot = rest.find('.');
    const bool with_fraction = dot != std::string::npos;
    if (with_fraction) rest = rest.substr(0, dot);

    std::vector<long> parts;
    for (const auto& p : split(rest, ':')) {
        const long v = toLong(p, -1);
        if (v < 0) return UNKNOWN;
        parts.push_back(v);
    }
    if (days < 0 || parts.empty() || parts.size() > 3) return UNKNOWN;

    long seconds = 0;
    if (parts.size() == 3)
        seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
    else if (parts.size() == 2)
        seconds = with_days ? parts[0] * 3600 + parts[1] * 60 : parts[0] * 60 + parts[1];
    else
        seconds = with_days ? parts[0] * 3600 : with_fraction ? parts[0] : parts[0] * 60;
    return days * 86400 + seconds;
}

// Short duration: 45s, 12m, 3h12m, 1d05h, 12d
inline std::string duration(long seconds) {
    if (seconds == UNLIMITED) return "unlimited";
    if (seconds < 0) return "-";
    char buf[32];
    if (seconds < 60) std::snprintf(buf, sizeof buf, "%lds", seconds);
    else if (seconds < 3600) std::snprintf(buf, sizeof buf, "%ldm", seconds / 60);
    else if (seconds < 86400) std::snprintf(buf, sizeof buf, "%ldh%02ldm", seconds / 3600, seconds % 3600 / 60);
    else if (seconds < 10 * 86400) std::snprintf(buf, sizeof buf, "%ldd%02ldh", seconds / 86400, seconds % 86400 / 3600);
    else std::snprintf(buf, sizeof buf, "%ldd", seconds / 86400);
    return buf;
}

// "2026-10-22T10:39:59" (local time, as Slurm prints it), 0 for "N/A", "Unknown", "None"...
inline std::time_t parseTime(const std::string& s) {
    std::tm tm{};
    int Y, M, D, h, m, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t < 0 ? 0 : t;
}

// A date that reads well next to now: 14:32 today, Wed 09:10 this week, 22 Oct 18:05 otherwise
inline std::string clock(std::time_t t, std::time_t now = std::time(nullptr)) {
    if (t <= 0) return "-";
    std::tm tm{}, today{};
    localtime_r(&t, &tm);
    localtime_r(&now, &today);
    char buf[32];
    const bool same_day = tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday;
    const double days = std::difftime(t, now) / 86400.0;
    if (same_day) std::strftime(buf, sizeof buf, "%H:%M", &tm);
    else if (days > -6 && days < 6) std::strftime(buf, sizeof buf, "%a %H:%M", &tm);
    else std::strftime(buf, sizeof buf, "%d %b %H:%M", &tm);
    return buf;
}

// "in 25m", "3h12m ago"
inline std::string relative(std::time_t t, std::time_t now = std::time(nullptr)) {
    if (t <= 0) return "";
    const long d = static_cast<long>(std::difftime(t, now));
    if (d >= 0) return "in " + duration(d);
    return duration(-d) + " ago";
}

// ---------------------------------------------------------------- sizes

// "324M", "1.5G", "512K", "8000" (MB when no unit, like Slurm's memory values) to MiB
inline double parseMiB(const std::string& text, double no_unit = 1.0) {
    const std::string s = trim(text);
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) return 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    switch (end && *end ? std::toupper(static_cast<unsigned char>(*end)) : 0) {
        case 'K': return v / 1024;
        case 'M': return v;
        case 'G': return v * 1024;
        case 'T': return v * 1024 * 1024;
        case 'P': return v * 1024 * 1024 * 1024;
        default: return v * no_unit;
    }
}

// MiB in a short format: 512M, 1.5G, 801G, 1.2T
inline std::string mib(double v) {
    char buf[32];
    if (v <= 0) return "0";
    if (v < 1024) std::snprintf(buf, sizeof buf, "%.0fM", v);
    else if (v < 10 * 1024) std::snprintf(buf, sizeof buf, "%.1fG", v / 1024);
    else if (v < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.0fG", v / 1024);
    else std::snprintf(buf, sizeof buf, "%.1fT", v / 1024 / 1024);
    return buf;
}

// Bytes in a short format: 812B, 4.0K, 12M, 1.5G
inline std::string bytes(uintmax_t b) {
    if (b < 1024) return std::to_string(b) + "B";
    char buf[32];
    const double k = b / 1024.0;
    if (k < 1024) std::snprintf(buf, sizeof buf, "%.1fK", k);
    else return mib(k / 1024);
    return buf;
}

}  // namespace util
