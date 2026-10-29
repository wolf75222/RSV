#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../utils/format.hpp"
#include "run.hpp"
#include "slurmjobs.hpp"

/* A job that serves a model (vLLM, SGLang, TGI, llama.cpp, Triton, Ollama): where its server is and what it does
- Found without being told: the job's output, errors and script say which server it runs and on which port; the
  server's Prometheus metrics (GET /metrics on its node, from the login node) say what it does
- RSV_METRICS_URL gives it by hand: "http://{node}:8000/metrics" ({node}: each of the job's nodes), several with commas
- Each engine names its metrics its own way: one adapter each turns them into the same reading
*/
namespace api {

// ---------------------------------------------------------------- Prometheus' text format

struct PromSample {
    std::string name;
    std::vector<std::pair<std::string, std::string>> labels;
    double value = 0;

    std::string label(const std::string& key) const {
        for (const auto& [k, v] : labels)
            if (k == key) return v;
        return "";
    }
};

/* A histogram: (upper bound, how many at most that) rising, the sum and the count of what it measured
- A summary of an engine without buckets (Triton: a total time and a count) has only sum and count: its mean
*/
struct Histogram {
    std::vector<std::pair<double, double>> buckets;
    double sum = 0, count = 0;

    bool empty() const { return count <= 0; }
    double mean() const { return count > 0 ? sum / count : -1; }

    // What it measured since `before` (an older reading of the same): a restart (fewer) gives it all
    Histogram since(const Histogram& before) const {
        if (before.count > count || before.buckets.size() != buckets.size()) return *this;
        Histogram d;
        d.sum = sum - before.sum;
        d.count = count - before.count;
        for (size_t i = 0; i < buckets.size(); i++)
            d.buckets.push_back({buckets[i].first, std::max(0.0, buckets[i].second - before.buckets[i].second)});
        return d;
    }

    // As Prometheus' histogram_quantile: within the bucket it falls in, linearly; -1 when it measured nothing
    double quantile(double q) const {
        if (count <= 0) return -1;
        if (buckets.empty()) return mean();
        const double total = buckets.back().second > 0 ? buckets.back().second : count;
        const double rank = q * total;
        double lower = 0, below = 0, n = 0;
        for (const auto& [le, count_le] : buckets) {
            n = std::max(n, count_le);   // never fewer than in the bucket under it (a reading taken while it counted)
            if (n >= rank) {
                if (std::isinf(le)) return lower;
                if (n - below <= 0) return le;
                return lower + (le - lower) * (rank - below) / (n - below);
            }
            lower = std::isinf(le) ? lower : le;
            below = n;
        }
        return lower;
    }
};

struct Metrics {
    std::vector<PromSample> samples;

    bool has(const std::string& name) const {
        for (const auto& s : samples)
            if (s.name == name) return true;
        return false;
    }

    // The sum over its label sets (only those with key=value when given); -1 when it isn't there
    double sum(const std::string& name, const std::string& key = "", const std::string& value = "") const {
        double total = 0;
        bool found = false;
        for (const auto& s : samples) {
            if (s.name != name || (!key.empty() && s.label(key) != value) || std::isnan(s.value)) continue;
            total += s.value;
            found = true;
        }
        return found ? total : -1;
    }

    // The first of these names that is there (engines renamed theirs between versions)
    double first(std::initializer_list<const char*> names) const {
        for (const char* n : names) {
            const double v = sum(n);
            if (v >= 0) return v;
        }
        return -1;
    }

    // Each value of a label: {value: sum}
    std::map<std::string, double> byLabel(const std::string& name, const std::string& key) const {
        std::map<std::string, double> out;
        for (const auto& s : samples)
            if (s.name == name && !std::isnan(s.value)) out[s.label(key)] += s.value;
        return out;
    }

    std::string anyLabel(const std::string& prefix, const std::string& key) const {
        for (const auto& s : samples)
            if (util::startsWith(s.name, prefix)) {
                const std::string v = s.label(key);
                if (!v.empty()) return v;
            }
        return "";
    }

