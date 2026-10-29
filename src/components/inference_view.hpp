#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "../api/inference.hpp"
#include "charts.hpp"
#include "live.hpp"
#include "theme.hpp"

namespace ui {
using namespace ftxui;

// ---------------------------------------------------------------- figures

namespace fmt {

inline std::string num(double v, const char* format) {
    char buf[32];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

// 173, 2.2k, 77M: counts and rates of tokens
inline std::string count(double v) {
    if (v < 0) return "-";
    if (v < 10 && v != std::floor(v)) return num(v, "%.1f");
    if (v < 1000) return num(v, "%.0f");
    if (v < 1e4) return num(v / 1e3, "%.2fk");
    if (v < 1e6) return num(v / 1e3, "%.0fk");
    if (v < 1e7) return num(v / 1e6, "%.2fM");
    if (v < 1e9) return num(v / 1e6, "%.0fM");
    return num(v / 1e9, "%.1fG");
}

// 0.0138 -> "14 ms", 0.58 -> "580 ms", 6.2 -> "6.2 s", 95 -> "1m35"
inline std::string seconds(double s) {
    if (s < 0) return "-";
    if (s == 0) return "0";
    if (s < 0.001) return num(s * 1e6, "%.0f µs");
    if (s < 1) return num(s * 1e3, s < 0.01 ? "%.1f ms" : "%.0f ms");
    if (s < 10) return num(s, "%.2f s");
    if (s < 60) return num(s, "%.1f s");
    return util::duration(static_cast<long>(s + 0.5));
}

inline std::string percent(double r) { return r < 0 ? "-" : r < 0.1 && r > 0 ? num(r * 100, "%.1f%%") : num(r * 100, "%.0f%%"); }

}  // namespace fmt

// ---------------------------------------------------------------- one server, in time

/* A server's readings turned into curves
- Rates from two readings in a row; latencies, cache hits and accepted drafts over the last 30 s (their histograms
  and counters then minus 30 s before): a curve that moves with what the server does now, not since it started
- Five minutes of readings are kept for that and for the figures over the last minute
*/
struct InferenceServer {
    api::Endpoint where;
    api::InferenceReading last;
    struct Kept {
        double at;
        api::InferenceReading r;
    };
    std::vector<Kept> kept;
    double seen = 0, first_at = 0, last_active = 0, preempted_at = 0, failed_at = 0;
    std::string error;

    Series generation, prompt, running, waiting, kv, prefix_hit, requests, ttft50, ttft95, itl50, itl95, e2e50, e2e95, accepted;

    // The reading at least `window` seconds before `at`, else the oldest kept (null: none)
    const api::InferenceReading* before(double at, double window) const {
        const api::InferenceReading* r = nullptr;
        for (const auto& k : kept) {
            if (at - k.at < window && r) break;
            r = &k.r;
        }
        return r;
    }
    void add(const api::InferenceReading& r, double at) {
        error.clear();
        const auto delta = [](double now, double then) { return now >= 0 && then >= 0 && now >= then ? now - then : -1; };
        double gen_rate = -1;
        if (!kept.empty()) {
            const auto& prev = kept.back();
            const double dt = at - prev.at;
            if (dt > 0.2) {
                const auto rate = [&](double now, double then) {
                    const double d = delta(now, then);
                    return d < 0 ? -1 : d / dt;
                };
                gen_rate = rate(r.generation_tokens, prev.r.generation_tokens);
                double p = rate(r.prompt_tokens, prev.r.prompt_tokens);
                if (gen_rate < 0) gen_rate = r.generation_tps;
                if (p < 0) p = r.prompt_tps;
                if (gen_rate >= 0) generation.add(at, gen_rate);
                if (p >= 0) prompt.add(at, p);
                const double q = rate(r.requests_done, prev.r.requests_done);
                if (q >= 0) requests.add(at, q * 60);
                if (r.preemptions > prev.r.preemptions && prev.r.preemptions >= 0) preempted_at = at;
                if (r.requests_failed > prev.r.requests_failed && prev.r.requests_failed >= 0) failed_at = at;
            }
        } else if (r.generation_tps >= 0) {
            gen_rate = r.generation_tps;
            generation.add(at, gen_rate);
        }
        if (r.running >= 0) running.add(at, r.running);
        if (r.waiting >= 0) waiting.add(at, r.waiting);
        if (r.kv_usage >= 0) kv.add(at, std::min(1.0, r.kv_usage));

        // Over the last 30 s
        if (const auto* b = before(at, 30)) {
            const double dq = delta(r.prefix_queries, b->prefix_queries), dh = delta(r.prefix_hits, b->prefix_hits);
            if (dq > 0 && dh >= 0) prefix_hit.add(at, std::min(1.0, dh / dq));
            const auto quantiles = [&](const api::Histogram& now, const api::Histogram& then, Series& p50, Series& p95) {
                const api::Histogram d = now.since(then);
                if (d.count <= 0) return;
                p50.add(at, d.quantile(0.5));
                p95.add(at, d.quantile(0.95));
            };
            quantiles(r.ttft, b->ttft, ttft50, ttft95);
            quantiles(r.itl, b->itl, itl50, itl95);
            quantiles(r.e2e, b->e2e, e2e50, e2e95);
            const double dd = delta(r.draft_tokens, b->draft_tokens), da = delta(r.accepted_tokens, b->accepted_tokens);
            if (dd > 0 && da >= 0) accepted.add(at, std::min(1.0, da / dd));
        }
        if (prefix_hit.empty() && r.prefix_hit_rate >= 0) prefix_hit.add(at, std::min(1.0, r.prefix_hit_rate));
        else if (r.prefix_queries < 0 && r.prefix_hit_rate >= 0) prefix_hit.add(at, std::min(1.0, r.prefix_hit_rate));
        if (r.running > 0 || gen_rate > 0 || first_at == 0) last_active = at;

        kept.push_back({at, r});
        while (kept.size() > 2 && at - kept[1].at >= 300) kept.erase(kept.begin());
        last = r;
        seen = at;
        if (first_at == 0) first_at = at;
    }

