#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../api/slurmjobs.hpp"
#include "../utils/format.hpp"

/* Colors and small drawing helpers shared by the views
- Colors are RGB: FTXUI turns them into the closest 256 or 16 color when the terminal has fewer (see main.cpp)
- Text stays in the terminal's own foreground, so it reads on light and dark themes; the selection is inverted
- Only ASCII and box/block characters: icons from Nerd Fonts or emoji don't have the same width everywhere
*/
namespace ui {
using namespace ftxui;

namespace theme {
inline Color green() { return Color::RGB(90, 185, 105); }
inline Color yellow() { return Color::RGB(222, 165, 50); }
inline Color red() { return Color::RGB(226, 86, 86); }
inline Color blue() { return Color::RGB(80, 145, 235); }
inline Color magenta() { return Color::RGB(190, 110, 220); }
inline Color cyan() { return Color::RGB(60, 175, 195); }
inline Color gray() { return Color::RGB(128, 128, 128); }
inline Color accent() { return Color::RGB(100, 160, 240); }
}  // namespace theme

inline Color stateColor(const std::string& state) {
    if (state == "RUNNING" || state == "COMPLETING" || state == "CONFIGURING") return theme::green();
    if (api::isPending(state)) return theme::yellow();
    if (state == "COMPLETED") return theme::blue();
    if (state == "CANCELLED") return theme::magenta();
    if (api::isFailure(state)) return theme::red();
    return theme::gray();
}

// A key and what it does, for the footer and the views: "l log"
inline Element keyHint(const std::string& key, const std::string& label) {
    return hbox({text(key) | bold | color(theme::accent()), text(" " + label + "  ") | dim});
}

inline Elements keyHints(const std::vector<std::pair<std::string, std::string>>& keys) {
    Elements out;
    for (const auto& [k, l] : keys) out.push_back(keyHint(k, l));
    return out;
}

// "Label   value", with labels of the same width
inline Element field(const std::string& label, Element value, int label_width = 10) {
    return hbox({text(util::padRight(label, label_width)) | dim, value});
}

// A bar `width` columns wide, filled to `ratio` (0..1), with 1/8 steps
inline Element bar(double ratio, int width, Color fill) {
    if (width <= 0) return text("");
    ratio = std::clamp(ratio, 0.0, 1.0);
    static const char* partial[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    const int eighths = static_cast<int>(ratio * width * 8 + 0.5);
    const int full = eighths / 8;
    std::string filled;
    for (int i = 0; i < full; i++) filled += "█";
    if (full < width) filled += partial[eighths % 8];
    const int used = full + (full < width && eighths % 8 ? 1 : 0);
    std::string empty;
    for (int i = used; i < width; i++) empty += "░";
    return hbox({text(filled) | color(fill), text(empty) | color(theme::gray()) | dim});
}

// Color of a usage ratio: green, then yellow, red when almost full
inline Color usageColor(double ratio) {
    return ratio >= 0.9 ? theme::red() : ratio >= 0.7 ? theme::yellow() : theme::green();
}

// Color of an efficiency (CPU or memory used / allocated): red when little of it was used
inline Color efficiencyColor(double ratio) {
    return ratio < 0.3 ? theme::red() : ratio < 0.7 ? theme::yellow() : theme::green();
}

inline std::string percent(double ratio) {
    if (ratio < 0) return "-";
    return std::to_string(static_cast<int>(ratio * 100 + 0.5)) + "%";
}

/* A list of lines seen through a window of `height` lines starting at `offset`
- Only the visible lines are drawn (a log can have thousands)
- A scroll bar on the right when everything doesn't fit
*/
inline Element scrollWindow(const Elements& all, int offset, int height) {
    height = std::max(1, height);
    const int total = static_cast<int>(all.size());
    offset = std::clamp(offset, 0, std::max(0, total - height));
    Elements visible;
    for (int i = offset; i < std::min(total, offset + height); i++) visible.push_back(all[static_cast<size_t>(i)]);
    while (static_cast<int>(visible.size()) < height) visible.push_back(text(""));
    Element content = vbox(std::move(visible)) | flex;
    if (total <= height) return content;

    // Scroll bar: the thumb's size and position follow the window
    const int thumb = std::max(1, height * height / total);
    const int pos = total - height > 0 ? (height - thumb) * offset / (total - height) : 0;
    Elements track;
    for (int i = 0; i < height; i++)
        track.push_back(i >= pos && i < pos + thumb ? text("┃") | color(theme::gray()) : text("│") | dim);
    return hbox({content, vbox(std::move(track))});
}

/* A framed box with a title in its top line, drawn over what is behind it
- Only the frame and the title take the color (FTXUI's window() has none, color() would tint the content)
- The title line is put in a vbox: in the dbox alone, it would get the whole height, and its bold and color
  with it, on every line of the content
*/
inline Element panel(const std::string& title, Element content, Color frame = theme::accent(), Element right = nullptr) {
    Element framed = std::move(content) | borderStyled(ROUNDED, frame);
    Elements top = {text("") | size(WIDTH, EQUAL, 1), text(" " + title + " ") | bold | color(frame), filler()};
    if (right) top.push_back(hbox({text(" "), right, text(" ")}) | color(frame));
    top.push_back(text("") | size(WIDTH, EQUAL, 1));
    return dbox({framed, vbox({hbox(std::move(top)), filler()})}) | clear_under;
}

}  // namespace ui
