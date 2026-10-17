// beliefd — HTTP front end: JSON over the same calls as the library and the
// CLI, plus what a shared service must add: who is asking (actor tokens),
// what they may write (their own domains), what they may see (sensitivity),
// batches per request, and quiet hours for the embedding GPU.
#pragma once

#include <beliefdb/store.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace beliefd {

struct ActorConfig {
    std::string name;
    std::vector<std::string> domains;  // the actor writes here; private nodes of other domains are invisible to it
    bool human = false;                // a human sees and may change everything and needs no batch
    std::string token;                 // from the environment, never from the file
};

struct Config {
    std::string db = "belief.sqlite";
    std::string listen = "127.0.0.1";
    int port = 7420;
    beliefdb::Policy policy;                           // require_batch applies to agents only
    std::optional<beliefdb::EmbeddingConfig> embeddings;
    std::string quiet_hours;                           // "23:00-08:00" local time: no embedding calls, the GPU sleeps
    std::vector<ActorConfig> actors;
    std::vector<std::string> warnings;                 // actors dropped for lack of a token, etc.

    using Getenv = std::function<std::string(const std::string&)>;
    /// Tokens come from the environment as BELIEFD_TOKEN_<NAME> (upper case, '-' -> '_');
    /// an actor without one is dropped with a warning. The file holds no secrets.
    static Config from_json(const nlohmann::json&, const Getenv& getenv_fn);
    static Config load(const std::string& path, const Getenv& getenv_fn);
};

/// True when `hhmm` (minutes since midnight) falls inside "HH:MM-HH:MM" (wrapping midnight).
bool in_quiet_hours(const std::string& spec, int minutes_of_day);

class Server {
public:
    explicit Server(Config);
    ~Server();
    /// Binds (port 0 = any free port) and serves on a background thread. Returns the port.
    int start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace beliefd
