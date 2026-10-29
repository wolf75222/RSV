#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "charts.hpp"
#include "theme.hpp"

// What the live views (m) share: readings in time, and the pieces they are drawn with
namespace ui {
using namespace ftxui;

// ---------------------------------------------------------------- readings in time

/* Readings of one quantity: (seconds since the epoch, value)
- Two readings less than half a second apart are one: nvidia-smi gives the GPUs of a node one line each
- An hour is kept: the longest the widest zoom shows
*/
struct Series {
    std::vector<std::pair<double, double>> points;

    void add(double at, double value) {
        if (!points.empty() && at - points.back().first < 0.5) points.back() = {points.back().first, value};
        else points.emplace_back(at, value);
        if (points.size() > 64 && at - points.front().first > 3600)
            points.erase(points.begin(), points.begin() + static_cast<long>(points.size() / 8));
    }
    bool empty() const { return points.empty(); }
    double last() const { return points.empty() ? -1 : points.back().second; }
    double peak(double since = 0) const {
        double p = 0;
        for (const auto& [at, v] : points)
            if (at >= since) p = std::max(p, v);
        return p;
    }
};

/* The series as `n` values, one per `step` seconds, the newest at the right (at `now`), each divided by `scale`
- Several readings in one: their mean. None: the one before holds (sstat answers every 2 s)
- -1 before the first reading: nothing is drawn there
*/
inline std::vector<double> resample(const Series& s, double now, double step, int n, double scale) {
    std::vector<double> out(static_cast<size_t>(std::max(0, n)), -1);
    if (s.empty() || scale <= 0) return out;
    size_t k = 0;
    double held = -1;
    const double first = now - n * step;
    while (k < s.points.size() && s.points[k].first <= first) held = s.points[k++].second;
    for (int i = 0; i < n; i++) {
        const double end = first + (i + 1) * step;
        double sum = 0;
        int count = 0;
        while (k < s.points.size() && s.points[k].first <= end) {
            sum += s.points[k++].second;
            count++;
        }
        if (count > 0) held = sum / count;
        if (held >= 0) out[static_cast<size_t>(i)] = std::clamp(held / scale, 0.0, 1.0);
    }
    return out;
}

/* How fast a total grows (CPU seconds, bytes), from readings far enough apart
- sstat counts CPU time in whole seconds and doesn't say when it took it: over 2 s, a second more or less is 50%
  on 1 CPU. The base is a reading at least `window` seconds older: the curve is smooth.
- Less than before (a step ended): it starts again from this reading
*/
struct Rate {
    double window = 6;
    std::vector<std::pair<double, double>> readings;

    // The rate per second, -1 until there are two readings 1.5 s apart
    double add(double at, double total) {
        if (!readings.empty() && total < readings.back().second) readings.clear();
        readings.emplace_back(at, total);
        const std::pair<double, double>* base = nullptr;
        for (const auto& r : readings)
            if (at - r.first >= window || (!base && at - r.first >= 1.5)) base = &r;
        const double rate = base ? (total - base->second) / (at - base->first) : -1;
        while (readings.size() > 2 && at - readings[1].first >= window) readings.erase(readings.begin());
        return rate;
    }
};

// ---------------------------------------------------------------- pieces

struct LivePieces {

    static std::string watts(double w) {
        if (w >= 1000) {
            const long tenths = std::lround(w / 100);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " kW";
        }
        return std::to_string(static_cast<long>(w + 0.5)) + " W";
    }
    static std::string pct(double r) { return r < 0 ? "-" : std::to_string(static_cast<int>(std::lround(r * 100))) + "%"; }
    static std::string shortNode(const std::string& node) {
        const size_t dash = node.rfind('-');
        return dash == std::string::npos ? node : node.substr(dash + 1);
    }