    // name_bucket{le=...}, name_sum, name_count, summed over the other labels
    Histogram histogram(const std::string& name) const {
        Histogram h;
        std::map<double, double> by_le;
        bool found = false;
        for (const auto& s : samples) {
            if (s.name == name + "_bucket") {
                const std::string le = s.label("le");
                by_le[le == "+Inf" ? INFINITY : util::toDouble(le)] += s.value;
            } else if (s.name == name + "_sum") {
                h.sum += s.value;
                found = true;
            } else if (s.name == name + "_count") {
                h.count += s.value;
                found = true;
            }
        }
        if (!found) return Histogram{};
        for (const auto& [le, n] : by_le) h.buckets.push_back({le, n});
        return h;
    }

    Histogram firstHistogram(std::initializer_list<const char*> names) const {
        for (const char* n : names) {
            Histogram h = histogram(n);
            if (h.count > 0 || !h.buckets.empty()) return h;
        }
        return Histogram{};
    }
};

/* name{a="x",b="y"} 12.5 [timestamp] per line; # lines are comments
- Label values may hold \" \\ \n, commas and braces
*/
inline Metrics parsePrometheus(const std::string& text) {
    Metrics m;
    for (const auto& raw : util::lines(text)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t i = line.find_first_not_of(' ');
        if (i == std::string::npos || line[i] == '#') continue;
        PromSample s;
        const size_t name_end = line.find_first_of("{ \t", i);
        if (name_end == std::string::npos) continue;
        s.name = line.substr(i, name_end - i);
        i = name_end;
        if (line[i] == '{') {
            i++;
            while (i < line.size() && line[i] != '}') {
                while (i < line.size() && (line[i] == ',' || line[i] == ' ')) i++;
                if (i < line.size() && line[i] == '}') break;
                const size_t eq = line.find('=', i);
                if (eq == std::string::npos || eq + 1 >= line.size() || line[eq + 1] != '"') break;
                std::string key = util::trim(line.substr(i, eq - i)), value;
                i = eq + 2;
                for (; i < line.size() && line[i] != '"'; i++) {
                    if (line[i] == '\\' && i + 1 < line.size()) {
                        i++;
                        value += line[i] == 'n' ? '\n' : line[i];
                    } else {
                        value += line[i];
                    }
                }
                i++;   // the closing quote
                s.labels.emplace_back(std::move(key), std::move(value));
            }
            if (i >= line.size()) continue;
            i++;   // }
        }
        const size_t v = line.find_first_not_of(" \t", i);
        if (v == std::string::npos) continue;
        const std::string value = line.substr(v, line.find_first_of(" \t", v) - v);
        if (value == "NaN") s.value = NAN;
        else if (value == "+Inf") s.value = INFINITY;
        else if (value == "-Inf") s.value = -INFINITY;
        else {
            char* end = nullptr;
            s.value = std::strtod(value.c_str(), &end);
            if (end == value.c_str()) continue;
        }
        m.samples.push_back(std::move(s));
    }
    return m;
}

// ---------------------------------------------------------------- engines

enum class Engine { None, Vllm, Sglang, Tgi, LlamaCpp, Triton, Ollama };

inline std::string engineName(Engine e) {
    switch (e) {
        case Engine::Vllm: return "vLLM";
        case Engine::Sglang: return "SGLang";
        case Engine::Tgi: return "TGI";
        case Engine::LlamaCpp: return "llama.cpp";
        case Engine::Triton: return "Triton";
        case Engine::Ollama: return "Ollama";
        default: return "";
    }
}

// The port each one listens on when told nothing
inline int defaultPort(Engine e) {
    switch (e) {
        case Engine::Vllm: return 8000;
        case Engine::Sglang: return 30000;
        case Engine::Tgi: return 3000;
        case Engine::LlamaCpp: return 8080;
        case Engine::Triton: return 8002;
        case Engine::Ollama: return 11434;
        default: return 0;
    }
}

// Whose metrics these are, by their names
inline Engine engineOfMetrics(const Metrics& m) {
    for (const auto& s : m.samples) {
        if (util::startsWith(s.name, "vllm:")) return Engine::Vllm;
        if (util::startsWith(s.name, "sglang:")) return Engine::Sglang;
        if (util::startsWith(s.name, "tgi_")) return Engine::Tgi;
        if (util::startsWith(s.name, "llamacpp:")) return Engine::LlamaCpp;
        if (util::startsWith(s.name, "nv_inference_")) return Engine::Triton;
    }
    return Engine::None;
}

// What a log or a script says it runs: the words each one writes (lower case)
inline Engine engineInText(const std::string& text) {
    const std::string t = util::lower(text);
    static const std::vector<std::pair<const char*, Engine>> words = {
        {"vllm", Engine::Vllm}, {"avg prompt throughput", Engine::Vllm},
        {"sglang", Engine::Sglang}, {"decode batch. #running-req", Engine::Sglang},
        {"text-generation-launcher", Engine::Tgi}, {"text_generation_router", Engine::Tgi}, {"text-generation-inference", Engine::Tgi},
        {"llama-server", Engine::LlamaCpp}, {"llama_server", Engine::LlamaCpp}, {"llama.cpp", Engine::LlamaCpp},
        {"tritonserver", Engine::Triton}, {"triton inference server", Engine::Triton},
        {"ollama", Engine::Ollama}};
    for (const auto& [w, e] : words)
        if (t.find(w) != std::string::npos) return e;
    return Engine::None;
}

/* One reading of a server, whatever its engine: -1 (or empty) for what it doesn't tell
- Counters only grow (rates come from two readings); gauges are what it is now
*/
struct InferenceReading {
    Engine engine = Engine::None;
    std::string model;
    double running = -1, waiting = -1, kv_usage = -1;          // requests; KV cache 0..1
    double prompt_tokens = -1, generation_tokens = -1;         // counters
    double prompt_tps = -1, generation_tps = -1;               // the engine's own rates (llama.cpp, SGLang)
    double requests_done = -1, requests_failed = -1, preemptions = -1;
    double prefix_queries = -1, prefix_hits = -1, prefix_hit_rate = -1;   // tokens; or the engine's own rate
    double drafts = -1, draft_tokens = -1, accepted_tokens = -1, accept_length = -1;
    std::vector<double> accepted_per_position;
    std::map<std::string, double> finished;                    // by reason: stop, length, abort, error
    std::map<std::string, double> waiting_by_reason;
    Histogram ttft, itl, e2e, queue, prefill, decode, prompt_length, output_length;
    std::vector<std::pair<std::string, double>> loaded;        // Ollama: the models in memory, their VRAM bytes
};

inline InferenceReading readVllm(const Metrics& m) {
    InferenceReading r;
    r.engine = Engine::Vllm;
    r.model = m.anyLabel("vllm:", "model_name");
    r.running = m.sum("vllm:num_requests_running");
    r.waiting = m.sum("vllm:num_requests_waiting");
    for (const auto& [reason, n] : m.byLabel("vllm:num_requests_waiting_by_reason", "reason")) r.waiting_by_reason[reason] = n;
    r.kv_usage = m.first({"vllm:kv_cache_usage_perc", "vllm:gpu_cache_usage_perc"});
    r.prompt_tokens = m.sum("vllm:prompt_tokens_total");
    r.generation_tokens = m.sum("vllm:generation_tokens_total");
    r.finished = m.byLabel("vllm:request_success_total", "finished_reason");
    if (!r.finished.empty()) {
        r.requests_done = r.requests_failed = 0;
        for (const auto& [reason, n] : r.finished) {
            r.requests_done += n;
            if (reason == "abort" || reason == "error") r.requests_failed += n;
        }
    }
    r.preemptions = m.sum("vllm:num_preemptions_total");
    r.prefix_queries = m.first({"vllm:prefix_cache_queries_total", "vllm:gpu_prefix_cache_queries_total"});
    r.prefix_hits = m.first({"vllm:prefix_cache_hits_total", "vllm:gpu_prefix_cache_hits_total"});
    r.prefix_hit_rate = m.sum("vllm:gpu_prefix_cache_hit_rate");
    r.drafts = m.sum("vllm:spec_decode_num_drafts_total");
    r.draft_tokens = m.sum("vllm:spec_decode_num_draft_tokens_total");
    r.accepted_tokens = m.sum("vllm:spec_decode_num_accepted_tokens_total");
    for (const auto& [pos, n] : m.byLabel("vllm:spec_decode_num_accepted_tokens_per_pos_total", "position")) {
        const size_t p = static_cast<size_t>(std::max(0L, util::toLong(pos)));
        if (p < 64) {
            if (r.accepted_per_position.size() <= p) r.accepted_per_position.resize(p + 1, 0);
            r.accepted_per_position[p] += n;
        }
    }
    r.ttft = m.histogram("vllm:time_to_first_token_seconds");
    r.itl = m.firstHistogram({"vllm:inter_token_latency_seconds", "vllm:time_per_output_token_seconds"});
    r.e2e = m.histogram("vllm:e2e_request_latency_seconds");
    r.queue = m.histogram("vllm:request_queue_time_seconds");
    r.prefill = m.histogram("vllm:request_prefill_time_seconds");
    r.decode = m.histogram("vllm:request_decode_time_seconds");
    r.prompt_length = m.histogram("vllm:request_prompt_tokens");
    r.output_length = m.histogram("vllm:request_generation_tokens");
    return r;
}

inline InferenceReading readSglang(const Metrics& m) {
    InferenceReading r;
    r.engine = Engine::Sglang;
    r.model = m.anyLabel("sglang:", "model_name");
    r.running = m.sum("sglang:num_running_reqs");
    r.waiting = m.sum("sglang:num_queue_reqs");
    r.kv_usage = m.sum("sglang:token_usage");
    r.prompt_tokens = m.sum("sglang:prompt_tokens_total");
    r.generation_tokens = m.sum("sglang:generation_tokens_total");
    r.generation_tps = m.sum("sglang:gen_throughput");
    r.requests_done = m.sum("sglang:num_requests_total");
    const double aborted = m.first({"sglang:num_aborted_requests_total", "sglang:num_aborted_requests"});
    if (aborted >= 0) {
        r.requests_failed = aborted;
        r.finished["abort"] = aborted;
        if (r.requests_done >= 0) r.finished["stop"] = std::max(0.0, r.requests_done - aborted);
    }
    r.preemptions = m.first({"sglang:num_retracted_reqs_total", "sglang:num_retracted_reqs"});
    r.prefix_hit_rate = m.sum("sglang:cache_hit_rate");
    r.accept_length = m.sum("sglang:spec_accept_length");
    r.ttft = m.histogram("sglang:time_to_first_token_seconds");
    r.itl = m.firstHistogram({"sglang:inter_token_latency_seconds", "sglang:time_per_output_token_seconds"});
    r.e2e = m.histogram("sglang:e2e_request_latency_seconds");
    r.queue = m.firstHistogram({"sglang:queue_time_seconds", "sglang:request_queue_time_seconds"});
    return r;
}

inline InferenceReading readTgi(const Metrics& m) {
    InferenceReading r;
    r.engine = Engine::Tgi;
    r.running = m.sum("tgi_batch_current_size");
    r.waiting = m.sum("tgi_queue_size");
    const double ok = m.sum("tgi_request_success"), failed = m.sum("tgi_request_failure");
    if (ok >= 0) {
        r.requests_done = ok + std::max(0.0, failed);
        r.requests_failed = std::max(0.0, failed);
        r.finished["stop"] = ok;
        if (failed > 0) r.finished["error"] = failed;
    }
    r.prompt_length = m.histogram("tgi_request_input_length");
    r.output_length = m.histogram("tgi_request_generated_tokens");
    if (!r.prompt_length.empty()) r.prompt_tokens = r.prompt_length.sum;
    if (!r.output_length.empty()) r.generation_tokens = r.output_length.sum;
    r.e2e = m.histogram("tgi_request_duration");
    r.queue = m.histogram("tgi_request_queue_duration");
    r.itl = m.histogram("tgi_request_mean_time_per_token_duration");
    r.prefill = m.histogram("tgi_request_inference_duration");
    return r;
}

inline InferenceReading readLlamaCpp(const Metrics& m) {
    InferenceReading r;
    r.engine = Engine::LlamaCpp;
    r.running = m.sum("llamacpp:requests_processing");
    r.waiting = m.sum("llamacpp:requests_deferred");
    r.kv_usage = m.sum("llamacpp:kv_cache_usage_ratio");
    r.prompt_tokens = m.sum("llamacpp:prompt_tokens_total");
    r.generation_tokens = m.sum("llamacpp:tokens_predicted_total");
    r.prompt_tps = m.sum("llamacpp:prompt_tokens_seconds");
    r.generation_tps = m.sum("llamacpp:predicted_tokens_seconds");
    return r;
}

// Triton: counts and total times in microseconds (a mean, no buckets); TensorRT-LLM's backend adds its batch and cache
inline InferenceReading readTriton(const Metrics& m) {
    InferenceReading r;
    r.engine = Engine::Triton;
    r.model = m.anyLabel("nv_inference_", "model");
    r.waiting = m.sum("nv_inference_pending_request_count");
    r.running = m.sum("nv_trt_llm_request_metrics", "request_type", "active");
    const double used = m.sum("nv_trt_llm_kv_cache_block_metrics", "kv_cache_block_type", "used");
    const double max = m.sum("nv_trt_llm_kv_cache_block_metrics", "kv_cache_block_type", "max");
    if (used >= 0 && max > 0) r.kv_usage = used / max;
    const double ok = m.sum("nv_inference_request_success"), failed = m.sum("nv_inference_request_failure");
    if (ok >= 0) {
        r.requests_done = ok + std::max(0.0, failed);
        r.requests_failed = std::max(0.0, failed);
        r.finished["stop"] = ok;
        if (failed > 0) r.finished["error"] = failed;
    }
    const auto mean = [&](const char* total_us) {
        Histogram h;
        const double t = m.sum(total_us);
        if (t >= 0 && ok >= 0) {
            h.sum = t / 1e6;
            h.count = ok;
        }
        return h;
    };
    r.e2e = mean("nv_inference_request_duration_us");
    r.queue = mean("nv_inference_queue_duration_us");
    r.prefill = mean("nv_inference_compute_infer_duration_us");
    return r;
}

inline InferenceReading readMetrics(const Metrics& m) {
    switch (engineOfMetrics(m)) {
        case Engine::Vllm: return readVllm(m);
        case Engine::Sglang: return readSglang(m);
        case Engine::Tgi: return readTgi(m);
        case Engine::LlamaCpp: return readLlamaCpp(m);
        case Engine::Triton: return readTriton(m);
        default: return InferenceReading{};
    }
}

/* Ollama has no metrics: /api/ps says which models are loaded
   {"models":[{"name":"llama3:8b","model":"llama3:8b","size":5137025024,"size_vram":5137025024,...}]} */
inline InferenceReading readOllama(const std::string& json) {
    InferenceReading r;
    r.engine = Engine::Ollama;
    size_t at = 0;
    while ((at = json.find("\"name\"", at)) != std::string::npos) {
        const size_t open = json.find('"', json.find(':', at) + 1);
        const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
        if (close == std::string::npos) break;
        const std::string name = json.substr(open + 1, close - open - 1);
        double vram = 0;
        const size_t next = json.find("\"name\"", close);
        const size_t v = json.find("\"size_vram\"", close);
        if (v != std::string::npos && (next == std::string::npos || v < next)) vram = util::toDouble(json.substr(json.find(':', v) + 1, 24));
        r.loaded.push_back({name, vram});
        if (r.model.empty()) r.model = name;
        at = close;
    }
    r.running = static_cast<double>(r.loaded.size());
    return r;
}

// ---------------------------------------------------------------- finding the server

struct Endpoint {
    std::string host;   // a node of the job (or the host the log gave)
    int port = 0;
    std::string path = "/metrics";
    std::string url() const { return "http://" + host + ":" + std::to_string(port) + path; }
    std::string where() const { return host + ":" + std::to_string(port); }
    bool operator==(const Endpoint& o) const { return host == o.host && port == o.port && path == o.path; }
};

/* What a log or a script says: which engine, and the ports it may listen on, the surest first
- http://host:port anywhere (a server announcing itself), host:port after a wildcard address, --port N, port=N
- A host that isn't one of the job's nodes nor a wildcard (huggingface.co...) is left out
*/
struct ServerHint {
    Engine engine = Engine::None;
    std::vector<std::pair<std::string, int>> places;   // host ("" for each of the job's nodes), port
    std::string evidence;                               // the line it came from

