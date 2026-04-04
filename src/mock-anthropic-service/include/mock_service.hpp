#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace claw::mock {

struct CapturedRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string scenario;
    bool        stream{false};
    std::string raw_body;
};

class MockAnthropicService {
public:
    [[nodiscard]] static std::unique_ptr<MockAnthropicService> spawn();
    [[nodiscard]] static std::unique_ptr<MockAnthropicService>
        spawn_on(std::string_view bind_addr);

    ~MockAnthropicService();

    MockAnthropicService(const MockAnthropicService&)            = delete;
    MockAnthropicService& operator=(const MockAnthropicService&) = delete;
    MockAnthropicService(MockAnthropicService&&)                 = default;
    MockAnthropicService& operator=(MockAnthropicService&&)      = default;

    [[nodiscard]] const std::string& base_url() const noexcept { return base_url_; }
    [[nodiscard]] std::vector<CapturedRequest> captured_requests() const;

private:
    explicit MockAnthropicService(std::string base_url, int server_fd);
    void accept_loop();

    std::string  base_url_;
    int          server_fd_{-1};
    mutable std::mutex           requests_mutex_;
    std::vector<CapturedRequest> requests_;
    std::atomic<bool>            stop_flag_{false};
    std::jthread                 accept_thread_;
};

}  // namespace claw::mock
