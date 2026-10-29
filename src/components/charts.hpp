#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../utils/format.hpp"

/* Charts for the live view (m)
- Areas: one value per column, solid from the base, its top to an eighth of a row (▁ ▂ ▃ ▄ ▅ ▆ ▇ █) in a bright
  edge, the rest darker: no dots, no holes, at any size
- btop's, on v: each character shows the step from the value before to this one (5 x 5 levels in a braille or block
  character). The idea comes from btop (Apache-2.0, github.com/aristocratos/btop, src/btop_draw.cpp: Draw::Graph and
  Draw::Meter); the code is RSV's own.
- Charts: the first curve as an area, the others as lines over it (─ ╭ ╮ ╰ ╯ │), a faint grid of plain lines, the
  scale on the left and the time under it
*/
namespace ui::chart {
using namespace ftxui;

// ---------------------------------------------------------------- colours

// Three colours along a height or a gauge: the low end, the middle, the top
struct Gradient {
    Color low, mid, high;
    Color at(double t) const {
        t = std::clamp(t, 0.0, 1.0);
        return t < 0.5 ? Color::Interpolate(static_cast<float>(t * 2), low, mid)
                       : Color::Interpolate(static_cast<float>((t - 0.5) * 2), mid, high);
    }
};

// btop's default theme
inline Gradient cpuGradient() { return {Color::RGB(0x77, 0xca, 0x9b), Color::RGB(0xcb, 0xc0, 0x6c), Color::RGB(0xdc, 0x4c, 0x4c)}; }
inline Gradient usedGradient() { return {Color::RGB(0x59, 0x2b, 0x26), Color::RGB(0xd9, 0x62, 0x6d), Color::RGB(0xff, 0x47, 0x69)}; }
inline Gradient freeGradient() { return {Color::RGB(0x38, 0x4f, 0x21), Color::RGB(0xb5, 0xe6, 0x85), Color::RGB(0xdc, 0xff, 0x85)}; }
inline Gradient cachedGradient() { return {Color::RGB(0x16, 0x33, 0x50), Color::RGB(0x74, 0xe6, 0xfc), Color::RGB(0x26, 0xc5, 0xff)}; }
inline Gradient availableGradient() { return {Color::RGB(0x4e, 0x3f, 0x0e), Color::RGB(0xff, 0xd7, 0x7a), Color::RGB(0xff, 0xb8, 0x14)}; }
inline Gradient readGradient() { return {Color::RGB(0x29, 0x1f, 0x75), Color::RGB(0x4f, 0x43, 0xa3), Color::RGB(0xb0, 0xa9, 0xde)}; }
inline Gradient writeGradient() { return {Color::RGB(0x62, 0x06, 0x65), Color::RGB(0x7d, 0x41, 0x80), Color::RGB(0xdc, 0xaf, 0xde)}; }
inline Gradient tempGradient() { return {Color::RGB(0x48, 0x97, 0xd4), Color::RGB(0x54, 0x74, 0xe8), Color::RGB(0xff, 0x40, 0xb6)}; }
inline Gradient gpuGradient() { return {Color::RGB(0x3c, 0x8d, 0x5a), Color::RGB(0x9a, 0xd8, 0x6e), Color::RGB(0xe8, 0xe0, 0x6a)}; }

inline Color faint() { return Color::RGB(0x40, 0x40, 0x40); }   // btop's meter_bg and inactive text
inline Color graphText() { return Color::RGB(0x80, 0x80, 0x80); }
inline Color gridColor() { return Color::RGB(0x2c, 0x30, 0x38); }

// The boxes' frames: each in the colour of what it shows (RSV's own colours); titles bright, keys in the accent
inline Color cpuBox() { return Color::RGB(0x77, 0xca, 0x9b); }
inline Color memBox() { return Color::RGB(0xd9, 0x62, 0x6d); }
inline Color netBox() { return Color::RGB(0x4f, 0x43, 0xa3); }
inline Color procBox() { return Color::RGB(0xcb, 0xc0, 0x6c); }
inline Color titleColor() { return Color::RGB(0xee, 0xee, 0xee); }
inline Color hiColor() { return Color::RGB(0x5f, 0xaf, 0xff); }
inline Color divColor() { return Color::RGB(0x40, 0x40, 0x40); }
inline std::string superscript(int n) {
    static const char* digits[10] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    return n >= 0 && n <= 9 ? digits[n] : "";
}
inline Color background() { return Color::RGB(0x16, 0x18, 0x1d); }

// The inside of an area, under its bright edge: the colour darkened (`light`: 0 the background, 1 the colour)
inline Color areaFill(Color c, double light) {
    return Color::Interpolate(static_cast<float>(std::clamp(light, 0.0, 1.0)), background(), c);
}

// ---------------------------------------------------------------- btop's graph

/* How a graph is drawn
- Bars: an area, one value per column, its top to an eighth of a character (▁ ▂ ▃ ▄ ▅ ▆ ▇ █) in a bright edge
- Braille, Block: btop's, each character showing the step from the value before (left half) to its own (right)
*/
enum class Symbols { Bars, Braille, Block };

namespace detail {

inline std::string utf8(int cp) {
    std::string s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

/* The character for a left and a right level (0..4 each): btop's tables (src/btop_draw.cpp, Symbols::graph_symbols),
   filled from the bottom (up) or from the top (down) */
inline const std::string& glyph(Symbols s, bool up, int left, int right) {
    static const std::vector<std::string> braille_up = {
        " ", "⢀", "⢠", "⢰", "⢸", "⡀", "⣀", "⣠", "⣰", "⣸", "⡄", "⣄", "⣤", "⣴", "⣼", "⡆", "⣆", "⣦", "⣶", "⣾", "⡇", "⣇", "⣧", "⣷", "⣿"};
    static const std::vector<std::string> braille_down = {
        " ", "⠈", "⠘", "⠸", "⢸", "⠁", "⠉", "⠙", "⠹", "⢹", "⠃", "⠋", "⠛", "⠻", "⢻", "⠇", "⠏", "⠟", "⠿", "⢿", "⡇", "⡏", "⡟", "⡿", "⣿"};
    static const std::vector<std::string> block_up = {
        " ", "▗", "▗", "▐", "▐", "▖", "▄", "▄", "▟", "▟", "▖", "▄", "▄", "▟", "▟", "▌", "▙", "▙", "█", "█", "▌", "▙", "▙", "█", "█"};
    static const std::vector<std::string> block_down = {
        " ", "▝", "▝", "▐", "▐", "▘", "▀", "▀", "▜", "▜", "▘", "▀", "▀", "▜", "▜", "▌", "▛", "▛", "█", "█", "▌", "▛", "▛", "█", "█"};
    const auto& t = s == Symbols::Block ? (up ? block_up : block_down) : (up ? braille_up : braille_down);
    return t[static_cast<size_t>(std::clamp(left, 0, 4) * 5 + std::clamp(right, 0, 4))];
}

/* btop's graph (Draw::Graph::_create): TWO readings per character, the left half the older one
- Each row is a band of the height: a reading fills it to 0..4 dots (+0.1 before rounding, as btop)
- `no_zero`: the bottom row shows at least one dot where there is a reading: a baseline that says "0", not "nothing"
- Rows coloured along the gradient by their height (the top one the high end); one row: each character by its value
*/
inline Element btopGraph(const std::vector<double>& values, int width, int height, const Gradient& g, bool invert, Symbols symbols, bool no_zero) {
    const int n = static_cast<int>(values.size()), samples = width * 2;
    std::vector<double> v(static_cast<size_t>(samples), -1);
    for (int i = 0; i < samples && i < n; i++) v[static_cast<size_t>(samples - 1 - i)] = values[static_cast<size_t>(n - 1 - i)];
    const double mod = height == 1 ? 0.3 : 0.1;
    Elements rows;
    for (int r = 0; r < height; r++) {
        const int horizon = invert ? height - 1 - r : r;   // bands from the top of an upright graph
        const double high = height > 1 ? double(height - horizon) / height : 1, low = height > 1 ? double(height - horizon - 1) / height : 0;
        const bool bottom = horizon == height - 1;
        const auto level = [&](double x) {
            if (x < 0) return 0;
            const int least = no_zero && bottom ? 1 : 0;
            if (x >= high - 1e-9) return 4;
            if (x <= low) return least;
            return std::clamp(static_cast<int>(std::lround((x - low) * 4 / (high - low) + mod)), least, 4);
        };
        const Color row_color = invert ? g.at(double(r + 1) / height) : g.at(1.0 - double(r) / height);
        Elements runs;
        std::string run;
        Color run_color = row_color;
        for (int x = 0; x < width; x++) {
            const double a = v[static_cast<size_t>(2 * x)], b = v[static_cast<size_t>(2 * x + 1)];
            const std::string& ch = (a < 0 && b < 0) ? glyph(symbols, !invert, 0, 0) : glyph(symbols, !invert, level(a), level(b));
            const Color c = height == 1 ? g.at(std::max(a, b)) : row_color;
            if (!(c == run_color) && !run.empty()) {
                runs.push_back(text(run) | color(run_color));
                run.clear();
            }
            run_color = c;
            run += ch;
        }
        if (!run.empty()) runs.push_back(text(run) | color(run_color));
        rows.push_back(hbox(std::move(runs)));
    }
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, width);
}

}  // namespace detail

// How many readings a column of a graph drawn so takes: btop's take two
inline int perColumn(Symbols s) { return s == Symbols::Bars ? 1 : 2; }

/* A graph of the last `values` (0..1, -1: no reading), the newest on the right: one per column (areas), two (btop's)
- The rows are coloured along `g`, the top ones with its high end
- `invert` hangs it from the top (the writes under the reads)
- `base`: a faint line along the bottom where there is nothing (small graphs read better with it)
*/
inline Element graph(const std::vector<double>& values, int width, int height, const Gradient& g, bool invert = false,
                     Symbols symbols = Symbols::Bars, bool base = false) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (symbols != Symbols::Bars) return detail::btopGraph(values, width, height, g, invert, symbols, true);
    std::vector<double> v(static_cast<size_t>(width), -1);
    const int n = static_cast<int>(values.size());
    for (int x = 0; x < width && x < n; x++) v[static_cast<size_t>(width - 1 - x)] = values[static_cast<size_t>(n - 1 - x)];
    const int per_row = 8;
    std::vector<int> levels(v.size());
    for (size_t i = 0; i < v.size(); i++)
        levels[i] = v[i] < 0 ? -1 : std::max(v[i] > 0.005 ? 1 : 0, static_cast<int>(std::lround(std::clamp(v[i], 0.0, 1.0) * height * per_row)));
    static const char* eighths[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

    Elements rows;
    for (int r = 0; r < height; r++) {
        const int level = invert ? r : height - 1 - r;   // rows between this one and the base
        const Color c = g.at(double(level + 1) / height);
        Elements runs;
        std::string run;
        Color run_color;
        int run_kind = -1;   // 0 drawn, 1 drawn inverted (a bar hanging down), 2 the faint base
        auto flush = [&] {
            if (run.empty()) return;
            Element e = text(run) | color(run_kind == 2 ? faint() : run_color);
            runs.push_back(run_kind == 1 ? e | inverted : e);
            run.clear();
        };
        auto put = [&](const std::string& ch, int kind, Color col) {
            if (kind != run_kind || !(col == run_color)) {
                flush();
                run_kind = kind;
                run_color = col;
            }
            run += ch;
        };
        for (int x = 0; x < width; x++) {
            const int cur = levels[static_cast<size_t>(x)];
            // An area: each column in the colour of its value, its top bright, the rest darker
            const int k = cur < 0 ? 0 : std::clamp(cur - level * 8, 0, 8);
            const bool edge = k > 0 && cur <= (level + 1) * 8;
            const Color col = g.at(v[static_cast<size_t>(x)]);
            if (k == 0) put(base && level == 0 ? (invert ? "▔" : "▁") : " ", base && level == 0 ? 2 : 0, c);
            else if (!invert || k == 8) put(eighths[k], 0, edge ? col : areaFill(col, 0.55));
            else put(eighths[8 - k], 1, col);   // from the top: the rest of the character, inverted
        }
        flush();
        rows.push_back(hbox(std::move(runs)));
    }
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, width);
}

