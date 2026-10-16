#include "util.hpp"
#include "sha256.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace beliefdb::util {

std::string now_iso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

std::string ulid() {
    static constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    using namespace std::chrono;
    const auto ms = static_cast<unsigned long long>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const unsigned long long r1 = rng(), r2 = rng();

    std::string out(26, '0');
    // 48-bit time -> 10 chars
    unsigned long long t = ms;
    for (int i = 9; i >= 0; --i) { out[static_cast<size_t>(i)] = kAlphabet[t & 31]; t >>= 5; }
    // 80-bit randomness -> 16 chars (uses 64 bits of r1 and 16 of r2)
    unsigned long long a = r1, b = r2 & 0xFFFF;
    for (int i = 25; i >= 13; --i) { out[static_cast<size_t>(i)] = kAlphabet[a & 31]; a >>= 5; }
    for (int i = 12; i >= 10; --i) { out[static_cast<size_t>(i)] = kAlphabet[b & 31]; b >>= 5; }
    return out;
}

std::string sha256_hex(std::string_view data) {
    return sha256::hex(data);
}

bool looks_like_iso8601(std::string_view s) {
    // YYYY-MM-DD at minimum; optional time part. Not a full parser on purpose.
    if (s.size() < 10) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return digit(s[0]) && digit(s[1]) && digit(s[2]) && digit(s[3]) && s[4] == '-' && digit(s[5]) && digit(s[6]) &&
           s[7] == '-' && digit(s[8]) && digit(s[9]) && (s.size() == 10 || s[10] == 'T' || s[10] == ' ');
}

}  // namespace beliefdb::util

namespace beliefdb::util {

std::vector<std::string> node_ids_in(std::string_view text) {
    // <prefix>_<26 chars of Crockford base32>; prefixes as in Impl::type_prefix
    static constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::vector<std::string> out;
    for (size_t i = 0; i + 28 <= text.size(); ++i) {
        const char p = text[i];
        if ((p != 's' && p != 'c' && p != 'h' && p != 'q' && p != 'r') || text[i + 1] != '_') continue;
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_')) continue;
        size_t j = i + 2;
        while (j < text.size() && j < i + 28 && kAlphabet.find(text[j]) != std::string_view::npos) ++j;
        if (j != i + 28) continue;
        if (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) continue;
        out.emplace_back(text.substr(i, 28));
    }
    return out;
}

}  // namespace beliefdb::util
