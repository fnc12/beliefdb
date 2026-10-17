// HTTP client for the embedding provider. Kept in its own translation unit
// because httplib.h is large and nothing else needs it.
#pragma once

#include "beliefdb/types.hpp"

#include <string>
#include <vector>

namespace beliefdb::embed {

struct Result {
    std::vector<float> vector;  // empty on failure
    std::string error;          // set on failure
};

/// POST {base_url}/v1/embeddings {"model": cfg.model, "input": text}.
/// Never throws; a failure is reported in `error` and the caller falls back to FTS.
Result embed_text(const EmbeddingConfig& cfg, const std::string& text);

/// Vector <-> blob (float32, native endianness; the store and its readers share a machine class).
std::vector<char> to_blob(const std::vector<float>&);
std::vector<float> from_blob(const std::vector<char>&);
double cosine(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace beliefdb::embed
