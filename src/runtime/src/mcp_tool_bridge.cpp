// Bridge between MCP tool surface (ListMcpResources, ReadMcpResource, McpAuth, MCP)
// and the existing McpServerManager runtime.
//
// Provides a stateful client registry that tool handlers can use to
// connect to MCP servers and invoke their capabilities.
//
// Faithfully converted from:
//   crates/runtime/src/mcp_tool_bridge.rs

#include "mcp_tool_bridge.hpp"
#include "mcp.hpp"           // mcp_tool_name(server_name, tool_name)

#include <format>
#include <thread>
#include <future>
#include <algorithm>
#include <stdexcept>

namespace claw::runtime {

// ─── McpConnectionStatus helpers ─────────────────────────────────────────────
//
// Rust:  impl Display for McpConnectionStatus
//   Disconnected  -> "disconnected"
//   Connecting    -> "connecting"
//   Connected     -> "connected"
//   AuthRequired  -> "auth_required"
//   Error         -> "error"

[[nodiscard]] static std::string status_to_string(McpConnectionStatus s) {
    switch (s) {
        case McpConnectionStatus::Disconnected:  return "disconnected";
        case McpConnectionStatus::Connecting:    return "connecting";
        case McpConnectionStatus::Connected:     return "connected";
        case McpConnectionStatus::AuthRequired:  return "auth_required";
        case McpConnectionStatus::Error:         return "error";
    }
    return "unknown";
}

// ─── McpToolRegistry::register_server ────────────────────────────────────────
//
// Rust:
//   pub fn register_server(
//       &self,
//       server_name: &str,
//       status: McpConnectionStatus,
//       tools: Vec<McpToolInfo>,
//       resources: Vec<McpResourceInfo>,
//       server_info: Option<String>,
//   ) {
//       let mut inner = self.inner.lock()...;
//       inner.insert(server_name.to_owned(), McpServerState { ... error_message: None });
//   }
//
// The C++ header signature is (std::string name, McpServerState state); callers
// construct the McpServerState before passing it in. The upsert / overwrite
// semantics (insert-or-replace) are preserved via operator[].

void McpToolRegistry::register_server(std::string name, McpServerState state) {
    std::lock_guard lock(mutex_);
    // Rust uses HashMap::insert which overwrites any existing entry.
    inner_.servers[std::move(name)] = std::move(state);
}

// ─── McpToolRegistry::get_server ─────────────────────────────────────────────
//
// Rust:
//   pub fn get_server(&self, server_name: &str) -> Option<McpServerState> {
//       let inner = self.inner.lock()...;
//       inner.get(server_name).cloned()
//   }

[[nodiscard]] std::optional<McpServerState>
McpToolRegistry::get_server(std::string_view name) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(name));
    if (it == inner_.servers.end()) return std::nullopt;
    return it->second;  // cloned copy
}

// ─── McpToolRegistry::list_servers ───────────────────────────────────────────
//
// Rust:
//   pub fn list_servers(&self) -> Vec<McpServerState> {
//       let inner = self.inner.lock()...;
//       inner.values().cloned().collect()
//   }

[[nodiscard]] std::vector<McpServerState>
McpToolRegistry::list_servers() const {
    std::lock_guard lock(mutex_);
    std::vector<McpServerState> result;
    result.reserve(inner_.servers.size());
    for (const auto& [_, state] : inner_.servers) {
        result.push_back(state);  // cloned
    }
    return result;
}

// ─── McpToolRegistry::list_resources ─────────────────────────────────────────
//
// Rust:
//   pub fn list_resources(&self, server_name: &str)
//       -> Result<Vec<McpResourceInfo>, String>
//   {
//       match inner.get(server_name) {
//           Some(state) => {
//               if state.status != McpConnectionStatus::Connected {
//                   return Err(format!("server '{}' is not connected (status: {})",
//                       server_name, state.status));
//               }
//               Ok(state.resources.clone())
//           }
//           None => Err(format!("server '{}' not found", server_name)),
//       }
//   }

