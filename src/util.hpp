#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace beliefdb::util {
std::string now_iso();                       // 2026-10-16T12:34:56.789Z
std::string ulid();                          // 26 chars, Crockford base32, time-ordered
/// Every token shaped like a node id of this store (type prefix + 26-char ULID) found in `text`.
std::vector<std::string> node_ids_in(std::string_view text);
std::string sha256_hex(std::string_view);    // lowercase hex
bool looks_like_iso8601(std::string_view);   // cheap sanity check for timestamps
}