    // What changed over the last `window` seconds (or since it was first read)
    struct Window {
        double seconds = 0;
        api::Histogram ttft, itl, e2e, queue, prefill, decode, prompt_length, output_length;
        double done = -1, failed = -1, prompt_tokens = -1, generation_tokens = -1, prefix_queries = -1, prefix_hits = -1;
        double draft_tokens = -1, accepted_tokens = -1, drafts = -1, preemptions = -1;
        std::vector<double> per_position;
        std::map<std::string, double> finished;
    };
    Window window(double at, double seconds) const {
        Window w;
        const auto* b = before(at, seconds);
        if (!b) return w;
        double since = at;
        for (const auto& k : kept)
            if (&k.r == b) since = k.at;
        w.seconds = at - since;
        const auto d = [](double now, double then) { return now >= 0 && then >= 0 && now >= then ? now - then : -1; };
        w.ttft = last.ttft.since(b->ttft);
        w.itl = last.itl.since(b->itl);
        w.e2e = last.e2e.since(b->e2e);
        w.queue = last.queue.since(b->queue);
        w.prefill = last.prefill.since(b->prefill);
        w.decode = last.decode.since(b->decode);
        w.prompt_length = last.prompt_length.since(b->prompt_length);
        w.output_length = last.output_length.since(b->output_length);
        w.done = d(last.requests_done, b->requests_done);
        w.failed = d(last.requests_failed, b->requests_failed);
        w.prompt_tokens = d(last.prompt_tokens, b->prompt_tokens);
        w.generation_tokens = d(last.generation_tokens, b->generation_tokens);
        w.prefix_queries = d(last.prefix_queries, b->prefix_queries);
        w.prefix_hits = d(last.prefix_hits, b->prefix_hits);
        w.draft_tokens = d(last.draft_tokens, b->draft_tokens);
        w.accepted_tokens = d(last.accepted_tokens, b->accepted_tokens);
        w.drafts = d(last.drafts, b->drafts);
        w.preemptions = d(last.preemptions, b->preemptions);
        for (size_t i = 0; i < last.accepted_per_position.size(); i++)
            w.per_position.push_back(last.accepted_per_position[i] - (i < b->accepted_per_position.size() ? b->accepted_per_position[i] : 0));
        for (const auto& [reason, n] : last.finished) {
            const auto it = b->finished.find(reason);
            w.finished[reason] = n - (it == b->finished.end() ? 0 : it->second);
        }
        return w;
    }

    // Held for `seconds`: every reading of `s` over that time above `above`
    static bool held(const Series& s, double at, double seconds, double above) {
        bool any = false;
        for (auto it = s.points.rbegin(); it != s.points.rend() && at - it->first <= seconds; ++it) {
            if (it->second <= above) return false;
            any = true;
        }
        return any && !s.points.empty() && at - s.points.front().first >= seconds;
    }
};

// ---------------------------------------------------------------- the servers of a job

struct InferenceView : LivePieces {
    bool searching = false;
    double searched_at = 0;
    int searches = 0;
    api::Discovery found;
    std::vector<InferenceServer> servers;
    int selected = 0;
    bool scraping = false;
    double scraped_at = 0;

