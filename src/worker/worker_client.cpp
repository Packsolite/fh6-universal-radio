#include "fh6/worker/worker_client.hpp"
#include "fh6/worker/ipc_protocol.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace fh6::worker {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WorkerClient::~WorkerClient() { stop(); }

bool WorkerClient::start(const std::filesystem::path& worker_exe) {
    std::scoped_lock lk{mu_};
    if (pipe_ != INVALID_HANDLE_VALUE) return true;  // already started

    if (!std::filesystem::exists(worker_exe)) {
        log::warn("[worker] worker exe not found at {}", worker_exe.string());
        return false;
    }

    // Launch the worker.  This is the ONE fork() from the game process.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = subprocess::quote(worker_exe.wstring());
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        log::error("[worker] failed to launch worker (err {})", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;

    // Give the worker a moment to create the control pipe.
    for (int i = 0; i < 50; ++i) {
        pipe_ = CreateFileW(kControlPipeName, GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (pipe_ == INVALID_HANDLE_VALUE) {
        log::error("[worker] could not connect to control pipe after 5 s");
        TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
        return false;
    }

    // Switch to byte-mode reading (in case the pipe was created in message mode).
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);

    log::info("[worker] connected to worker process (pid {})", pi.dwProcessId);
    return true;
}

void WorkerClient::stop() {
    std::scoped_lock lk{mu_};
    if (pipe_ != INVALID_HANDLE_VALUE) {
        // Best-effort shutdown command.
        ipc_send(pipe_, R"({"op":"shutdown"})");
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (process_) {
        // Wait up to 3 s for graceful exit, then force-kill.
        if (WaitForSingleObject(process_, 3000) == WAIT_TIMEOUT)
            TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
    }
}

bool WorkerClient::alive() const noexcept {
    std::scoped_lock lk{mu_};
    if (pipe_ == INVALID_HANDLE_VALUE || !process_) return false;
    DWORD ec = 0;
    return GetExitCodeProcess(process_, &ec) && ec == STILL_ACTIVE;
}

// ---------------------------------------------------------------------------
// IPC helpers
// ---------------------------------------------------------------------------

std::string WorkerClient::send_recv(const std::string& request) {
    // mu_ must be held by caller.
    if (pipe_ == INVALID_HANDLE_VALUE) return {};
    if (!ipc_send(pipe_, request)) {
        log::warn("[worker] send failed (pipe broken?)");
        return {};
    }
    auto resp = ipc_recv(pipe_);
    if (resp.empty()) {
        log::warn("[worker] recv failed (pipe broken?)");
    }
    return resp;
}

// ---------------------------------------------------------------------------
// Synchronous run + capture
// ---------------------------------------------------------------------------

std::string WorkerClient::run_capture(const std::wstring& cmd, bool capture_stderr) {
    std::scoped_lock lk{mu_};
    if (pipe_ == INVALID_HANDLE_VALUE) return {};

    uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    json req = {
        {"op", "run"},
        {"id", id},
        {"cmd", subprocess::narrow(cmd)},
        {"capture_stderr", capture_stderr}
    };
    auto resp_str = send_recv(req.dump());
    if (resp_str.empty()) return {};

    try {
        auto resp = json::parse(resp_str);
        if (resp.value("ok", false))
            return resp.value("output", "");
        log::warn("[worker] run failed: {}", resp.value("error", "unknown"));
    } catch (...) {
        log::warn("[worker] malformed run response");
    }
    return {};
}

// ---------------------------------------------------------------------------
// Asynchronous pipeline spawn
// ---------------------------------------------------------------------------

static HANDLE connect_to_stream(const std::wstring& pipe_name) {
    // The worker has already created the pipe and is waiting for us.
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE h = CreateFileW(pipe_name.c_str(), GENERIC_READ,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

WorkerClient::SpawnResult WorkerClient::spawn_pipeline(
    const std::vector<std::wstring>& chain, const std::wstring& side_cmd)
{
    std::scoped_lock lk{mu_};
    SpawnResult out;
    if (pipe_ == INVALID_HANDLE_VALUE) return out;

    out.pipeline_id = next_id_.fetch_add(1, std::memory_order_relaxed);

    json cmds = json::array();
    for (auto& c : chain) cmds.push_back(subprocess::narrow(c));

    json req = {
        {"op", "spawn"},
        {"id", out.pipeline_id},
        {"chain", cmds},
    };
    if (!side_cmd.empty())
        req["side_cmd"] = subprocess::narrow(side_cmd);

    auto resp_str = send_recv(req.dump());
    if (resp_str.empty()) return out;

    try {
        auto resp = json::parse(resp_str);
        if (!resp.value("ok", false)) {
            log::warn("[worker] spawn failed: {}", resp.value("error", "unknown"));
            return out;
        }
        // Connect to the data streams the worker created.
        if (resp.contains("pcm_pipe")) {
            auto name = subprocess::widen(resp["pcm_pipe"].get<std::string>());
            out.pcm_pipe = connect_to_stream(name);
        }
        if (resp.contains("meta_pipe")) {
            auto name = subprocess::widen(resp["meta_pipe"].get<std::string>());
            out.meta_pipe = connect_to_stream(name);
        }
        out.ok = (out.pcm_pipe != nullptr);
    } catch (...) {
        log::warn("[worker] malformed spawn response");
    }
    return out;
}

WorkerClient::SpawnResult WorkerClient::spawn_single(const std::wstring& cmd) {
    return spawn_pipeline({cmd});
}

void WorkerClient::kill_pipeline(uint32_t id) {
    std::scoped_lock lk{mu_};
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    json req = {{"op", "kill"}, {"id", id}};
    // Fire-and-forget: send but don't wait for response (pipeline might
    // already be dead, and we don't want to block the source destructor).
    ipc_send(pipe_, req.dump());
    // Drain the response so the pipe stays in sync.
    ipc_recv(pipe_);
}

} // namespace fh6::worker
