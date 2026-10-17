#include "embed.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <httplib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

namespace beliefdb::embed {

Result embed_text(const EmbeddingConfig& cfg, const std::string& text) {
    Result r;
    if (cfg.base_url.empty()) { r.error = "embeddings not configured"; return r; }
    try {
        httplib::Client cli(cfg.base_url);
        const auto ms = std::chrono::milliseconds(cfg.timeout_ms);
        cli.set_connection_timeout(ms);
        cli.set_read_timeout(ms);
        cli.set_write_timeout(ms);
        const nlohmann::json body{{"model", cfg.model}, {"input", text}};
        auto res = cli.Post("/v1/embeddings", body.dump(), "application/json");
        if (!res) { r.error = cfg.base_url + ": " + httplib::to_string(res.error()); return r; }
        if (res->status != 200) { r.error = cfg.base_url + ": HTTP " + std::to_string(res->status); return r; }
        const auto j = nlohmann::json::parse(res->body, nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array() || j["data"].empty() ||
            !j["data"][0].contains("embedding") || !j["data"][0]["embedding"].is_array()) {
            r.error = cfg.base_url + ": unexpected response shape";
            return r;
        }
        for (const auto& x : j["data"][0]["embedding"]) r.vector.push_back(x.get<float>());
        if (r.vector.empty()) r.error = cfg.base_url + ": empty vector";
    } catch (const std::exception& e) {
        r.vector.clear();
        r.error = cfg.base_url + ": " + e.what();
    }
    return r;
}

std::vector<char> to_blob(const std::vector<float>& v) {
    std::vector<char> out(v.size() * sizeof(float));
    if (!v.empty()) std::memcpy(out.data(), v.data(), out.size());
    return out;
}

std::vector<float> from_blob(const std::vector<char>& b) {
    std::vector<float> out(b.size() / sizeof(float));
    if (!out.empty()) std::memcpy(out.data(), b.data(), out.size() * sizeof(float));
    return out;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += double(a[i]) * b[i]; na += double(a[i]) * a[i]; nb += double(b[i]) * b[i]; }
    if (na == 0 || nb == 0) return 0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

}  // namespace beliefdb::embed
