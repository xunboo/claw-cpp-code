#include "bash.hpp"
#include <format>
#include <cstdio>
#include <cstring>
#include <array>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
extern char** environ;
#endif

namespace claw::runtime {

std::string truncate_output(std::string output) {
    if (output.size() > BASH_OUTPUT_TRUNCATE_BYTES) {
        output.resize(BASH_OUTPUT_TRUNCATE_BYTES);
        output += "\n... [output truncated]";
    }
    return output;
}

std::string prepare_command(const BashCommandInput& input, const std::vector<std::string>& sandbox_dirs) {
    std::string cmd = input.command;
    if (input.sandbox && !sandbox_dirs.empty()) {
        // No-op here: sandbox is applied at the process level by sandbox.hpp
    }
    return cmd;
}

#ifdef _WIN32

tl::expected<BashCommandOutput, std::string> execute_bash(const BashCommandInput& input) {
    // On Windows, use CreateProcess with cmd.exe /c
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdOutRead = nullptr, hStdOutWrite = nullptr;
    HANDLE hStdErrRead = nullptr, hStdErrWrite = nullptr;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0) ||
        !CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0)) {
        return tl::unexpected(std::string("CreatePipe failed"));
    }
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdOutWrite;
    si.hStdError  = hStdErrWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::string cmdline = "cmd.exe /c " + input.command;

    // Build environment block
    std::string env_block;
    for (const auto& kv : input.extra_env) {
        env_block += kv;
        env_block += '\0';
    }
    // Inherit current environment too — append current env vars
    char* current_env = GetEnvironmentStringsA();
    if (current_env) {
        char* p = current_env;
        while (*p) {
            std::string var(p);
            env_block += var;
            env_block += '\0';
            p += var.size() + 1;
        }
        FreeEnvironmentStringsA(current_env);
    }
    env_block += '\0';

    const char* cwd_ptr = nullptr;
    std::string cwd_str;
    if (input.cwd.has_value()) {
        cwd_str = *input.cwd;
        cwd_ptr = cwd_str.c_str();
    }

    BOOL ok = CreateProcessA(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE,
        0, env_block.empty() ? nullptr : env_block.data(),
        cwd_ptr, &si, &pi);

    CloseHandle(hStdOutWrite);
    CloseHandle(hStdErrWrite);

    if (!ok) {
        CloseHandle(hStdOutRead);
        CloseHandle(hStdErrRead);
        return tl::unexpected(std::format("CreateProcess failed: {}", GetLastError()));
    }

    // Read stdout/stderr
    auto read_pipe = [](HANDLE h) -> std::string {
        std::string buf;
        char tmp[4096];
        DWORD n;
        while (ReadFile(h, tmp, sizeof(tmp), &n, nullptr) && n > 0) {
            buf.append(tmp, n);
        }
        return buf;
    };

    std::string out_str, err_str;
    std::thread out_t([&]{ out_str = read_pipe(hStdOutRead); });
    std::thread err_t([&]{ err_str = read_pipe(hStdErrRead); });

    BashCommandOutput result;
    bool timed_out = false;

    DWORD wait_ms = INFINITE;
    if (input.timeout.has_value()) {
        wait_ms = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(*input.timeout).count());
    }

    DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        timed_out = true;
        result.exit_code = -1;
    } else {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        result.exit_code = static_cast<int>(exit_code);
    }

    out_t.join(); err_t.join();
    CloseHandle(hStdOutRead);
    CloseHandle(hStdErrRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result.timed_out = timed_out;
    result.stdout_output = truncate_output(std::move(out_str));
    result.stderr_output = truncate_output(std::move(err_str));
    result.truncated = (result.stdout_output.find("[output truncated]") != std::string::npos);
    return result;
}

#else
// POSIX implementation

tl::expected<BashCommandOutput, std::string> execute_bash(const BashCommandInput& input) {
    std::vector<std::string> env_copy;
    if (environ) {
        for (char** e = environ; *e; ++e) env_copy.push_back(*e);
    }
    for (const auto& kv : input.extra_env) env_copy.push_back(kv);

    int out_pipe[2], err_pipe[2];
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        return tl::unexpected(std::format("pipe(): {}", strerror(errno)));
    }

    pid_t pid = ::fork();
    if (pid < 0) return tl::unexpected(std::format("fork(): {}", strerror(errno)));

    if (pid == 0) {
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);

        if (input.cwd.has_value()) {
            if (::chdir(input.cwd->c_str()) != 0) ::_exit(1);
        }

        std::vector<const char*> envp;
        for (const auto& e : env_copy) envp.push_back(e.c_str());
        envp.push_back(nullptr);

        const char* argv[] = {"/bin/bash", "-c", input.command.c_str(), nullptr};
        ::execvpe("/bin/bash", const_cast<char**>(argv), const_cast<char**>(envp.data()));
        ::_exit(127);
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    BashCommandOutput result;

    auto read_all = [](int fd) -> std::string {
        std::string buf;
        std::array<char, 4096> tmp{};
        ssize_t n;
        while ((n = ::read(fd, tmp.data(), tmp.size())) > 0) {
            buf.append(tmp.data(), static_cast<std::size_t>(n));
        }
        return buf;
    };

    std::string out_str, err_str;
    std::thread out_t([&]{ out_str = read_all(out_pipe[0]); });
    std::thread err_t([&]{ err_str = read_all(err_pipe[0]); });

    bool timed_out = false;
    if (input.timeout.has_value()) {
        auto deadline = std::chrono::steady_clock::now() + *input.timeout;
        int status;
        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                timed_out = true;
                break;
            }
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (timed_out) result.exit_code = -1;
        else result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else {
        int status;
        ::waitpid(pid, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    out_t.join(); err_t.join();
    ::close(out_pipe[0]); ::close(err_pipe[0]);

    result.timed_out = timed_out;
    result.stdout_output = truncate_output(std::move(out_str));
    result.stderr_output = truncate_output(std::move(err_str));
    result.truncated = (result.stdout_output.find("[output truncated]") != std::string::npos);
    return result;
}

#endif

} // namespace claw::runtime