    bool any() const { return !servers.empty(); }
    bool hinted() const { return found.hint.engine != api::Engine::None; }
    api::Engine engine() const { return any() ? servers.front().last.engine : found.hint.engine; }

    // Look again while nothing answers: often when the job runs one (its model may still be loading), seldom if not
    bool wantSearch(double now, bool shown) const {
        if (searching || any()) return false;
        if (searched_at == 0) return true;
        return now - searched_at > (hinted() || shown ? 15 : 120);
    }

    void setDiscovery(api::Discovery d, double at) {
        searching = false;
        searched_at = at;
        searches++;
        for (size_t i = 0; i < d.servers.size(); i++) {
            bool known = false;
            for (const auto& s : servers) known = known || s.where == d.servers[i];
            if (known) continue;
            InferenceServer s;
            s.where = d.servers[i];
            s.add(d.first[i], at);
            servers.push_back(std::move(s));
        }
        found = std::move(d);
    }

    void addReading(size_t i, const api::Scraped& s, double at) {
        if (i >= servers.size()) return;
        if (s.error.empty()) servers[i].add(s.reading, at);
        else servers[i].error = s.error;
    }

    // ---------------------------------------------------------------- what it says

    std::vector<std::pair<std::string, Color>> warnings(const InferenceServer& s, double now, double gpu_busy) const {
        std::vector<std::pair<std::string, Color>> w;
        const auto& r = s.last;
        const Color info = Color::RGB(0x74, 0xc7, 0xec);
        if (!s.error.empty() && now - s.seen > 10)
            w.push_back({"no answer for " + util::duration(static_cast<long>(now - s.seen)) + ": " + s.error, theme::red()});
        if (s.preempted_at > 0 && now - s.preempted_at < 120)
            w.push_back({"requests preempted " + util::duration(static_cast<long>(now - s.preempted_at)) +
                             " ago: the KV cache was full, their work is done again",
                         theme::red()});
        if (InferenceServer::held(s.waiting, now, 10, 0)) {
            std::string why;
            for (const auto& [reason, n] : r.waiting_by_reason)
                if (n > 0) why += (why.empty() ? " (" : ", ") + reason;
            if (!why.empty()) why += ")";
            w.push_back({fmt::count(r.waiting) + " requests wait" + why + " for 10 s or more: the server is at its limit", theme::yellow()});
        }
        if (r.kv_usage > 0.9) w.push_back({"KV cache " + fmt::percent(r.kv_usage) + " full: more requests at once will wait or be preempted", theme::yellow()});
        if (s.failed_at > 0 && now - s.failed_at < 120) w.push_back({"requests aborted or failed in the last 2 minutes", theme::yellow()});
        if (gpu_busy > 0.5 && (r.running <= 0) && s.generation.last() <= 0)
            w.push_back({"its GPUs are busy (" + fmt::percent(gpu_busy) + ") but it makes no token: loading, compiling, or another program", theme::yellow()});
        const double accepted = s.accepted.last();
        if (!s.accepted.empty() && accepted < 0.3)
            w.push_back({"speculative decoding: only " + fmt::percent(accepted) + " of the drafted tokens accepted, it may slow decoding down",
                         theme::yellow()});
        if (now - s.first_at > 120 && !s.kv.empty() && s.kv.peak(now - 300) < 0.05 && s.waiting.peak(now - 300) <= 0 && s.running.peak(now - 300) > 0)
            w.push_back({"KV cache under 5% for minutes, never a queue: it could serve many more requests at once (or run on fewer GPUs)", info});
        if (now - s.last_active > 300) w.push_back({"no request for " + util::duration(static_cast<long>(now - s.last_active)), info});
        const auto win = s.window(now, 60);
        if (s.prefix_hit.last() > 0.8 && win.prompt_tokens > 20 * std::max(1.0, win.generation_tokens))
            w.push_back({fmt::percent(s.prefix_hit.last()) + " of the prompt tokens come from the prefix cache: long shared contexts (an agent?)", info});
        return w;
    }

    // ---------------------------------------------------------------- drawing

    static Color engineColor(api::Engine e) {
        switch (e) {
            case api::Engine::Vllm: return Color::RGB(0x30, 0x8c, 0xf0);
            case api::Engine::Sglang: return Color::RGB(0xe0, 0x6c, 0x3a);
            case api::Engine::Tgi: return Color::RGB(0xf5, 0xc5, 0x3d);
            case api::Engine::LlamaCpp: return Color::RGB(0x9b, 0x7c, 0xe8);
            case api::Engine::Triton: return Color::RGB(0x76, 0xb9, 0x00);
            default: return Color::RGB(0xa0, 0xa0, 0xa0);
        }
    }

