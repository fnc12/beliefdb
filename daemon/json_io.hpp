// beliefd — JSON <-> library inputs and nodes, shared by the HTTP server and the MCP server.
#pragma once

#include <beliefdb/store.hpp>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace beliefd {

/// An input error with the HTTP status it maps to; the MCP server reports the code and message.
struct HttpError {
    int status;
    std::string code;
    std::string message;
};

template <class E>
E enum_or(std::optional<E> v, const char* what, const std::string& text) {
    if (!v) throw HttpError{400, "bad_input", std::string("unknown ") + what + ": " + text};
    return *v;
}

beliefdb::Value parse_value(const nlohmann::json& j);
std::vector<std::string> ids_of(const nlohmann::json& j, const char* key);
std::string need_str(const nlohmann::json& j, const char* key);
beliefdb::SourceInput parse_source(const nlohmann::json& j);
beliefdb::ClaimInput parse_claim(const nlohmann::json& j);
beliefdb::DeriveInput parse_derive(const nlohmann::json& j);
beliefdb::HypothesisInput parse_hypothesis(const nlohmann::json& j);
nlohmann::json node_json(const beliefdb::Node& n);

}  // namespace beliefd