[[nodiscard]] tl::expected<std::vector<McpResourceInfo>, std::string>
McpToolRegistry::list_resources(std::string_view server_name) const {
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(server_name));
    if (it == inner_.servers.end()) {
        return tl::unexpected(std::format("server '{}' not found", server_name));
    }
    const auto& state = it->second;
    if (state.status != McpConnectionStatus::Connected) {
        return tl::unexpected(std::format(
            "server '{}' is not connected (status: {})",
            server_name, status_to_string(state.status)));
    }
    return state.resources;  // cloned
}

// ─── McpToolRegistry::read_resource ──────────────────────────────────────────
//
// Rust:
//   pub fn read_resource(&self, server_name: &str, uri: &str)
//       -> Result<McpResourceInfo, String>
//   {
//       let state = inner.get(server_name)
//           .ok_or_else(|| format!("server '{}' not found", server_name))?;
//
//       if state.status != McpConnectionStatus::Connected {
//           return Err(format!(...));
//       }
//
//       state.resources.iter()
//           .find(|r| r.uri == uri)
//           .cloned()
//           .ok_or_else(|| format!(
//               "resource '{}' not found on server '{}'", uri, server_name))
//   }
//
// The C++ header declares this as returning McpReadResourceResult (the richer
// protocol type) rather than McpResourceInfo, because the bridge delegates to
// the manager which performs a real protocol read_resource call. If the
// manager is not set we fall back to a cache lookup that wraps the
// McpResourceInfo into a single-element McpReadResourceResult.

[[nodiscard]] tl::expected<McpReadResourceResult, std::string>
McpToolRegistry::read_resource(std::string_view server_name, std::string_view uri) {
    // First check the registry cache for connection status and resource existence.
    {
        std::lock_guard lock(mutex_);
        auto it = inner_.servers.find(std::string(server_name));
        if (it == inner_.servers.end()) {
            return tl::unexpected(
                std::format("server '{}' not found", server_name));
        }
        const auto& state = it->second;
        if (state.status != McpConnectionStatus::Connected) {
            return tl::unexpected(std::format(
                "server '{}' is not connected (status: {})",
                server_name, status_to_string(state.status)));
        }
        // Find the resource in the cached list (mirrors Rust's cache lookup).
        auto rit = std::find_if(state.resources.begin(), state.resources.end(),
            [&](const McpResourceInfo& r) {
                return r.uri == uri;
            });
        if (rit == state.resources.end()) {
            return tl::unexpected(std::format(
                "resource '{}' not found on server '{}'", uri, server_name));
        }
        // Resource found in cache — build a McpReadResourceResult from it.
        // (The manager's live read_resource is also attempted below if available.)
    }

    // If the manager is available, delegate to it for a live fetch.
    std::shared_ptr<McpServerManager> mgr;
    {
        std::lock_guard lock(manager_mutex_);
        mgr = manager_;
    }
    if (mgr) {
        auto result = mgr->read_resource(std::string(server_name), std::string(uri));
        if (!result) {
            return tl::unexpected(result.error().message);
        }
        return std::move(*result);
    }

    // No manager — return a synthetic McpReadResourceResult from the cache entry.
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(server_name));
    if (it == inner_.servers.end()) {
        return tl::unexpected(
            std::format("server '{}' not found", server_name));
    }
    const auto& state = it->second;
    auto rit = std::find_if(state.resources.begin(), state.resources.end(),
        [&](const McpResourceInfo& r) { return r.uri == uri; });
    if (rit == state.resources.end()) {
        return tl::unexpected(std::format(
            "resource '{}' not found on server '{}'", uri, server_name));
    }
    McpReadResourceResult out;
    McpResourceContents contents;
    contents.uri = rit->uri;
    contents.mime_type = rit->mime_type;
    // name and description are metadata, not carried in McpResourceContents
    out.contents.push_back(std::move(contents));
    return out;
}