// A one-line graph: the last values, each column's height to an eighth and its colour along `g` by its value
inline Element sparkline(const std::vector<double>& values, int width, const Gradient& g) {
    static const char* eighths[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    width = std::max(1, width);
    const int n = static_cast<int>(values.size());
    Elements cells;
    for (int x = 0; x < width; x++) {
        const int i = n - width + x;
        const double v = i >= 0 && i < n ? values[static_cast<size_t>(i)] : -1;
        const int k = v < 0 ? 0 : std::max(v > 0.005 ? 1 : 0, static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 8)));
        cells.push_back(k == 0 ? text("▁") | color(faint()) : text(eighths[k]) | color(g.at(v)));
    }
    return hbox(std::move(cells));
}

/* A meter: `width` squares, the filled ones coloured along `g` by where they are, the others faint */
inline Element meter(double ratio, int width, const Gradient& g) {
    width = std::max(1, width);
    const int filled = ratio < 0 ? 0 : static_cast<int>(std::lround(std::clamp(ratio, 0.0, 1.0) * width));
    Elements cells;
    for (int i = 0; i < width; i++)
        cells.push_back(text("■") | color(i < filled ? g.at(width == 1 ? 1.0 : double(i) / (width - 1)) : faint()));
    return hbox(std::move(cells));
}

