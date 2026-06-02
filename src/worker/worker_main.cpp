// fh6-radio-worker: lightweight process that spawns yt-dlp / ffmpeg on behalf
// of the game-injected DLL.  Lives outside the multi-GB game address space so
// the fork() that Wine/Proton performs inside CreateProcess is cheap (~5 MB
// instead of several GB), eliminating the in-game stutter that occurs on every
// track change.
//
// Protocol: length-prefixed JSON over \\.\pipe\fh6-radio-ctrl.
// Data streams: per-pipeline named pipes \\.\pipe\fh6-radio-<id>-pcm/meta.

#include "fh6/worker/ipc_protocol.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using namespace fh6::worker;
using namespace fh6::subprocess;

// ---------------------------------------------------------------------------
// Pipeline: a set of child processes + proxy threads for one audio track.
// ---------------------------------------------------------------------------

struct ProxyData {
    HANDLE source;
    std::atomic<HANDLE> dest;
    ProxyData(HANDLE s, HANDLE d) : source(s), dest(d) {}
};

struct Pipeline {
    uint32_t id = 0;
    HANDLE job = nullptr;
    std::vector<HANDLE> processes;
    std::vector<std::unique_ptr<ProxyData>> proxies_data;
    std::vector<HANDLE> anon_sources;    // read-ends fed to proxy threads
    std::vector<std::thread> proxies;

    void kill() {
        // 1. Terminate children and their entire process trees explicitly.
        // This is crucial for PyInstaller executables (yt-dlp) under Wine,
        // which spawn child payloads that bypass simple TerminateProcess.
        for (HANDLE h : processes) {
            if (h) {
                DWORD pid = GetProcessId(h);
                if (pid) kill_process_tree(pid);
                TerminateProcess(h, 1); // fallback if GetProcessId fails
                CloseHandle(h);
            }
        }
        processes.clear();

        if (job) { CloseHandle(job); job = nullptr; }

        // 2. Unblock any ConnectNamedPipe() calls by closing the named pipe handles.
        for (auto& pd : proxies_data) {
            HANDLE h = pd->dest.exchange(nullptr, std::memory_order_acq_rel);
            if (h) { DisconnectNamedPipe(h); CloseHandle(h); }
        }
        proxies_data.clear();

        // 3. Now that handles are broken/closed, proxy threads will exit their loops.
        for (auto& t : proxies) {
            if (t.joinable()) t.join();
        }
        proxies.clear();

        // 4. Finally, close our read-ends of the anonymous pipes.
        for (HANDLE h : anon_sources) {
            if (h) CloseHandle(h);
        }
        anon_sources.clear();
    }

    ~Pipeline() { kill(); }
};

static std::mutex g_mu;
static std::unordered_map<uint32_t, std::unique_ptr<Pipeline>> g_pipelines;

// ---------------------------------------------------------------------------
// Proxy thread: reads from an anonymous pipe and writes to a named pipe.
// The named pipe must have ConnectNamedPipe called first (client connects
// after receiving the "ready" response on the control channel).
// ---------------------------------------------------------------------------

static void proxy_thread_fn(ProxyData* data) {
    HANDLE initial_dest = data->dest.load(std::memory_order_acquire);
    if (!initial_dest) return;

    // Wait for the DLL to connect (should happen within ~100ms of the
    // "ready" response being sent).
    ConnectNamedPipe(initial_dest, nullptr);  // no-op if already connected

    char buf[8192];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(data->source, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        
        HANDLE current_dest = data->dest.load(std::memory_order_acquire);
        if (!current_dest) break; // killed

        DWORD written = 0;
        if (!WriteFile(current_dest, buf, got, &written, nullptr)) break;
    }
    
    // Child process EOF or client disconnected. We must flush before closing
    // because Wine discards unread named pipe buffers on CloseHandle.
    // FlushFileBuffers will block until the DLL reads everything or until
    // the DLL closes its end of the pipe (which it now does before calling kill_pipeline).
    HANDLE h = data->dest.exchange(nullptr, std::memory_order_acq_rel);
    if (h) {
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
}

// ---------------------------------------------------------------------------
// Create a named pipe server instance for a data stream.
// ---------------------------------------------------------------------------

static HANDLE create_stream_pipe(const std::wstring& name) {
    return CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_OUTBOUND,                      // server writes, client reads
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,        // max instances
        1 << 20,  // out buffer 1 MB
        0,        // in buffer (not used for outbound)
        0,        // default timeout
        nullptr);
}