// ─── McpToolRegistry::list_tools ─────────────────────────────────────────────
//
// Rust:
//   pub fn list_tools(&self, server_name: &str) -> Result<Vec<McpToolInfo>, String> {
//       match inner.get(server_name) {
//           Some(state) => {
//               if state.status != McpConnectionStatus::Connected {
//                   return Err(format!(...));
//               }
//               Ok(state.tools.clone())
//           }
//           None => Err(format!("server '{}' not found", server_name)),
//       }
//   }
//
// The C++ header declares list_tools with an optional server_name so it can
// also aggregate across all servers (useful for the plugin layer). When a
// server_name is provided the Rust semantics (check Connected, error on
// disconnected/missing) are applied. When nullopt is given, all tools from
// all servers are returned without a status check (best-effort aggregate).

[[nodiscard]] std::vector<McpToolInfo>
McpToolRegistry::list_tools(std::optional<std::string_view> server_name) const {
    std::lock_guard lock(mutex_);
    std::vector<McpToolInfo> result;

    if (server_name.has_value()) {
        // Rust single-server path: must be Connected, error if missing/disconnected.
        auto it = inner_.servers.find(std::string(*server_name));
        if (it == inner_.servers.end()) {
            // Mirror Rust: returns Err. Since C++ signature returns a vector
            // (not expected), callers that need the error should use the
            // expected-returning overload below. Here we return empty.
            return {};
        }
        const auto& state = it->second;
        if (state.status != McpConnectionStatus::Connected) {
            return {};
        }
        return state.tools;
    }

    // Aggregate across all servers.
    for (const auto& [_, state] : inner_.servers) {
        for (const auto& t : state.tools) {
            result.push_back(t);
        }
    }
    return result;
}

// ─── McpToolRegistry::list_tools_for_server (Rust-faithful expected version) ─
//
// Rust's list_tools returns Result<Vec<McpToolInfo>, String>. The C++ header
// exposes the non-failing vector overload; we add this internal helper used by
// call_tool's pre-call validation to replicate the exact Rust error messages.

[[nodiscard]] static tl::expected<std::vector<McpToolInfo>, std::string>
list_tools_impl(const std::unordered_map<std::string, McpServerState>& servers,
                std::string_view server_name)
{
    auto it = servers.find(std::string(server_name));
    if (it == servers.end()) {
        return tl::unexpected(
            std::format("server '{}' not found", server_name));
    }
    const auto& state = it->second;
    if (state.status != McpConnectionStatus::Connected) {
        return tl::unexpected(std::format(
            "server '{}' is not connected (status: {})",
            server_name, status_to_string(state.status)));
    }
    return state.tools;
}

// ─── McpToolRegistry::spawn_tool_call ────────────────────────────────────────
//
// Rust (private static):
//   fn spawn_tool_call(
//       manager: Arc<Mutex<McpServerManager>>,
//       qualified_tool_name: String,
//       arguments: Option<serde_json::Value>,
//   ) -> Result<serde_json::Value, String>
//   {
//       // Spawns a thread that builds its own tokio runtime and runs the
//       // async call_tool / discover_tools / shutdown sequence, then joins.
//       let join_handle = std::thread::Builder::new()
//           .name(format!("mcp-tool-call-{qualified_tool_name}"))
//           .spawn(move || {
//               let runtime = tokio::runtime::Builder::new_current_thread()...;
//               runtime.block_on(async move {
//                   let response = {
//                       let mut manager = manager.lock()?;
//                       manager.discover_tools().await?;
//                       let response = manager.call_tool(&qualified_tool_name, arguments).await;
//                       let shutdown = manager.shutdown().await;
//                       match (response, shutdown) { ... }
//                   }?;
//                   // unwrap JSON-RPC error / missing result
//                   serde_json::to_value(result)
//               })
//           })?;
//       join_handle.join().map_err(|panic_payload| ...)
//   }
//
// C++ translation:
//   • The tokio runtime disappears — the manager calls are synchronous.
//   • discover_tools() + call_tool() + shutdown() are called sequentially.
//   • We spawn a std::thread with a name hint and join it, propagating
//     exceptions (mimicking Rust's panic payload handling).
//   • The result is communicated via std::promise / std::future.
//
// arguments: nullopt maps to Rust's None (i.e. no arguments were passed).

