#include "mock_service.hpp"
#include "response_builder.hpp"
#include "scenario.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using sock_t = SOCKET;
   static constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
   inline void close_sock(sock_t s) { closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using sock_t = int;
   static constexpr sock_t INVALID_SOCK = -1;
   inline void close_sock(sock_t s) { close(s); }
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace claw::mock {

// ─── helpers ────────────────────────────────────────────────────────────────

static std::string recv_all(sock_t fd) {
    std::string buf;
    std::array<char, 4096> tmp{};
    while (true) {
        auto n = recv(fd, tmp.data(), static_cast<int>(tmp.size()), 0);
        if (n <= 0) break;
        buf.append(tmp.data(), static_cast<std::size_t>(n));
        // stop when we have the full HTTP request (headers terminated by \r\n\r\n
        // plus any Content-Length body)
        auto hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos) continue;
        // parse Content-Length
        std::size_t body_start = hdr_end + 4;
        std::size_t content_len = 0;
        auto cl_pos = buf.find("Content-Length:");
        if (cl_pos == std::string::npos)
            cl_pos = buf.find("content-length:");
        if (cl_pos != std::string::npos && cl_pos < hdr_end) {
            auto val_start = buf.find_first_not_of(' ', cl_pos + 15);
            auto val_end   = buf.find("\r\n", val_start);
            std::string_view sv(buf.data() + val_start, val_end - val_start);
            std::from_chars(sv.data(), sv.data() + sv.size(), content_len);
        }
        if (buf.size() >= body_start + content_len) break;
    }
    return buf;
}

struct ParsedRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

static ParsedRequest parse_http(const std::string& raw) {
    ParsedRequest req;
    std::istringstream ss(raw);
    std::string line;

    // request line
    if (!std::getline(ss, line)) return req;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto sp1 = line.find(' ');
    auto sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp1 == sp2) return req;
    req.method = line.substr(0, sp1);
    req.path   = line.substr(sp1 + 1, sp2 - sp1 - 1);

    // headers
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // ltrim value
        auto vstart = val.find_first_not_of(' ');
        if (vstart != std::string::npos) val = val.substr(vstart);
        // lowercase key
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        req.headers[std::move(key)] = std::move(val);
    }

    // body
    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end != std::string::npos)
        req.body = raw.substr(hdr_end + 4);

    return req;
}