    // A figure in a frame: its name on the border, its value, its curve under it
    static Element tile(const std::string& name, const std::string& value, const std::string& note, const std::vector<double>& curve,
                        const chart::Gradient& g, int width) {
        const int w = std::max(6, width - 2);
        Element top = hbox({text(value) | bold | color(g.high), filler(), text(note) | color(chart::graphText())});
        return box(name, nullptr, vbox({top, chart::graph(curve, w, 2, g, false, chart::Symbols::Bars, true)}), chart::faint(), width, 5);
    }

    static std::string repeatStr(const std::string& s, int n) {
        std::string out;
        for (int i = 0; i < n; i++) out += s;
        return out;
    }

    // Segments of one bar, each its share of `width` in its colour
    static Element stacked(const std::vector<std::pair<double, Color>>& parts, int width) {
        double total = 0;
        for (const auto& p : parts) total += std::max(0.0, p.first);
        if (total <= 0) return text(std::string(static_cast<size_t>(width), ' '));
        Elements cells;
        int used = 0;
        for (size_t i = 0; i < parts.size(); i++) {
            int n = i + 1 == parts.size() ? width - used : static_cast<int>(std::lround(std::max(0.0, parts[i].first) / total * width));
            n = std::clamp(n, 0, width - used);
            if (n > 0) cells.push_back(text(repeatStr("█", n)) | color(parts[i].second));
            used += n;
        }
        return hbox(std::move(cells));
    }

    // A bar for a share: "■■■■■□□□ 61%"
    static Element shareBar(double r, int width, const chart::Gradient& g) {
        return hbox({chart::meter(r, std::max(3, width - 6), g), text(util::padLeft(fmt::percent(r), 6)) | bold | color(g.at(std::max(0.0, r)))});
    }

    Element searchingView(int width, const std::string& job_id) const {
        Elements lines = {text(""), text("  Looking for a server of models in job " + job_id + "...") | bold,
                          text("  its output, its errors and its script, then GET /metrics on its nodes") | color(chart::graphText())};
        return vbox(std::move(lines)) | size(WIDTH, EQUAL, width);
    }

    Element notFoundView(int width, double now) const {
        Elements lines = {text("")};
        const std::string engine = api::engineName(found.hint.engine);
        if (hinted()) {
            lines.push_back(hbox({text("  "), text(" " + engine + " ") | bold | inverted | color(engineColor(found.hint.engine)),
                                  text("  in the job, but no server answers yet: the model may still be loading") | bold}));
            if (!found.hint.evidence.empty())
                lines.push_back(text("  " + util::cut(found.hint.evidence, static_cast<size_t>(std::max(10, width - 4)))) | color(chart::graphText()));
        } else {
            lines.push_back(text("  No server of models found in this job") | bold);
            lines.push_back(text("  RSV knows vLLM, SGLang, TGI, llama.cpp, Triton and Ollama: it reads the job's output, errors and script for") |
                            color(chart::graphText()));
            lines.push_back(text("  the server and its port, then asks it for its metrics (GET /metrics on the job's nodes)") | color(chart::graphText()));
        }
        lines.push_back(text(""));
        if (!found.looked_in.empty()) {
            lines.push_back(text("  read") | color(chart::graphText()));
            for (const auto& f : found.looked_in) lines.push_back(text("    " + util::cutMiddle(f, static_cast<size_t>(std::max(10, width - 6)))));
        }
        if (!found.tried.empty()) {
            lines.push_back(text("  asked") | color(chart::graphText()));
            for (const auto& t : found.tried) lines.push_back(text("    " + util::cut(t, static_cast<size_t>(std::max(10, width - 6)))));
        }
        lines.push_back(text(""));
        lines.push_back(text("  looked " + std::to_string(searches) + (searches == 1 ? " time" : " times") +
                             (searched_at > 0 ? ", last " + util::duration(static_cast<long>(now - searched_at)) + " ago" : "") +
                             " · again every " + (hinted() ? "15 s" : "2 min") + " · r: now") |
                        color(chart::graphText()));
        lines.push_back(text("  another server, or another port: RSV_METRICS_URL=http://{node}:PORT/metrics rsv") | color(chart::graphText()));
        return vbox(std::move(lines)) | size(WIDTH, EQUAL, width);
    }