    void add(const std::string& host, int port) {
        if (port < 1024 || port > 65535) return;
        for (const auto& p : places)
            if (p.first == host && p.second == port) return;
        places.push_back({host, port});
    }
};

namespace detail {

inline int digitsAt(const std::string& s, size_t i) {
    size_t j = i;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && j - i < 6) j++;
    return j == i ? -1 : static_cast<int>(util::toLong(s.substr(i, j - i)));
}

inline bool wildcardHost(const std::string& h) {
    return h.empty() || h == "0.0.0.0" || h == "127.0.0.1" || h == "localhost" || h == "::" || h == "[::]" || h == "*";
}

}  // namespace detail

inline void scanForServer(const std::string& text, const std::vector<std::string>& nodes, ServerHint& hint) {
    const std::set<std::string> node_set(nodes.begin(), nodes.end());
    const auto hostOk = [&](const std::string& h) {
        if (detail::wildcardHost(h) || node_set.count(h)) return true;
        // a node by its short name or another of its names (romeo-a033.u-reims.fr, romeo-a033-ib)
        for (const auto& n : nodes)
            if (util::startsWith(h, n)) return true;
        return false;
    };
    for (const auto& line : util::lines(text)) {
        if (line.size() > 4096) continue;   // a config dump: no address in it worth the time
        const std::string low = util::lower(line);
        const Engine e = engineInText(low);
        if (e != Engine::None && hint.engine == Engine::None) hint.engine = e;
        bool found = false;
        // http://host:port
        for (size_t at = 0; (at = low.find("://", at)) != std::string::npos; at += 3) {
            if (at < 4 || (low.compare(at - 4, 4, "http") != 0 && (at < 5 || low.compare(at - 5, 5, "https") != 0))) continue;
            size_t h = at + 3;
            std::string host;
            if (h < low.size() && low[h] == '[') {
                const size_t close = low.find(']', h);
                if (close == std::string::npos) continue;
                host = low.substr(h, close - h + 1);
                h = close + 1;
            } else {
                while (h < low.size() && (std::isalnum(static_cast<unsigned char>(low[h])) || low[h] == '.' || low[h] == '-' || low[h] == '_')) host += low[h++];
            }
            if (h >= low.size() || low[h] != ':') continue;
            const int port = detail::digitsAt(low, h + 1);
            if (port > 0 && hostOk(host)) {
                hint.add(detail::wildcardHost(host) ? "" : host, port);
                found = true;
            }
        }
        // 0.0.0.0:8000, localhost:8000
        for (const char* w : {"0.0.0.0:", "127.0.0.1:", "localhost:", "[::]:"}) {
            for (size_t at = 0; (at = low.find(w, at)) != std::string::npos; at++) {
                const int port = detail::digitsAt(low, at + std::string(w).size());
                if (port > 0) {
                    hint.add("", port);
                    found = true;
                }
            }
        }
        // --port 8000, --port=8000, port=8000, PORT: 8000 (when the line is about a server)
        for (size_t at = 0; (at = low.find("port", at)) != std::string::npos; at += 4) {
            if (at > 0 && std::isalpha(static_cast<unsigned char>(low[at - 1])) && low[at - 1] != 't') continue;   // "report", but "http_port"
            size_t v = at + 4;
            while (v < low.size() && (low[v] == ' ' || low[v] == '=' || low[v] == ':' || low[v] == '"' || low[v] == '\'')) v++;
            if (v == at + 4) continue;
            const int port = detail::digitsAt(low, v);
            const bool about_server = e != Engine::None || low.find("--port") != std::string::npos || low.find("serve") != std::string::npos ||
                                      low.find("api") != std::string::npos || low.find("http") != std::string::npos;
            if (port > 0 && about_server) {
                hint.add("", port);
                found = true;
            }
        }
        if (found && hint.evidence.empty()) hint.evidence = util::trim(line).substr(0, 200);
    }
}