    // A box: its name and what it says now on the top border, between the corners
    /* A box as btop draws them: rounded, its title on the top border between ┐ and ┌ ("╭─┐¹cpu┌───"), the key that
       shows it (`number`) as a red superscript; `right` (what it says now) and `bottom` (its keys) on its borders too */
    static Element tag(Element inner, Color frame, bool down = false) {
        return hbox({text(down ? "┘" : "┐") | color(frame), inner, text(down ? "└" : "┌") | color(frame)});
    }
    static Element keyTag(const std::string& key, const std::string& what, Color frame, bool down = false) {
        return tag(hbox({text(key) | bold | color(chart::hiColor()), text(what.empty() ? "" : " " + what) | color(chart::titleColor())}), frame, down);
    }
    static Element box(const std::string& name, Element right, Element content, Color frame, int width, int height, int number = 0,
                       Elements bottom = {}) {
        Elements title;
        if (number > 0) title.push_back(text(chart::superscript(number)) | bold | color(chart::hiColor()));
        title.push_back(text(name) | bold | color(chart::titleColor()));
        Elements top = {text("") | size(WIDTH, EQUAL, 1), tag(hbox(std::move(title)), frame), filler()};
        if (right) top.push_back(tag(right, frame));
        top.push_back(text("") | size(WIDTH, EQUAL, 1));
        Elements layers = {content | flex | borderStyled(ROUNDED, frame), vbox({hbox(std::move(top)), filler()})};
        if (!bottom.empty()) {
            Elements under = {text("") | size(WIDTH, EQUAL, 1)};
            for (auto& b : bottom) under.push_back(std::move(b));
            under.push_back(filler());
            layers.push_back(vbox({filler(), hbox(std::move(under))}));
        }
        return dbox(std::move(layers)) | clear_under | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    }

    // "CPU  ■■■■■■■■■■■■  91%"
    static Element gauge(const std::string& label, double ratio, int width, const chart::Gradient& g) {
        const int w = std::max(3, width - 5 - 5);
        return hbox({text(util::padRight(label, 5)) | bold, chart::meter(ratio, w, g),
                     text(util::padLeft(pct(ratio), 5)) | bold | color(g.at(std::max(0.0, ratio)))});
    }

    // The scale beside a graph: its top, its middle when there is room, and 0 at the bottom (the top at the bottom
    // for one hanging down, and no 0: it shares the one above it)
    static Element axis(int height, const std::string& top, const std::string& mid = "", bool hanging = false, int width = 6) {
        Elements rows;
        for (int r = 0; r < height; r++) {
            std::string label;
            if (r == (hanging ? height - 1 : 0)) label = top;
            else if (!hanging && r == height - 1 && height > 2) label = "0";
            else if (!mid.empty() && height >= 7 && r == (height - 1) / 2) label = mid;
            rows.push_back(text(util::padLeft(label, static_cast<size_t>(width - 1)) + " ") | color(chart::graphText()));
        }
        return vbox(std::move(rows));
    }

    // A line between two graphs: what the one under it shows, then a faint rule
    static Element rule(Elements what) {
        what.push_back(text(" "));
        what.push_back(separatorLight() | color(chart::faint()) | flex);
        return hbox(std::move(what));
    }


    // "-2m", "now": the time under a chart, `from_right` columns of `step` seconds before now
    static std::string ago(int from_right, double step) {
        const long s = std::lround(from_right * step);
        if (from_right == 0) return "now";
        if (s < 60) return "-" + std::to_string(s) + "s";
        if (s % 60 == 0 || s >= 3600) return "-" + util::duration(s);
        return "-" + std::to_string(s / 60) + "m" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
    }

    // A grid line every round number of seconds, about `across` of them on `cols` columns
    static int tickColumns(int cols, double step, double across = 6) {
        double tick = 10;
        for (double t : {10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600})
            if ((tick = t) >= cols / across * step) break;
        return std::max(4, static_cast<int>(tick / step));
    }

    static double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

}  // namespace ui