    // Ollama has no metrics: its models in memory
    Element ollamaView(const InferenceServer& s, int width) const {
        Elements lines = {text(""), text("  Ollama gives no metrics: the models it keeps in memory") | color(chart::graphText()), text("")};
        for (const auto& [name, vram] : s.last.loaded)
            lines.push_back(hbox({text("  " + util::padRight(util::cut(name, 40), 42)) | bold, text(chart::bytes(vram) + " on the GPUs")}));
        if (s.last.loaded.empty()) lines.push_back(text("  none loaded now") | dim);
        return vbox(std::move(lines)) | size(WIDTH, EQUAL, width);
    }

    Element render(int width, int height, double now, double step, const Series* gpu_busy, int gpus, const std::string& job_id) {
        if (!any()) return searches == 0 ? searchingView(width, job_id) : notFoundView(width, now);
        selected = std::clamp(selected, 0, static_cast<int>(servers.size()) - 1);
        const InferenceServer& s = servers[static_cast<size_t>(selected)];
        if (s.last.engine == api::Engine::Ollama) return ollamaView(s, width);
        const auto& r = s.last;

        // Head: the engine, the model, where; what it did since it started
        Elements head = {text(" " + api::engineName(r.engine) + " ") | bold | inverted | color(engineColor(r.engine)), text(" "),
                         text(util::cut(r.model.empty() ? "?" : r.model, static_cast<size_t>(std::max(10, width / 3)))) | bold,
                         text("  " + s.where.where()) | color(chart::graphText()), filler()};
        if (r.requests_done >= 0) head.push_back(text(fmt::count(r.requests_done) + " requests") | color(chart::graphText()));
        if (r.prompt_tokens >= 0)
            head.push_back(text(" · " + fmt::count(r.prompt_tokens) + " prompt · " + fmt::count(r.generation_tokens) + " generated tokens") |
                           color(chart::graphText()));
        Elements out = {hbox(std::move(head))};
        if (servers.size() > 1) {
            Elements strip = {text("servers ") | color(chart::graphText())};
            for (size_t i = 0; i < servers.size(); i++) {
                const auto& o = servers[i];
                Element e = text(" " + o.where.where() + " " + fmt::count(o.generation.last()) + " tok/s ");
                strip.push_back(static_cast<int>(i) == selected ? e | bold | inverted | color(engineColor(o.last.engine)) : e | color(chart::graphText()));
            }
            strip.push_back(text("  ↑↓") | color(chart::graphText()));
            out.push_back(hbox(std::move(strip)));
        }
        auto warn = warnings(s, now, gpu_busy ? gpu_busy->last() : -1);
        if (warn.size() > 3) warn.resize(3);
        for (const auto& [msg, c] : warn) out.push_back(text("! " + util::cut(msg, static_cast<size_t>(width - 2))) | color(c));

        // Tiles: the figures that matter, each with its curve
        const auto win = s.window(now, 60);
        struct T {
            std::string name, value, note;
            const Series* series;
            double scale;
            chart::Gradient g;
        };
        const double gen_top = chart::niceMax(std::max(1.0, s.generation.peak(now - 600)));
        const double prompt_top = chart::niceMax(std::max(1.0, s.prompt.peak(now - 600)));
        const double req_top = chart::niceMax(std::max({1.0, s.running.peak(now - 600), s.waiting.peak(now - 600)}));
        const double ttft_top = chart::niceMax(std::max(0.01, s.ttft95.peak(now - 600)));
        const double itl_top = chart::niceMax(std::max(0.001, s.itl95.peak(now - 600)));
        std::vector<T> tiles = {
            {"generation", fmt::count(s.generation.last()) + " tok/s", "", &s.generation, gen_top, chart::cachedGradient()},
            {"prompt", fmt::count(s.prompt.last()) + " tok/s", "", &s.prompt, prompt_top, chart::readGradient()},
            {"requests", fmt::count(r.running) + " running", fmt::count(std::max(0.0, r.waiting)) + " wait", &s.running, req_top, chart::availableGradient()}};
        if (r.kv_usage >= 0) tiles.push_back({"KV cache", fmt::percent(r.kv_usage), "", &s.kv, 1, chart::usedGradient()});
        if (!s.prefix_hit.empty()) tiles.push_back({"prefix cache", fmt::percent(s.prefix_hit.last()), "hits", &s.prefix_hit, 1, chart::freeGradient()});
        if (!s.ttft50.empty()) tiles.push_back({"first token", fmt::seconds(s.ttft50.last()), "p50", &s.ttft50, ttft_top, chart::tempGradient()});
        if (!s.itl50.empty()) {
            const double itl = s.itl50.last();
            tiles.push_back({"per token", fmt::seconds(itl), itl > 0 ? fmt::count(1 / itl) + "/s" : "", &s.itl50, itl_top, chart::writeGradient()});
        }
        const int per_row = std::clamp(width / 22, 2, static_cast<int>(tiles.size()));
        Elements tile_row;
        int used_w = 0;
        for (int i = 0; i < per_row; i++) {
            const int tw = i + 1 == per_row ? width - used_w : width / per_row;
            const auto& t = tiles[static_cast<size_t>(i)];
            tile_row.push_back(tile(t.name, t.value, t.note, resample(*t.series, now, step * 60 / std::max(4, tw - 2), tw - 2, t.scale), t.g, tw));
            used_w += tw;
        }
        out.push_back(hbox(std::move(tile_row)));

        // The charts, then the boxes under them: they share what is left
        const int left_h = height - static_cast<int>(out.size());
        const bool bottom = left_h >= 24;
        const int charts_h = bottom ? std::clamp(left_h * 45 / 100, 9, 18) : left_h;
        const int bottom_h = left_h - charts_h;
        const bool two = width >= 110;
        const int cw = two ? (width - 3) / 2 : width;
        const auto chartOf = [&](const std::string& what, std::vector<std::tuple<std::string, const Series*, Color>> curves, double top,
                                 std::function<std::string(double)> y_label, int w, int h) {
            const int cols = chart::lineColumns(w, h - 1, y_label);
            std::vector<chart::Line> lines;
            Elements legend = {text(what) | bold, text("   ")};
            for (const auto& [name, series, c] : curves) {
                lines.push_back({resample(*series, now, step / 2, cols * 2, top), c, lines.empty()});
                legend.push_back(text("━ ") | bold | color(c));
                legend.push_back(text(name + "  ") | color(chart::graphText()));
            }
            return vbox({hbox(std::move(legend)), chart::lineChart(lines, w, h - 1, y_label, [step](int c) { return ago(c, step); },
                                                                   tickColumns(cols, step, w >= 100 ? 6 : 4))}) |
                   size(WIDTH, EQUAL, w);
        };
        const double tput_top = chart::niceMax(std::max({1.0, s.generation.peak(now - step * cw), s.prompt.peak(now - step * cw)}));
        Element tput = chartOf("tokens per second", {{"generated", &s.generation, chart::cachedGradient().mid}, {"prompt", &s.prompt, chart::readGradient().high}},
                               tput_top, [tput_top](double f) { return fmt::count(f * tput_top); }, cw, charts_h);
        Element second;
        if (!s.ttft50.empty()) {
            const double top = chart::niceMax(std::max({0.01, s.ttft95.peak(now - step * cw), s.ttft50.peak(now - step * cw)}));
            second = chartOf("time to first token", {{"p50", &s.ttft50, chart::tempGradient().high}, {"p95", &s.ttft95, chart::availableGradient().mid}}, top,
                             [top](double f) { return fmt::seconds(f * top); }, cw, charts_h);
        } else {
            second = chartOf("requests", {{"running", &s.running, chart::availableGradient().mid}, {"waiting", &s.waiting, theme::red()}}, req_top,
                             [req_top](double f) { return fmt::count(f * req_top); }, cw, charts_h);
        }
        out.push_back(two ? hbox({tput, text(" "), separatorLight() | color(chart::faint()), text(" "), second}) : tput);
        if (bottom_h >= 8) {
            const int bw1 = width * 36 / 100, bw2 = width * 32 / 100, bw3 = width - bw1 - bw2;
            out.push_back(hbox({latencyBox(s, win, bw1, bottom_h), cacheBox(s, win, now, bw2, bottom_h), requestsBox(s, win, now, gpu_busy, gpus, bw3, bottom_h)}));
        }
        return vbox(std::move(out)) | size(WIDTH, EQUAL, width);
    }