// The start of a file and its end: the server announces itself early, the engine keeps naming itself later
inline std::string readEnds(const std::string& path, size_t head = 2u << 20, size_t tail = 512u << 10) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(std::max<std::streamoff>(0, f.tellg()));
    std::string out;
    out.resize(std::min(size, head));
    f.seekg(0);
    f.read(&out[0], static_cast<std::streamsize>(out.size()));
    if (size > head) {
        const size_t from = std::max(head, size - tail);
        std::string end(size - from, '\0');
        f.seekg(static_cast<std::streamoff>(from));
        f.read(&end[0], static_cast<std::streamsize>(end.size()));
        out += "\n" + end;
    }
    return out;
}

/* Where to look, the surest first: RSV_METRICS_URL; then each place the texts give, on each node; then the engine's
   default port. At most `max` of them. */
inline std::vector<Endpoint> candidates(const ServerHint& hint, const std::vector<std::string>& nodes, const std::string& override_urls,
                                        size_t max = 12) {
    std::vector<Endpoint> out;
    const auto add = [&](Endpoint e) {
        if (out.size() < max && std::find(out.begin(), out.end(), e) == out.end()) out.push_back(std::move(e));
    };
    for (const auto& u : util::split(override_urls, ',')) {
        std::string url = util::trim(u);
        if (url.empty()) continue;
        if (util::startsWith(url, "http://")) url = url.substr(7);
        const size_t slash = url.find('/');
        const std::string hostport = url.substr(0, slash), path = slash == std::string::npos ? "/metrics" : url.substr(slash);
        const size_t colon = hostport.rfind(':');
        const std::string host = hostport.substr(0, colon);
        const int port = colon == std::string::npos ? 80 : static_cast<int>(util::toLong(hostport.substr(colon + 1)));
        if (host == "{node}") {
            for (const auto& n : nodes) add({n, port, path});
        } else {
            add({host, port, path});
        }
    }
    const std::string path = hint.engine == Engine::Ollama ? "/api/ps" : "/metrics";
    for (const auto& [host, port] : hint.places) {
        if (!host.empty()) add({host, port, path});
        else
            for (const auto& n : nodes) add({n, port, path});
    }
    if (hint.engine != Engine::None)
        for (const auto& n : nodes) add({n, defaultPort(hint.engine), path});
    return out;
}

