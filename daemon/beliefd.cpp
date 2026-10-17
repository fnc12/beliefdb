// beliefd — the HTTP service around beliefdb. Usage: beliefd --config PATH (or $BELIEFD_CONFIG).
#include "server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) path = argv[++i];
        else if (a == "-h" || a == "--help") { std::cout << "usage: beliefd --config PATH   (or $BELIEFD_CONFIG)\n"; return 0; }
    }
    if (path.empty()) if (const char* env = std::getenv("BELIEFD_CONFIG")) path = env;
    if (path.empty()) { std::cerr << "beliefd: --config PATH or $BELIEFD_CONFIG is required\n"; return 1; }
    try {
        auto cfg = beliefd::Config::load(path, [](const std::string& k) { const char* v = std::getenv(k.c_str()); return v ? std::string(v) : std::string(); });
        for (const auto& w : cfg.warnings) std::cerr << "beliefd: warning: " << w << '\n';
        if (cfg.actors.empty()) { std::cerr << "beliefd: no actor has a token; nothing could ever call this server\n"; return 1; }
        beliefd::Server server(cfg);
        const int port = server.start();
        std::cerr << "beliefd: db " << cfg.db << ", listening on " << cfg.listen << ":" << port << ", " << cfg.actors.size() << " actors"
                  << (cfg.embeddings ? ", embeddings " + cfg.embeddings->base_url + " (" + cfg.embeddings->model + ")" : ", full-text only")
                  << (cfg.quiet_hours.empty() ? "" : ", quiet hours " + cfg.quiet_hours) << '\n';
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        server.stop();
        std::cerr << "beliefd: stopped\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "beliefd: " << e.what() << '\n';
        return 1;
    }
}