    // Latencies over the last minute: p50, p95, mean; where a request's time goes; how long the requests took
    Element latencyBox(const InferenceServer& s, const InferenceServer::Window& win, int width, int height) const {
        const int w = width - 2;
        const auto row = [&](const std::string& name, const api::Histogram& h, Color c) {
            return hbox({text(util::padRight(name, 12)) | color(c), text(util::padLeft(fmt::seconds(h.quantile(0.5)), 9)),
                         text(util::padLeft(fmt::seconds(h.quantile(0.95)), 9)), text(util::padLeft(fmt::seconds(h.mean()), 9)) | color(chart::graphText())});
        };
        Elements lines = {hbox({text(util::padRight("", 12)), text(util::padLeft("p50", 9)), text(util::padLeft("p95", 9)), text(util::padLeft("mean", 9))}) |
                          color(chart::graphText())};
        const std::vector<std::tuple<std::string, const api::Histogram*, Color>> rows = {
            {"first token", &win.ttft, chart::tempGradient().high}, {"per token", &win.itl, chart::writeGradient().high},
            {"request", &win.e2e, chart::cachedGradient().mid},      {"queue", &win.queue, chart::availableGradient().mid},
            {"prefill", &win.prefill, chart::readGradient().high},   {"decode", &win.decode, chart::freeGradient().mid}};
        for (const auto& [name, h, c] : rows)
            if (!h->empty() || h->count > 0) lines.push_back(row(name, *h, c));
        if (lines.size() == 1) lines.push_back(text("no request ended in the last minute") | color(chart::graphText()));
        // Where the time goes: queue, prefill, decode, summed over the requests
        const double q = win.queue.sum, p = win.prefill.sum, d = win.decode.sum;
        if (q + p + d > 0) {
            lines.push_back(text(""));
            lines.push_back(text("where a request's time goes") | color(chart::graphText()));
            lines.push_back(stacked({{q, chart::availableGradient().mid}, {p, chart::readGradient().high}, {d, chart::freeGradient().mid}}, w));
            const double t = q + p + d;
            lines.push_back(hbox({text("queue " + fmt::percent(q / t)) | color(chart::availableGradient().mid), text(" · "),
                                  text("prefill " + fmt::percent(p / t)) | color(chart::readGradient().high), text(" · "),
                                  text("decode " + fmt::percent(d / t)) | color(chart::freeGradient().mid)}));
        }
        // How long the requests took: one bar per bucket that has some
        const api::Histogram& h = !win.e2e.buckets.empty() ? win.e2e : s.last.e2e;
        const int room = height - 2 - static_cast<int>(lines.size()) - 2;
        if (room >= 3 && !h.buckets.empty()) {
            std::vector<std::pair<std::string, double>> bars;
            double below = 0, most = 0;
            double lower = 0;
            for (const auto& [le, n] : h.buckets) {
                const double here = n - below;
                below = n;
                if (here > 0) {
                    bars.push_back({std::isinf(le) ? "> " + fmt::seconds(lower) : "≤ " + fmt::seconds(le), here});
                    most = std::max(most, here);
                }
                if (!std::isinf(le)) lower = le;
            }
            if (static_cast<int>(bars.size()) > room) bars.erase(bars.begin(), bars.end() - room);
            if (!bars.empty()) {
                lines.push_back(text(""));
                lines.push_back(text(&h == &win.e2e ? "requests by how long they took, last minute" : "requests by how long they took, since it started") |
                                color(chart::graphText()));
                for (const auto& [label, n] : bars) {
                    const int bw = std::max(1, w - 10 - 7);
                    const int fill = static_cast<int>(std::lround(n / most * bw));
                    lines.push_back(hbox({text(util::padLeft(label, 9) + " ") | color(chart::graphText()),
                                          text(repeatStr("▆", std::max(1, fill))) | color(chart::cachedGradient().at(n / most)),
                                          filler(), text(util::padLeft(fmt::count(n), 6))}));
                }
            }
        }
        const std::string span = win.seconds > 0 ? "last " + util::duration(static_cast<long>(win.seconds + 0.5)) : "";
        return box("latency", text(span) | color(chart::graphText()), vbox(std::move(lines)), chart::tempGradient().mid, width, height);
    }