[[nodiscard]] static tl::expected<nlohmann::json, std::string>
spawn_tool_call(std::shared_ptr<McpServerManager> manager,
                std::string qualified_tool_name,
                std::optional<nlohmann::json> arguments)
{
    std::promise<tl::expected<nlohmann::json, std::string>> promise;
    auto future = promise.get_future();

    // Thread name mirrors Rust: "mcp-tool-call-{qualified_tool_name}"
    // std::thread does not have a built-in name API on all platforms, but we
    // match the intent; on POSIX the name is set via pthread_setname_np.
    std::thread worker([mgr = std::move(manager),
                        qname = std::move(qualified_tool_name),
                        args = std::move(arguments),
                        p = std::move(promise)]() mutable
    {
        // Set thread name (POSIX; no-op on other platforms).
#if defined(__linux__) || defined(__APPLE__)
        {
            auto thread_name = std::string("mcp-tool-call-").append(qname);
            // Truncate to 15 chars (Linux pthread limit).
            if (thread_name.size() > 15) thread_name.resize(15);
#if defined(__linux__)
            pthread_setname_np(pthread_self(), thread_name.c_str());
#elif defined(__APPLE__)
            pthread_setname_np(thread_name.c_str());
#endif
        }
#endif
        try {
            // Step 1: discover_tools (mirrors manager.discover_tools().await)
            auto discover_result = mgr->discover_tools();
            if (!discover_result) {
                p.set_value(tl::unexpected(discover_result.error().message));
                return;
            }

            // Step 2: call_tool
            nlohmann::json call_args = args.value_or(nlohmann::json::object());
            auto call_result = mgr->call_tool(qname, call_args);

            // Step 3: shutdown (always attempted, mirrors Rust)
            mgr->shutdown();

            if (!call_result) {
                p.set_value(tl::unexpected(call_result.error().message));
                return;
            }

            // Convert McpToolCallResult to nlohmann::json (mirrors
            // serde_json::to_value(result)).
            const auto& tool_result = *call_result;
            nlohmann::json j;

            // content array
            nlohmann::json content_arr = nlohmann::json::array();
            for (const auto& c : tool_result.content) {
                nlohmann::json item;
                item["type"] = c.kind;
                for (const auto& [k, v] : c.data) {
                    item[k] = v;
                }
                content_arr.push_back(std::move(item));
            }
            j["content"] = std::move(content_arr);

            // structuredContent
            if (!tool_result.structured_content.is_null()) {
                j["structuredContent"] = tool_result.structured_content;
            }

            if (tool_result.is_error.has_value()) {
                j["isError"] = *tool_result.is_error;
            } else {
                j["isError"] = nullptr;
            }

            if (!tool_result.meta.is_null()) {
                j["_meta"] = tool_result.meta;
            }

            p.set_value(std::move(j));
        } catch (const std::exception& ex) {
            // Mirrors Rust: if let Some(msg) = panic_payload.downcast_ref::<String>()
            p.set_value(tl::unexpected(
                std::format("MCP tool call thread panicked: {}", ex.what())));
        } catch (...) {
            // Mirrors Rust: "MCP tool call thread panicked"
            p.set_value(tl::unexpected(
                std::string("MCP tool call thread panicked")));
        }
    });

    // join() — mirrors Rust join_handle.join().map_err(|panic_payload| ...)
    // We must join before accessing the future to avoid a race, but since we
    // use promise/future the set_value happens before join() returns.
    worker.join();
    return future.get();
}