static void send_all(sock_t fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto n = send(fd, data.data() + sent,
                      static_cast<int>(data.size() - sent), 0);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

// ─── connection handler ──────────────────────────────────────────────────────

static CapturedRequest handle_connection(sock_t client_fd) {
    std::string raw = recv_all(client_fd);
    ParsedRequest pr = parse_http(raw);

    CapturedRequest captured;
    captured.method  = pr.method;
    captured.path    = pr.path;
    captured.headers = pr.headers;
    captured.raw_body = pr.body;

    // detect streaming flag
    auto it = pr.headers.find("accept");
    if (it != pr.headers.end() &&
        it->second.find("text/event-stream") != std::string::npos)
        captured.stream = true;

    // extract scenario from system prompt inside the JSON body
    std::optional<Scenario> scenario;
    try {
        if (!pr.body.empty()) {
            auto j = nlohmann::json::parse(pr.body, nullptr, false);
            if (!j.is_discarded()) {
                scenario = detect_scenario_from_json(j);
                if (scenario) {
                    captured.scenario = std::string(scenario_name(*scenario));

                    // also check stream field from JSON
                    if (j.contains("stream") && j["stream"].is_boolean())
                        captured.stream = j["stream"].get<bool>();
                }
            }
        }
    } catch (...) {}

    // build response
    std::string response_body;
    std::string content_type;
    int status = 200;

    if (pr.path == "/v1/messages" && pr.method == "POST" && scenario) {
        if (captured.stream) {
            auto j = nlohmann::json::parse(pr.body, nullptr, false);
            response_body = build_stream_body(j, *scenario);
            content_type  = "text/event-stream";
        } else {
            auto j = nlohmann::json::parse(pr.body, nullptr, false);
            response_body = build_json_response(j, *scenario).dump();
            content_type  = "application/json";
        }
    } else if (pr.path == "/v1/messages" && pr.method == "POST") {
        // unknown scenario — return empty SSE or minimal JSON
        if (captured.stream) {
            response_body = "data: {\"type\":\"message_stop\"}\n\n";
            content_type  = "text/event-stream";
        } else {
            nlohmann::json j = {
                {"id", "req_unknown"},
                {"type", "message"},
                {"role", "assistant"},
                {"content", nlohmann::json::array()},
                {"model", DEFAULT_MODEL},
                {"stop_reason", "end_turn"},
                {"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}
            };
            response_body = j.dump();
            content_type  = "application/json";
        }
    } else {
        status = 404;
        response_body = "{\"error\":\"not found\"}";
        content_type  = "application/json";
    }

    // HTTP response
    std::ostringstream http;
    http << "HTTP/1.1 " << status
         << (status == 200 ? " OK" : " Not Found") << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << response_body.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << response_body;

    send_all(client_fd, http.str());
    close_sock(client_fd);

    return captured;
}

// ─── MockAnthropicService ────────────────────────────────────────────────────

MockAnthropicService::MockAnthropicService(std::string base_url, int server_fd)
    : base_url_(std::move(base_url))
    , server_fd_(server_fd)
    , accept_thread_([this] { accept_loop(); })
{}

MockAnthropicService::~MockAnthropicService() {
    stop_flag_.store(true, std::memory_order_relaxed);
    // wake the blocking accept() by closing the fd
    if (server_fd_ >= 0) {
        close_sock(static_cast<sock_t>(server_fd_));
        server_fd_ = -1;
    }
    // jthread destructor joins automatically
}

std::vector<CapturedRequest> MockAnthropicService::captured_requests() const {
    std::lock_guard lk(requests_mutex_);
    return requests_;
}

void MockAnthropicService::accept_loop() {
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        sock_t client = accept(static_cast<sock_t>(server_fd_), nullptr, nullptr);
        if (client == INVALID_SOCK) break;  // shutdown or error

        // handle on a detached thread so the accept loop keeps running
        std::thread([this, client] {
            auto cap = handle_connection(client);
            std::lock_guard lk(requests_mutex_);
            requests_.push_back(std::move(cap));
        }).detach();
    }
}

// ─── factory functions ───────────────────────────────────────────────────────

std::unique_ptr<MockAnthropicService> MockAnthropicService::spawn() {
    return spawn_on("127.0.0.1:0");
}

std::unique_ptr<MockAnthropicService>
MockAnthropicService::spawn_on(std::string_view bind_addr) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    // parse host:port
    std::string host;
    int         port = 0;
    auto colon = bind_addr.rfind(':');
    if (colon != std::string_view::npos) {
        host = std::string(bind_addr.substr(0, colon));
        std::string_view port_sv = bind_addr.substr(colon + 1);
        std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), port);
    } else {
        host = std::string(bind_addr);
    }

    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK)
        throw std::runtime_error("socket() failed");

    // allow port reuse
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(fd);
        throw std::runtime_error("bind() failed");
    }
    if (listen(fd, SOMAXCONN) != 0) {
        close_sock(fd);
        throw std::runtime_error("listen() failed");
    }

    // retrieve the actual bound port (important when port == 0)
    sockaddr_in bound{};
    socklen_t   bound_len = sizeof(bound);
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    int actual_port = ntohs(bound.sin_port);

    std::string base_url =
        "http://" + host + ":" + std::to_string(actual_port);

    return std::unique_ptr<MockAnthropicService>(
        new MockAnthropicService(std::move(base_url), static_cast<int>(fd)));
}

}  // namespace claw::mock