inline RunResult httpGet(const std::string& url, int timeout_seconds) {
    return run({"curl", "-sS", "--fail", "--noproxy", "*", "--max-time", std::to_string(timeout_seconds), url}, timeout_seconds + 2);
}

/* One reading of a server: its metrics (or Ollama's models), as one engine's adapter reads them
- `error` when it doesn't answer, or answers with nothing a server of models says
*/
struct Scraped {
    InferenceReading reading;
    std::string error;
};

inline Scraped scrape(const Endpoint& e, int timeout_seconds = 3) {
    Scraped s;
    const RunResult r = httpGet(e.url(), timeout_seconds);
    if (!r.ok()) {
        s.error = r.started ? util::trim(r.err.empty() ? r.error("curl") : r.err.substr(0, r.err.find('\n'))) : "curl: " + r.err;
        return s;
    }
    if (e.path == "/api/ps") {
        if (r.out.find("\"models\"") == std::string::npos) s.error = "not Ollama's answer";
        else s.reading = readOllama(r.out);
        return s;
    }
    s.reading = readMetrics(parsePrometheus(r.out));
    if (s.reading.engine == Engine::None) s.error = "metrics, but of no model server RSV knows";
    return s;
}

/* What the search found: the servers that answer (several when the job runs several), else why not
- `hint`: what the texts said, even when nothing answers yet (a model still loading)
*/
struct Discovery {
    std::vector<Endpoint> servers;
    std::vector<InferenceReading> first;   // their first reading
    ServerHint hint;
    std::vector<std::string> tried;        // "romeo-a033:8000: Connection refused"
    std::vector<std::string> looked_in;    // the files and the script read
};