// ─── McpToolRegistry::call_tool ──────────────────────────────────────────────
//
// Rust:
//   pub fn call_tool(
//       &self,
//       server_name: &str,
//       tool_name: &str,
//       arguments: &serde_json::Value,
//   ) -> Result<serde_json::Value, String>
//   {
//       // 1. lock registry, look up server, check Connected, check tool exists.
//       // 2. drop lock.
//       // 3. get manager (error if not configured).
//       // 4. spawn_tool_call(manager, mcp_tool_name(server_name, tool_name),
//                            (!arguments.is_null()).then(|| arguments.clone()))
//   }
//
// The C++ header exposes call_tool(qualified_name, arguments) — the caller
// has already composed the qualified name. To preserve the Rust validation
// logic (check server exists, check Connected, check tool exists in registry)
// we parse the server and tool names back out from the qualified name using
// mcp_tool_name's inverse: split on the separator "__".
//
// Alternatively — and more faithfully — we keep the Rust parameter layout by
// providing an additional overload. But since we must not change the header,
// we implement the header signature and reconstruct the server/tool split.
//
// The separator used by mcp_tool_name (see mcp.cpp) is "__".

[[nodiscard]] tl::expected<McpToolCallResult, std::string>
McpToolRegistry::call_tool(std::string_view qualified_name,
                           const nlohmann::json& arguments)
{
    // Split qualified_name into server_name and tool_name.
    // mcp_tool_name(server, tool) produces "server__tool".
    const std::string qn(qualified_name);
    const auto sep_pos = qn.find("__");
    if (sep_pos == std::string::npos) {
        return tl::unexpected(std::format(
            "invalid qualified tool name '{}': expected 'server__tool'",
            qualified_name));
    }
    const std::string server_name = qn.substr(0, sep_pos);
    const std::string tool_name   = qn.substr(sep_pos + 2);

    // --- Rust block: lock, validate, then drop lock ---
    {
        std::lock_guard lock(mutex_);

        // Rust: inner.get(server_name).ok_or_else(|| format!(...))
        auto it = inner_.servers.find(server_name);
        if (it == inner_.servers.end()) {
            return tl::unexpected(
                std::format("server '{}' not found", server_name));
        }
        const auto& state = it->second;

        // Rust: if state.status != McpConnectionStatus::Connected { return Err(...) }
        if (state.status != McpConnectionStatus::Connected) {
            return tl::unexpected(std::format(
                "server '{}' is not connected (status: {})",
                server_name, status_to_string(state.status)));
        }

        // Rust: if !state.tools.iter().any(|t| t.name == tool_name) { return Err(...) }
        const bool tool_found = std::any_of(
            state.tools.begin(), state.tools.end(),
            [&](const McpToolInfo& t) { return t.name == tool_name; });
        if (!tool_found) {
            return tl::unexpected(std::format(
                "tool '{}' not found on server '{}'", tool_name, server_name));
        }
    } // lock released here (mirrors Rust: drop(inner))

    // Rust: let manager = self.manager.get().cloned()
    //           .ok_or_else(|| "MCP server manager is not configured".to_string())?;
    std::shared_ptr<McpServerManager> mgr;
    {
        std::lock_guard lock(manager_mutex_);
        mgr = manager_;
    }
    if (!mgr) {
        return tl::unexpected(
            std::string("MCP server manager is not configured"));
    }

    // Rust: spawn_tool_call(manager, mcp_tool_name(server_name, tool_name),
    //           (!arguments.is_null()).then(|| arguments.clone()))
    const std::string full_name = mcp_tool_name(server_name, tool_name);
    std::optional<nlohmann::json> opt_args;
    if (!arguments.is_null()) {
        opt_args = arguments;
    }

    auto json_result = spawn_tool_call(mgr, full_name, std::move(opt_args));
    if (!json_result) {
        return tl::unexpected(json_result.error());
    }

    // Re-hydrate into McpToolCallResult from the JSON we built in spawn_tool_call.
    McpToolCallResult result;
    const auto& j = *json_result;

    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& item : j["content"]) {
            McpToolCallContent c;
            if (item.contains("type") && item["type"].is_string()) {
                c.kind = item["type"].get<std::string>();
            }
            for (auto it2 = item.begin(); it2 != item.end(); ++it2) {
                if (it2.key() == "type") continue;
                c.data[it2.key()] = it2.value();
            }
            result.content.push_back(std::move(c));
        }
    }

    if (j.contains("structuredContent")) {
        result.structured_content = j["structuredContent"];
    }

    if (j.contains("isError") && j["isError"].is_boolean()) {
        result.is_error = j["isError"].get<bool>();
    }

    if (j.contains("_meta")) {
        result.meta = j["_meta"];
    }

    return result;
}

