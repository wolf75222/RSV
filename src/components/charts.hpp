#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>

#include "../utils/format.hpp"

/* Charts for the live view (m), in two styles
- btop's: one value per column, each character showing the step from the value before to this one (5 x 5 levels
  in a braille or block character), the rows coloured along a gradient. The idea comes from btop (Apache-2.0,
  github.com/aristocratos/btop, src/btop_draw.cpp: Draw::Graph and Draw::Meter); the code is RSV's own.
- A task manager's: a line over a tinted area on FTXUI's canvas, with a grid, its scale on the left and the time
  under it
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

// ---------------------------------------------------------------- btop's graph

/* How a graph is drawn
- Bars: one value per column, its top to an eighth of a character (▁ ▂ ▃ ▄ ▅ ▆ ▇ █): smooth and solid
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

/* The character for a left and a right level (0..4 dots each), filled from the bottom (up) or the top (down)
- Braille: the left column's dots are 1 2 3 7 from the top, the right one's 4 5 6 8
- Blocks: a quarter of a character per half and per two levels (▖ ▌ ▗ ▐ and what they make together)
*/
inline const std::string& glyph(Symbols s, bool up, int left, int right) {
    static std::vector<std::string> tables[4];
    auto& t = tables[(s == Symbols::Block ? 2 : 0) + (up ? 0 : 1)];
    if (t.empty()) {
        for (int l = 0; l <= 4; l++)
            for (int r = 0; r <= 4; r++) {
                if (s == Symbols::Braille) {
                    static const int lb[4] = {0x40, 0x04, 0x02, 0x01}, rb[4] = {0x80, 0x20, 0x10, 0x08};   // from the bottom
                    int bits = 0;
                    for (int d = 0; d < l; d++) bits |= up ? lb[d] : lb[3 - d];
                    for (int d = 0; d < r; d++) bits |= up ? rb[d] : rb[3 - d];
                    t.push_back(bits ? utf8(0x2800 + bits) : " ");
                } else {
                    // Each half: 0 nothing, 1 its quarter near the base, 2 all of it
                    const int lh = l == 0 ? 0 : l <= 2 ? 1 : 2, rh = r == 0 ? 0 : r <= 2 ? 1 : 2;
                    static const char* upChars[3][3] = {{" ", "▗", "▐"}, {"▖", "▄", "▟"}, {"▌", "▙", "█"}};
                    static const char* downChars[3][3] = {{" ", "▝", "▐"}, {"▘", "▀", "▜"}, {"▌", "▛", "█"}};
                    t.push_back((up ? upChars : downChars)[lh][rh]);
                }
            }
    }
    return t[static_cast<size_t>(left * 5 + right)];
}

}  // namespace detail