    // The caches and the drafts: how full, how often they serve, how many drafted tokens are kept
    Element cacheBox(const InferenceServer& s, const InferenceServer::Window& win, double now, int width, int height) const {
        const int w = width - 2;
        const auto& r = s.last;
        const auto label = [](const std::string& l) { return text(util::padRight(l, 15)) | color(chart::graphText()); };
        Elements lines;
        if (r.kv_usage >= 0) {
            lines.push_back(hbox({label("KV cache"), shareBar(r.kv_usage, w - 15, chart::usedGradient())}));
            lines.push_back(hbox({label(""), text("peak " + fmt::percent(s.kv.peak(now - 300)) + " over 5 min") | color(chart::graphText())}));
        }
        const double hit = win.prefix_queries > 0 ? win.prefix_hits / win.prefix_queries : s.prefix_hit.last();
        if (hit >= 0) {
            lines.push_back(hbox({label("prefix cache"), shareBar(hit, w - 15, chart::freeGradient())}));
            lines.push_back(hbox({label(""), text(win.prefix_queries > 0 ? fmt::count(win.prefix_hits) + " of " + fmt::count(win.prefix_queries) + " prompt tokens"
                                                                          : "of the prompt tokens") |
                                                 color(chart::graphText())}));
        }
        if (r.preemptions >= 0) {
            const bool recent = s.preempted_at > 0 && now - s.preempted_at < 300;
            lines.push_back(hbox({label("preemptions"), text(fmt::count(r.preemptions)) | bold | color(recent ? theme::red() : Color::Default),
                                  text(recent ? "  some in the last 5 min" : "  none lately") | color(chart::graphText())}));
        }
        // Speculative decoding: the share of drafted tokens kept, and at each position of a draft
        const double acc = win.draft_tokens > 0 ? win.accepted_tokens / win.draft_tokens : s.accepted.last();
        if (acc >= 0 || r.accept_length >= 0) {
            lines.push_back(text(""));
            if (acc >= 0) lines.push_back(hbox({label("drafts kept"), shareBar(acc, w - 15, chart::availableGradient())}));
            const double per_step = win.drafts > 0 ? 1 + win.accepted_tokens / win.drafts : r.accept_length;
            if (per_step > 0) lines.push_back(hbox({label(""), text(fmt::num(per_step, "%.2f") + " tokens per step") | color(chart::graphText())}));
            if (win.drafts > 0)
                for (size_t i = 0; i < win.per_position.size() && static_cast<int>(lines.size()) < height - 2; i++)
                    lines.push_back(hbox({label("  position " + std::to_string(i + 1)),
                                          shareBar(win.per_position[i] / win.drafts, w - 15, chart::availableGradient())}));
        }
        if (lines.empty()) lines.push_back(text("its engine says nothing of its caches") | color(chart::graphText()));
        return box("cache & drafts", nullptr, vbox(std::move(lines)), chart::usedGradient().mid, width, height);
    }