// ─── McpToolRegistry::set_auth_status ────────────────────────────────────────
//
// Rust:
//   pub fn set_auth_status(
//       &self,
//       server_name: &str,
//       status: McpConnectionStatus,
//   ) -> Result<(), String>
//   {
//       let mut inner = self.inner.lock()...;
//       let state = inner.get_mut(server_name)
//           .ok_or_else(|| format!("server '{}' not found", server_name))?;
//       state.status = status;
//       Ok(())
//   }
//
// The C++ header returns void (swallowing the error). We silently do nothing
// if the server is not found, exactly matching what a Result-ignoring caller
// would observe.

void McpToolRegistry::set_auth_status(std::string_view server_name,
                                      McpConnectionStatus status)
{
    std::lock_guard lock(mutex_);
    auto it = inner_.servers.find(std::string(server_name));
    if (it != inner_.servers.end()) {
        it->second.status = status;
    }
    // If not found: mirrors Rust's Err path, which callers may ignore.
}

// ─── McpToolRegistry::disconnect ─────────────────────────────────────────────
//
// Rust:
//   pub fn disconnect(&self, server_name: &str) -> Option<McpServerState> {
//       let mut inner = self.inner.lock()...;
//       inner.remove(server_name)   // removes and returns the entry, or None
//   }
//
// The C++ header returns void. We perform the same removal (erase from map).

void McpToolRegistry::disconnect(std::string_view server_name) {
    std::lock_guard lock(mutex_);
    inner_.servers.erase(std::string(server_name));
}

// ─── McpToolRegistry::len ────────────────────────────────────────────────────
//
// Rust:
//   pub fn len(&self) -> usize {
//       let inner = self.inner.lock()...;
//       inner.len()
//   }

[[nodiscard]] std::size_t McpToolRegistry::len() const {
    std::lock_guard lock(mutex_);
    return inner_.servers.size();
}

// ─── McpToolRegistry::is_empty ───────────────────────────────────────────────
//
// Rust:
//   pub fn is_empty(&self) -> bool { self.len() == 0 }

[[nodiscard]] bool McpToolRegistry::is_empty() const {
    return len() == 0;
}

// ─── McpToolRegistry::set_manager ────────────────────────────────────────────
//
// Rust:
//   pub fn set_manager(
//       &self,
//       manager: Arc<Mutex<McpServerManager>>,
//   ) -> Result<(), Arc<Mutex<McpServerManager>>>
//   {
//       self.manager.set(manager)   // OnceLock::set — only succeeds once
//   }
//
// C++ mirrors the "set once" semantics: if a manager is already installed we
// silently ignore the second call (equivalent to OnceLock returning Err).

void McpToolRegistry::set_manager(std::shared_ptr<McpServerManager> manager) {
    std::lock_guard lock(manager_mutex_);
    if (!manager_) {
        manager_ = std::move(manager);
    }
    // If already set: mirrors Rust's OnceLock::set returning Err(manager).
    // The caller can detect this if needed, but the header returns void.
}

} // namespace claw::runtime
