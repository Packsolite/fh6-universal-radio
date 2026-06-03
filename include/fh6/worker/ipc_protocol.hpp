#pragma once

// Shared definitions for the DLL ↔ worker IPC protocol.
//
// Transport: the worker is a multi-instance named-pipe server; every control
//            request opens its own short-lived byte-mode connection, so a slow
//            capture never blocks an audio spawn/kill.
// Framing:   4-byte little-endian length prefix + UTF-8 JSON body.
// Security:  pipe names embed a per-session random token, so a local process
//            can neither squat the (otherwise well-known) name nor inject
//            commands by guessing it.

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fh6::worker {

// Control channel:  \\.\pipe\fh6-radio-<token>-ctrl
inline std::wstring control_pipe_name(std::wstring_view token) {
    std::wstring s{L"\\\\.\\pipe\\fh6-radio-"};
    s.append(token).append(L"-ctrl");
    return s;
}

// Data-stream pipes:  \\.\pipe\fh6-radio-<token>-<id>-<stream>
// where <id> is the pipeline id and <stream> is "pcm" or "meta".
inline std::wstring stream_pipe_name(std::wstring_view token, uint32_t id, const wchar_t* stream) {
    std::wstring s{L"\\\\.\\pipe\\fh6-radio-"};
    s.append(token).push_back(L'-');
    s.append(std::to_wstring(id)).push_back(L'-');
    s.append(stream);
    return s;
}

// ---- wire helpers (length-prefixed JSON over a byte-mode pipe) ----------

// Write a length-prefixed message.  Returns false on pipe error.
inline bool ipc_send(HANDLE pipe, std::string_view msg) noexcept {
    uint32_t len = static_cast<uint32_t>(msg.size());
    DWORD written = 0;
    if (!WriteFile(pipe, &len, 4, &written, nullptr) || written != 4) return false;
    if (len == 0) return true;
    if (!WriteFile(pipe, msg.data(), len, &written, nullptr) || written != len) return false;
    return true;
}

// Read a length-prefixed message.  Returns empty on pipe error / EOF.
inline std::string ipc_recv(HANDLE pipe) noexcept {
    uint32_t len = 0;
    DWORD got = 0;
    if (!ReadFile(pipe, &len, 4, &got, nullptr) || got != 4) return {};
    if (len == 0) return {};
    if (len > 4 * 1024 * 1024) return {};  // 4 MB sanity cap
    std::string buf(len, '\0');
    DWORD total = 0;
    while (total < len) {
        DWORD r = 0;
        if (!ReadFile(pipe, buf.data() + total, len - total, &r, nullptr) || r == 0) return {};
        total += r;
    }
    return buf;
}

} // namespace fh6::worker