/* A graph of the last `values` (0..1, -1: no reading), one per column, the newest on the right
- The rows are coloured along `g`, the top ones with its high end
- `invert` hangs it from the top (the writes under the reads)
- `base`: a faint line along the bottom where there is nothing (small graphs read better with it)
*/
inline Element graph(const std::vector<double>& values, int width, int height, const Gradient& g, bool invert = false,
                     Symbols symbols = Symbols::Bars, bool base = false) {
    width = std::max(1, width);
    height = std::max(1, height);
    std::vector<double> v(static_cast<size_t>(width), -1);
    const int n = static_cast<int>(values.size());
    for (int x = 0; x < width && x < n; x++) v[static_cast<size_t>(width - 1 - x)] = values[static_cast<size_t>(n - 1 - x)];
    const int per_row = symbols == Symbols::Bars ? 8 : 4;
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
        int run_kind = -1;   // 0 drawn, 1 drawn inverted (a bar hanging down), 2 the faint base
        auto flush = [&] {
            if (run.empty()) return;
            Element e = text(run);
            runs.push_back(run_kind == 2 ? e | color(faint()) : run_kind == 1 ? e | color(c) | inverted : e | color(c));
            run.clear();
        };
        auto put = [&](const std::string& ch, int kind) {
            if (kind != run_kind) {
                flush();
                run_kind = kind;
            }
            run += ch;
        };
        for (int x = 0; x < width; x++) {
            const int cur = levels[static_cast<size_t>(x)];
            if (symbols == Symbols::Bars) {
                const int k = cur < 0 ? 0 : std::clamp(cur - level * 8, 0, 8);
                if (k == 0) put(base && level == 0 ? (invert ? "▔" : "▁") : " ", base && level == 0 ? 2 : 0);
                else if (!invert || k == 8) put(eighths[k], 0);
                else put(eighths[8 - k], 1);   // from the top: the rest of the character, inverted
                continue;
            }
            const int prev = x > 0 && levels[static_cast<size_t>(x - 1)] >= 0 ? levels[static_cast<size_t>(x - 1)] : cur;
            if (cur < 0) {
                put(base && level == 0 ? detail::glyph(symbols, !invert, 1, 1) : " ", base && level == 0 ? 2 : 0);
                continue;
            }
            const int l = std::clamp(prev - level * 4, 0, 4), rr = std::clamp(cur - level * 4, 0, 4);
            if (base && level == 0 && l == 0 && rr == 0) put(detail::glyph(symbols, !invert, 1, 1), 2);
            else put(detail::glyph(symbols, !invert, l, rr), 0);
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
    std::vector<double> values;   // 0..1, two per column (the canvas' dots), -1: no reading
    Color color;
    bool fill = true;             // the area under it, tinted
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

inline Element lineChart(const std::vector<Line>& lines, int width, int height, const std::function<std::string(double)>& y_label,
                         const std::function<std::string(int)>& x_label, int x_step = 16) {
    const int rows = std::max(2, height - 1);
    const int grid_rows = detail::gridRows(rows);
    const size_t label_w = detail::labelWidth(rows, y_label);
    const int cols = lineColumns(width, height, y_label);
    const int dots_w = cols * 2, dots_h = rows * 4;

    // The rows of the grid lines, from the top
    std::vector<int> grid_at;
    for (int k = 0; k <= grid_rows; k++) grid_at.push_back(static_cast<int>(std::lround((rows - 1) * double(k) / grid_rows)));

    Element plot = canvas(dots_w, dots_h, [lines, rows, cols, dots_w, dots_h, grid_at, x_step](Canvas& cv) {
        const auto dim_line = [](Cell& c) {
            c.foreground_color = Color::RGB(0x34, 0x34, 0x3a);
        };
        // Grid: dotted rows and columns, under everything
        for (int r : grid_at)
            for (int x = 0; x < dots_w; x += 2) cv.DrawPoint(x, r * 4 + (r == rows - 1 ? 3 : 0), true, dim_line);
        for (int c = cols - 1; c >= 0; c -= x_step)
            for (int y = 0; y < dots_h; y += 2) cv.DrawPoint(c * 2 + 1, y, true, dim_line);

        const auto dotY = [dots_h](double v) { return dots_h - 1 - static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * (dots_h - 1))); };
        // Tinted areas, then the lines over them
        for (const auto& line : lines) {
            if (!line.fill) continue;
            const Color tint = Color::Interpolate(0.2f, Color::RGB(0x14, 0x16, 0x1a), line.color);
            for (int c = 0; c < cols; c++) {
                int lowest = -1;
                for (int side = 0; side < 2; side++) {
                    const size_t i = static_cast<size_t>(c * 2 + side);
                    if (i < line.values.size() && line.values[i] >= 0) lowest = std::max(lowest, dotY(line.values[i]));
                }
                if (lowest < 0) continue;
                for (int r = lowest / 4 + (lowest % 4 == 3 ? 1 : 0); r < rows; r++)
                    cv.Style(c * 2, r * 4, [tint](Cell& cell) { cell.background_color = tint; });
            }
        }
        for (const auto& line : lines) {
            const Color col = line.color;
            const auto ink = [col](Cell& cell) {
                cell.foreground_color = col;
                cell.bold = true;
            };
            for (size_t i = 0; i < line.values.size() && static_cast<int>(i) < dots_w; i++) {
                if (line.values[i] < 0) continue;
                const int x = static_cast<int>(i), y = dotY(line.values[i]);
                // Two dots thick: one under the other, the line reads as a line and not as dots
                const int prev = i > 0 && line.values[i - 1] >= 0 ? dotY(line.values[i - 1]) : y;
                cv.DrawPointLine(x - (i > 0 ? 1 : 0), prev, x, y, ink);
                cv.DrawPointLine(x - (i > 0 ? 1 : 0), std::min(dots_h - 1, prev + 1), x, std::min(dots_h - 1, y + 1), ink);
            }
        }
    });

    // The scale on the left, at the grid's rows
    Elements left;
    for (int r = 0; r < rows; r++) {
        std::string label;
        for (int k = 0; k <= grid_rows; k++)
            if (grid_at[static_cast<size_t>(k)] == r) label = y_label(1.0 - double(k) / grid_rows);
        left.push_back(text(util::padLeft(label, label_w) + " ") | color(graphText()));
    }
    // The time under it, at the grid's columns (the last one is "now")
    std::string under(static_cast<size_t>(cols), ' ');
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