// ---------------------------------------------------------------------------
// Handle "run" — synchronous command execution with output capture.
// ---------------------------------------------------------------------------

static json handle_run(const json& req) {
    auto cmd_u8 = req.at("cmd").get<std::string>();
    bool capture_stderr = req.value("capture_stderr", false);

    HANDLE job = create_kill_on_close_job();
    if (!job) return {{"ok", false}, {"error", "CreateJobObject failed"}};

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        CloseHandle(job);
        return {{"ok", false}, {"error", "CreatePipe failed"}};
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE nul_out = open_nul(GENERIC_WRITE);

    HANDLE h_stdout = capture_stderr ? nul_out : wr;
    HANDLE h_stderr = capture_stderr ? wr : open_stderr_log();

    std::wstring wcmd = widen(cmd_u8);
    HANDLE proc = spawn_in_job(job, wcmd, nul_in, h_stdout, h_stderr);
    DWORD spawn_ec = proc ? 0 : GetLastError();

    CloseHandle(wr);
    if (nul_in)  CloseHandle(nul_in);
    if (nul_out) CloseHandle(nul_out);
    if (!capture_stderr && h_stderr) CloseHandle(h_stderr);

    if (!proc) {
        CloseHandle(rd);
        CloseHandle(job);
        return {{"ok", false},
                {"error", describe_launch_failure(wcmd.substr(0, wcmd.find(L' ')), spawn_ec, false)}};
    }

    // Drain output.
    std::string output;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        output.append(buf, got);

    CloseHandle(rd);
    WaitForSingleObject(proc, 10000);
    CloseHandle(proc);
    CloseHandle(job);

    return {{"ok", true}, {"output", std::move(output)}};
}

// ---------------------------------------------------------------------------
// Handle "spawn" — asynchronous pipeline with named-pipe data streams.
// ---------------------------------------------------------------------------

