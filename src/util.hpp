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
/// Seconds from `from` to `to` for ISO-8601 UTC timestamps as written by now_iso()
/// (date-only accepted). Negative when `to` is earlier. 0 when either does not parse.
double seconds_between(std::string_view from, std::string_view to);
/// now_iso() shifted by `seconds` (may be negative), millisecond precision.
std::string iso_from_now(double seconds);
}