    // The requests: how many ended and how, how long they are; the GPUs beside the tokens
    Element requestsBox(const InferenceServer& s, const InferenceServer::Window& win, double now, const Series* gpu_busy, int gpus, int width,
                        int height) const {
        const int w = width - 2;
        const auto& r = s.last;
        const auto line = [](const std::string& l, const std::string& v) {
            return hbox({text(util::padRight(l, 14)) | color(chart::graphText()), text(v) | bold});
        };
        Elements lines;
        if (r.requests_done >= 0) {
            std::string how;
            for (const auto& [reason, n] : r.finished)
                if (n > 0) how += (how.empty() ? "" : " · ") + reason + " " + fmt::count(n);
            lines.push_back(line("ended", fmt::count(r.requests_done)));
            if (!how.empty()) lines.push_back(hbox({text(util::padRight("", 14)), text(util::cut(how, static_cast<size_t>(std::max(4, w - 14)))) | color(chart::graphText())}));
            lines.push_back(line("per minute", s.requests.empty() ? "-" : fmt::count(win.done > 0 && win.seconds > 0 ? win.done / win.seconds * 60 : s.requests.last())));
        }
        if (!win.prompt_length.empty()) lines.push_back(line("prompt", fmt::count(win.prompt_length.mean()) + " tokens on average"));
        else if (!r.prompt_length.empty()) lines.push_back(line("prompt", fmt::count(r.prompt_length.mean()) + " tokens on average"));
        if (!win.output_length.empty()) lines.push_back(line("answer", fmt::count(win.output_length.mean()) + " tokens on average"));
        else if (!r.output_length.empty()) lines.push_back(line("answer", fmt::count(r.output_length.mean()) + " tokens on average"));
        // The GPUs and the tokens on the same time: does one follow the other
        if (gpu_busy && !gpu_busy->empty()) {
            const int sw = std::max(6, w - 14 - 8);
            const double span = 300.0 / sw;
            const double top = chart::niceMax(std::max(1.0, s.generation.peak(now - 300)));
            lines.push_back(text(""));
            lines.push_back(text("the last 5 minutes") | color(chart::graphText()));
            lines.push_back(hbox({text(util::padRight("GPUs busy", 14)) | color(chart::graphText()), chart::sparkline(resample(*gpu_busy, now, span, sw, 1), sw, chart::gpuGradient()),
                                  text(util::padLeft(fmt::percent(gpu_busy->last()), 8)) | bold}));
            lines.push_back(hbox({text(util::padRight("tokens/s", 14)) | color(chart::graphText()),
                                  chart::sparkline(resample(s.generation, now, span, sw, top), sw, chart::cachedGradient()),
                                  text(util::padLeft(fmt::count(s.generation.last()), 8)) | bold}));
            if (gpus > 0 && s.generation.last() >= 0) lines.push_back(line("per GPU", fmt::count(s.generation.last() / gpus) + " tok/s"));
        }
        return box("requests", nullptr, vbox(std::move(lines)), chart::availableGradient().mid, width, height);
    }
};

}  // namespace ui