static json handle_spawn(const json& req) {
    uint32_t id = req.at("id").get<uint32_t>();
    auto chain = req.at("chain").get<std::vector<std::string>>();
    if (chain.empty())
        return {{"ok", false}, {"error", "empty chain"}};

    auto pl = std::make_unique<Pipeline>();
    pl->id  = id;
    pl->job = create_kill_on_close_job();

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    // Build the chain: each command's stdout feeds the next command's stdin.
    // The last command's stdout goes to a named pipe for the DLL to read.
    HANDLE prev_read = nul_in;  // first command reads from NUL

    for (size_t i = 0; i < chain.size(); ++i) {
        bool is_last = (i == chain.size() - 1);

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) {
            if (nul_in)  CloseHandle(nul_in);
            if (err_log) CloseHandle(err_log);
            return {{"ok", false}, {"error", "CreatePipe failed"}};
        }
        
        // If this is the last process, 'rd' will be consumed by our proxy thread,
        // so it MUST NOT be inherited by the child (otherwise EOF won't trigger).
        // If it's an intermediate process, 'rd' will be the stdin of the NEXT process,
        // so it MUST be inherited.
        if (is_last) {
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        }

        std::wstring wcmd = widen(chain[i]);
        HANDLE proc = spawn_in_job(pl->job, wcmd, prev_read, wr, err_log);
        CloseHandle(wr);

        // Close the previous pipe read-end now that the child inherited it
        // (unless it was nul_in, which we keep for the side command).
        if (prev_read != nul_in) CloseHandle(prev_read);

        if (!proc) {
            CloseHandle(rd);
            if (nul_in)  CloseHandle(nul_in);
            if (err_log) CloseHandle(err_log);
            return {{"ok", false}, {"error", "spawn failed for step " + std::to_string(i)}};
        }
        pl->processes.push_back(proc);

        if (is_last) {
            // Create named pipe for PCM and start proxy thread.
            auto pipe_name = stream_pipe_name(id, L"pcm");
            HANDLE np = create_stream_pipe(pipe_name);
            if (np == INVALID_HANDLE_VALUE) {
                CloseHandle(rd);
                if (nul_in)  CloseHandle(nul_in);
                if (err_log) CloseHandle(err_log);
                return {{"ok", false}, {"error", "CreateNamedPipe failed for pcm"}};
            }
            pl->proxies_data.push_back(std::make_unique<ProxyData>(rd, np));
            pl->anon_sources.push_back(rd);
            pl->proxies.emplace_back(proxy_thread_fn, pl->proxies_data.back().get());
        } else {
            prev_read = rd;  // feed to next command's stdin
        }
    }

    json resp = {{"ok", true},
                 {"pcm_pipe", narrow(stream_pipe_name(id, L"pcm"))}};

    // Optional side command (title resolver): its stdout goes to a separate
    // named pipe so the DLL can drain metadata independently.
    if (req.contains("side_cmd")) {
        auto side_u8 = req.at("side_cmd").get<std::string>();
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE srd = nullptr, swr = nullptr;
        if (CreatePipe(&srd, &swr, &sa, 1 << 16)) {
            SetHandleInformation(srd, HANDLE_FLAG_INHERIT, 0);
            std::wstring wcmd = widen(side_u8);
            HANDLE sp = spawn_in_job(pl->job, wcmd, nul_in, swr, err_log);
            CloseHandle(swr);
            if (sp) {
                pl->processes.push_back(sp);
                auto meta_name = stream_pipe_name(id, L"meta");
                HANDLE mnp = create_stream_pipe(meta_name);
                if (mnp != INVALID_HANDLE_VALUE) {
                    pl->proxies_data.push_back(std::make_unique<ProxyData>(srd, mnp));
                    pl->anon_sources.push_back(srd);
                    pl->proxies.emplace_back(proxy_thread_fn, pl->proxies_data.back().get());
                    resp["meta_pipe"] = narrow(meta_name);
                } else {
                    CloseHandle(srd);
                }
            } else {
                CloseHandle(srd);
            }
        }
    }

    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);

    std::scoped_lock lk{g_mu};
    g_pipelines[id] = std::move(pl);
    return resp;
}

// ---------------------------------------------------------------------------
// Handle "kill" — terminate a pipeline.
// ---------------------------------------------------------------------------

static json handle_kill(const json& req) {
    uint32_t id = req.at("id").get<uint32_t>();
    std::unique_ptr<Pipeline> pl;
    {
        std::scoped_lock lk{g_mu};
        auto it = g_pipelines.find(id);
        if (it != g_pipelines.end()) {
            pl = std::move(it->second);
            g_pipelines.erase(it);
        }
    }
    // Pipeline destructor calls kill() which terminates children + joins threads.
    return {{"ok", true}};
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // Create control pipe server.
    HANDLE ctrl = CreateNamedPipeW(
        kControlPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, nullptr);

    if (ctrl == INVALID_HANDLE_VALUE) return 1;

    // Wait for the DLL to connect.
    if (!ConnectNamedPipe(ctrl, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(ctrl);
        return 1;
    }

    // Message loop.
    for (;;) {
        auto msg_str = ipc_recv(ctrl);
        if (msg_str.empty()) break;  // DLL disconnected or pipe broken

        json resp;
        try {
            auto msg = json::parse(msg_str);
            auto op  = msg.at("op").get<std::string>();

            if (op == "shutdown") {
                resp = {{"ok", true}};
                ipc_send(ctrl, resp.dump());
                break;
            }
            if (op == "run")   resp = handle_run(msg);
            else if (op == "spawn") resp = handle_spawn(msg);
            else if (op == "kill")  resp = handle_kill(msg);
            else resp = {{"ok", false}, {"error", "unknown op"}};
        } catch (const std::exception& e) {
            resp = {{"ok", false}, {"error", e.what()}};
        }
        if (!ipc_send(ctrl, resp.dump())) break;
    }

    // Cleanup: kill all pipelines.
    {
        std::scoped_lock lk{g_mu};
        g_pipelines.clear();  // destructors terminate children
    }

    CloseHandle(ctrl);
    return 0;
}
