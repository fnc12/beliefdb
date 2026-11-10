// The MCP server: a scripted JSON-RPC session over stdio against the built binary.
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef BELIEF_MCP_BIN
#define BELIEF_MCP_BIN ""
#endif

using nlohmann::json;

namespace {
std::vector<json> session(const std::string& db, const std::vector<json>& requests) {
    const auto dir = std::filesystem::temp_directory_path() / "beliefdb-mcp-test";
    std::filesystem::create_directories(dir);
    const auto in = dir / "in.jsonl";
    { std::ofstream f(in); for (const auto& r : requests) f << r.dump() << '\n'; }
    const std::string cmd = std::string("\"") + BELIEF_MCP_BIN + "\" --db \"" + db + "\" < \"" + in.string() + "\" 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    std::vector<json> out;
    char buf[65536];
    std::string line;
    while (std::fgets(buf, sizeof buf, p)) {
        line += buf;
        if (!line.empty() && line.back() == '\n') { out.push_back(json::parse(line)); line.clear(); }
    }
    pclose(p);
    return out;
}
json call(int id, const char* tool, json args) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", std::move(args)}}}}; }
std::string text(const json& reply) { return reply["result"]["content"][0]["text"].get<std::string>(); }
}  // namespace

TEST_CASE("mcp: initialize, tools/list, a write and a read through tools/call, errors as isError, unknown methods as JSON-RPC errors") {
    if (std::string(BELIEF_MCP_BIN).empty()) { MESSAGE("BELIEF_MCP_BIN not set, skipping"); return; }
    const auto db = (std::filesystem::temp_directory_path() / "beliefdb-mcp-test" / "t.sqlite").string();
    std::filesystem::create_directories(std::filesystem::path(db).parent_path());
    std::filesystem::remove(db);
    auto r = session(db, {
        json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}, {"clientInfo", {{"name", "test"}, {"version", "0"}}}}}},
        json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},  // a notification: no answer
        json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
        call(3, "observe", {{"content", "User: I switched to decaf in March 2026"}, {"kind", "human_statement"}, {"attribution", "user"}, {"uri", "chat:1"}, {"domain", "health"}}),
        call(4, "assert", {{"text", "The user drinks decaf"}, {"support", json::array({"s_missing"})}, {"credence", 0.8}}),
        json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "nope"}},
    });
    REQUIRE(r.size() == 5);
    CHECK(r[0]["id"] == 1);
    CHECK(r[0]["result"]["serverInfo"]["name"] == "beliefdb");
    CHECK(r[0]["result"]["instructions"].get<std::string>().find("search") != std::string::npos);
    CHECK(r[1]["result"]["tools"].size() == 16);
    CHECK(r[1]["result"]["tools"][0]["name"] == "observe");
    CHECK(r[1]["result"]["tools"][0]["inputSchema"]["required"][0] == "content");
    const std::string sid = json::parse(text(r[2]))["id"];
    CHECK(sid.rfind("s_", 0) == 0);
    CHECK(r[3]["result"]["isError"] == true);
    CHECK(text(r[3]).find("invalid_support") != std::string::npos);
    CHECK(r[4]["error"]["code"] == -32601);

    // a second process on the same file sees the source: the memory persists between sessions
    auto r2 = session(db, {
        call(1, "assert", {{"text", "The user drinks decaf"}, {"support", json::array({sid})}, {"credence", 0.8}, {"domain", "health"}}),
        call(2, "search", {{"query", "decaf"}}),
        call(3, "why", {{"id", sid}}),
        call(4, "hypothesize", {{"text", "The user avoids caffeine"}, {"from", json::array()}, {"rule", "generalization"}}),
        call(5, "stats", json::object()),
    });
    REQUIRE(r2.size() == 5);
    const std::string cid = json::parse(text(r2[0]))["id"];
    CHECK(cid.rfind("c_", 0) == 0);
    const json found = json::parse(text(r2[1]));
    CHECK(found["entries"].size() == 1);
    CHECK(found["entries"][0]["id"] == cid);
    CHECK(found["mode"] == "fts");
    CHECK(json::parse(text(r2[2]))["id"] == sid);
    CHECK(r2[3]["result"]["isError"] == true);  // a hypothesis on nothing is refused, not written
    const json st = json::parse(text(r2[4]));
    CHECK(st["claims"] == 1);
    CHECK(st["sources"] == 1);
    CHECK(st["hypotheses"] == 0);
}