inline Discovery discover(const std::string& job_id, const std::vector<std::string>& nodes, const std::vector<std::string>& files,
                          const std::string& name, const std::string& override_urls) {
    Discovery d;
    for (const auto& f : files) {
        if (f.empty()) continue;
        const std::string text = readEnds(f);
        if (text.empty()) continue;
        d.looked_in.push_back(f);
        scanForServer(text, nodes, d.hint);
    }
    const RunResult script = run({"scontrol", "write", "batch_script", job_id, "-"}, 10);
    if (script.ok()) {
        d.looked_in.push_back("its script");
        scanForServer(script.out, nodes, d.hint);
    }
    if (d.hint.engine == Engine::None) d.hint.engine = engineInText(name);
    for (const auto& e : candidates(d.hint, nodes, override_urls)) {
        // One server per node and port: a second path there is the same server
        bool known = false;
        for (const auto& s : d.servers) known = known || (s.host == e.host && s.port == e.port);
        if (known) continue;
        const Scraped s = scrape(e, 2);
        if (!s.error.empty()) {
            d.tried.push_back(e.where() + ": " + s.error);
            continue;
        }
        d.servers.push_back(e);
        d.first.push_back(s.reading);
        if (d.hint.engine == Engine::None) d.hint.engine = s.reading.engine;
    }
    return d;
}

}  // namespace api
