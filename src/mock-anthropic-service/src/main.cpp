#include "mock_service.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string_view>

namespace {

std::atomic<bool> g_shutdown{false};
std::mutex        g_shutdown_mutex;
std::condition_variable g_shutdown_cv;

void signal_handler(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_relaxed);
    g_shutdown_cv.notify_all();
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string_view bind_addr = "127.0.0.1:0";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if ((arg == "--bind" || arg == "-b") && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (arg.starts_with("--bind=")) {
            bind_addr = arg.substr(7);
        } else {
            std::cerr << "Usage: " << argv[0]
                      << " [--bind <host:port>]\n";
            return EXIT_FAILURE;
        }
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::unique_ptr<claw::mock::MockAnthropicService> svc;
    try {
        svc = claw::mock::MockAnthropicService::spawn_on(bind_addr);
    } catch (const std::exception& e) {
        std::cerr << "Failed to start mock service: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    // Print the base URL so callers can discover the ephemeral port
    std::cout << svc->base_url() << "\n";
    std::cout.flush();

    // Wait for shutdown signal
    std::unique_lock lk(g_shutdown_mutex);
    g_shutdown_cv.wait(lk, [] { return g_shutdown.load(std::memory_order_relaxed); });

    return EXIT_SUCCESS;
}
