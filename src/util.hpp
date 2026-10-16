#pragma once
#include <string>
#include <string_view>

namespace beliefdb::util {
std::string now_iso();                       // 2026-10-16T12:34:56.789Z
std::string ulid();                          // 26 chars, Crockford base32, time-ordered
std::string sha256_hex(std::string_view);    // lowercase hex
bool looks_like_iso8601(std::string_view);   // cheap sanity check for timestamps
}
