// ORM rows and the storage definition. Rows are deliberately plain (strings,
// doubles, optionals); enum/JSON conversion lives in store.cpp.
#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wc++26-extensions"
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"
#endif
#include <sqlite_orm/sqlite_orm.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace beliefdb::schema {

struct NodeRow {
    std::string id;
    std::string type;
    std::string domain;
    std::string created_at;
    std::string updated_at;
    std::string status;
    std::string sensitivity;
    std::string payload;  // JSON
};

struct EdgeRow {
    std::string src;
    std::string dst;
    std::string type;
    std::optional<std::string> rule_id;
    double weight = 1.0;
    std::string created_at;
};

struct CredenceRow {
    std::string node_id;
    double value = 0;
    std::string method;
    std::string inputs;  // JSON
    std::string computed_at;
    int stale = 0;
};

struct EvidenceRow {
    std::string node_id;
    std::vector<char> source_set;  // sorted source ids joined by '\n'
    int n_sources = 0;
    int depth = 0;
    std::string computed_at;
};

struct PredicateRow {
    std::string name;
    std::string value_type;
    std::string unit;
    std::string enum_values;  // JSON array
    std::string note;
    std::string created_at;
};

struct StaleRow {
    std::string node_id;
    std::string cause;  // node id whose retraction/replacement reached node_id
    std::string reason;
    std::string at;
};

struct RevisionRow {
    int64_t id = 0;
    std::string node_id;
    std::string at;
    std::string actor;
    std::optional<std::string> before;  // JSON
    std::optional<std::string> after;   // JSON
    std::string reason;
};

inline auto make_storage(const std::string& path) {
    using namespace sqlite_orm;
    return sqlite_orm::make_storage(
        path,
        make_table("nodes",
                   make_column("id", &NodeRow::id, primary_key()),
                   make_column("type", &NodeRow::type),
                   make_column("domain", &NodeRow::domain),
                   make_column("created_at", &NodeRow::created_at),
                   make_column("updated_at", &NodeRow::updated_at),
                   make_column("status", &NodeRow::status),
                   make_column("sensitivity", &NodeRow::sensitivity),
                   make_column("payload", &NodeRow::payload)),
        make_table("edges",
                   make_column("src", &EdgeRow::src),
                   make_column("dst", &EdgeRow::dst),
                   make_column("type", &EdgeRow::type),
                   make_column("rule_id", &EdgeRow::rule_id),
                   make_column("weight", &EdgeRow::weight),
                   make_column("created_at", &EdgeRow::created_at),
                   primary_key(&EdgeRow::src, &EdgeRow::dst, &EdgeRow::type)),
        make_table("credence",
                   make_column("node_id", &CredenceRow::node_id, primary_key()),
                   make_column("value", &CredenceRow::value),
                   make_column("method", &CredenceRow::method),
                   make_column("inputs", &CredenceRow::inputs),
                   make_column("computed_at", &CredenceRow::computed_at),
                   make_column("stale", &CredenceRow::stale)),
        make_table("evidence",
                   make_column("node_id", &EvidenceRow::node_id, primary_key()),
                   make_column("source_set", &EvidenceRow::source_set),
                   make_column("n_sources", &EvidenceRow::n_sources),
                   make_column("depth", &EvidenceRow::depth),
                   make_column("computed_at", &EvidenceRow::computed_at)),
        make_table("predicates",
                   make_column("name", &PredicateRow::name, primary_key()),
                   make_column("value_type", &PredicateRow::value_type),
                   make_column("unit", &PredicateRow::unit),
                   make_column("enum_values", &PredicateRow::enum_values),
                   make_column("note", &PredicateRow::note),
                   make_column("created_at", &PredicateRow::created_at)),
        make_table("stale_marks",
                   make_column("node_id", &StaleRow::node_id),
                   make_column("cause", &StaleRow::cause),
                   make_column("reason", &StaleRow::reason),
                   make_column("at", &StaleRow::at),
                   primary_key(&StaleRow::node_id, &StaleRow::cause)),
        make_table("revisions",
                   make_column("id", &RevisionRow::id, primary_key().autoincrement()),
                   make_column("node_id", &RevisionRow::node_id),
                   make_column("at", &RevisionRow::at),
                   make_column("actor", &RevisionRow::actor),
                   make_column("before", &RevisionRow::before),
                   make_column("after", &RevisionRow::after),
                   make_column("reason", &RevisionRow::reason)),
        make_index("idx_nodes_domain_type_status", &NodeRow::domain, &NodeRow::type, &NodeRow::status),
        make_index("idx_edges_dst_type", &EdgeRow::dst, &EdgeRow::type),
        make_index("idx_edges_src_type", &EdgeRow::src, &EdgeRow::type),
        make_index("idx_revisions_node", &RevisionRow::node_id));
}

using Storage = decltype(make_storage(""));

}  // namespace beliefdb::schema