// ---------------------------------------------------------------- a task manager's chart

struct Line {
    std::vector<double> values;   // 0..1, two per column, -1: no reading
    Color color;
    bool fill = true;             // only the first line's counts: an area under it
};

/* Lines over a grid: `y_label(f)` names the height f (0..1) on the left, `x_label(columns from the right)` the
   time under it; `width` and `height` include the labels */
namespace detail {
inline int gridRows(int rows) { return rows >= 12 ? 4 : rows >= 6 ? 2 : 1; }
inline size_t labelWidth(int rows, const std::function<std::string(double)>& y_label) {
    size_t w = 0;
    for (int k = 0, n = gridRows(rows); k <= n; k++) w = std::max(w, util::width(y_label(double(k) / n)));
    return w;
}
}  // namespace detail

// How many columns of curve a chart of this size has: its values are twice as many
inline int lineColumns(int width, int height, const std::function<std::string(double)>& y_label) {
    return std::max(4, width - static_cast<int>(detail::labelWidth(std::max(2, height - 1), y_label)) - 1);
}

/* - The first line, when `fill`: a solid area, to an eighth of a row, its top cell in the line's colour
   - The others: lines of box characters, a row each, the first ones over the later ones; over the area they keep its
     colour behind them
   - The grid: plain faint lines (─ │ ┼) where nothing is drawn
*/
inline Element lineChart(const std::vector<Line>& lines, int width, int height, const std::function<std::string(double)>& y_label,
                         const std::function<std::string(int)>& x_label, int x_step = 16) {
    static const char* eighths[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const int rows = std::max(2, height - 1);
    const int grid_rows = detail::gridRows(rows);
    const size_t label_w = detail::labelWidth(rows, y_label);
    const int cols = lineColumns(width, height, y_label);
    x_step = std::max(1, x_step);

    std::vector<int> grid_at;
    std::vector<bool> grid_row(static_cast<size_t>(rows), false);
    for (int k = 0; k <= grid_rows; k++) {
        grid_at.push_back(static_cast<int>(std::lround((rows - 1) * double(k) / grid_rows)));
        grid_row[static_cast<size_t>(grid_at.back())] = true;
    }

    // The area: a height in eighths of a row for each column (the higher of its two values)
    const bool area = !lines.empty() && lines[0].fill;
    std::vector<int> level(static_cast<size_t>(cols), -1);
    if (area) {
        for (int c = 0; c < cols; c++) {
            double v = -1;
            for (int side = 0; side < 2; side++) {
                const size_t i = static_cast<size_t>(c * 2 + side);
                if (i < lines[0].values.size()) v = std::max(v, lines[0].values[i]);
            }
            if (v >= 0) level[static_cast<size_t>(c)] = std::max(v > 0.005 ? 1 : 0, static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * rows * 8)));
        }
    }

    // The lines: one row per cell, drawn with box lines (─ ╭ ╮ ╰ ╯ │); a cell keeps the first line that went through it
    std::vector<std::string> glyphs(static_cast<size_t>(rows * cols));
    std::vector<int> owner(static_cast<size_t>(rows * cols), -1);
    const auto put_line = [&](int c, int row_from_bottom, const char* g, int who) {
        if (c < 0 || c >= cols || row_from_bottom < 0 || row_from_bottom >= rows) return;
        const size_t cell = static_cast<size_t>((rows - 1 - row_from_bottom) * cols + c);
        if (owner[cell] >= 0 && owner[cell] < who) return;
        owner[cell] = who;
        glyphs[cell] = g;
    };
    for (int who = static_cast<int>(lines.size()) - 1; who >= (area ? 1 : 0); who--) {
        const auto& v = lines[static_cast<size_t>(who)].values;
        int prev = -1;
        for (int c = 0; c < cols; c++) {
            double x = -1;
            for (int side = 0; side < 2; side++) {
                const size_t i = static_cast<size_t>(c * 2 + side);
                if (i < v.size() && v[i] >= 0) x = x < 0 ? v[i] : (x + v[i]) / 2;
            }
            if (x < 0) {
                prev = -1;
                continue;
            }
            const int y = static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * (rows - 1)));
            if (prev < 0 || prev == y) {
                put_line(c, y, "─", who);
            } else if (y > prev) {   // up: from the left at prev, to the right at y
                put_line(c, prev, "╯", who);
                for (int k = prev + 1; k < y; k++) put_line(c, k, "│", who);
                put_line(c, y, "╭", who);
            } else {                 // down
                put_line(c, prev, "╮", who);
                for (int k = y + 1; k < prev; k++) put_line(c, k, "│", who);
                put_line(c, y, "╰", who);
            }
            prev = y;
        }
    }

    Elements out_rows;
    for (int r = 0; r < rows; r++) {
        const int lv = rows - 1 - r;   // rows under this one
        const Color fill = area ? areaFill(lines[0].color, 0.55) : Color();
        Elements runs;
        std::string run;
        Color run_fg, run_bg;
        bool run_has_bg = false, run_bold = false, started = false;
        const auto flush = [&] {
            if (run.empty()) return;
            Element e = text(run) | color(run_fg);
            if (run_has_bg) e = e | bgcolor(run_bg);
            if (run_bold) e = e | bold;
            runs.push_back(e);
            run.clear();
        };
        const auto put = [&](const std::string& ch, Color fg, bool has_bg, Color bg, bool is_bold) {
            if (!started || !(fg == run_fg) || has_bg != run_has_bg || (has_bg && !(bg == run_bg)) || is_bold != run_bold) {
                flush();
                run_fg = fg;
                run_bg = bg;
                run_has_bg = has_bg;
                run_bold = is_bold;
                started = true;
            }
            run += ch;
        };
        for (int c = 0; c < cols; c++) {
            const size_t cell = static_cast<size_t>(r * cols + c);
            const int lvl = level[static_cast<size_t>(c)];
            const int k = lvl < 0 ? 0 : std::clamp(lvl - lv * 8, 0, 8);
            const bool edge = k > 0 && lvl <= (lv + 1) * 8;
            if (owner[cell] >= 0) {
                const Color ink = lines[static_cast<size_t>(owner[cell])].color;
                put(glyphs[cell], ink, k == 8 && !edge, fill, true);
            } else if (k > 0) {
                put(eighths[k], edge ? lines[0].color : fill, false, Color(), false);
            } else {
                const bool gr = grid_row[static_cast<size_t>(r)], gc = (cols - 1 - c) % x_step == 0;
                put(gr && gc ? "┼" : gr ? "─" : gc ? "│" : " ", gridColor(), false, Color(), false);
            }
        }
        flush();
        out_rows.push_back(hbox(std::move(runs)));
    }
    Element plot = vbox(std::move(out_rows));

    // The scale on the left, at the grid's rows
    Elements left;
    for (int r = 0; r < rows; r++) {
        std::string label;
        for (int k = 0; k <= grid_rows; k++)
            if (grid_at[static_cast<size_t>(k)] == r) label = y_label(1.0 - double(k) / grid_rows);
        left.push_back(text(util::padLeft(label, label_w) + " ") | color(graphText()));
    }
    // The time under it, at the grid's columns (the last one is "now")
    std::vector<std::pair<int, std::string>> ticks;
    for (int c = cols - 1; c >= 0; c -= x_step) ticks.push_back({c, x_label(cols - 1 - c)});
    Elements bottom = {text(std::string(label_w + 1, ' '))};
    std::string axis;
    int at = 0;
    for (auto it = ticks.rbegin(); it != ticks.rend(); ++it) {
        const int w = static_cast<int>(util::width(it->second));
        const int start = std::max(at, std::min(cols - w, it->first - w / 2));
        if (start < at || start + w > cols) continue;
        axis += std::string(static_cast<size_t>(start - at), ' ') + it->second;
        at = start + w;
    }
    bottom.push_back(text(axis) | color(graphText()));
    return vbox({hbox({vbox(std::move(left)), plot}), hbox(std::move(bottom))}) | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
}

// ---------------------------------------------------------------- scales

// A round top for a scale: 1, 2 or 5 times a power of ten, at least `peak`
inline double niceMax(double peak) {
    if (peak <= 0) return 1;
    const double p = std::pow(10.0, std::floor(std::log10(peak)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * p >= peak) return m * p;
    return 10 * p;
}

// 1536 -> "1.5 KB", for rates and totals of bytes
inline std::string bytes(double b) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int u = 0;
    while (b >= 1000 && u < 5) {
        b /= 1000;
        u++;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, b < 10 && u > 0 ? "%.1f %s" : "%.0f %s", b, units[u]);
    return buf;
}

}  // namespace ui::chart
