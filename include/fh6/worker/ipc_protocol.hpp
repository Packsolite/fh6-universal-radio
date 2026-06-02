#pragma once

// Shared definitions for the DLL ↔ worker IPC protocol.
//
// Transport: a single bidirectional named pipe in byte mode.
// Framing:   4-byte little-endian length prefix + UTF-8 JSON body.
// The worker creates the pipe; the DLL connects as a client.

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fh6::worker {

// Control channel.  One instance, bidirectional.
constexpr const wchar_t* kControlPipeName = L"\\\\.\\pipe\\fh6-radio-ctrl";

// Data-stream pipes are named   \\.\pipe\fh6-radio-<id>-<stream>
// where <id> is the pipeline id and <stream> is "pcm" or "meta".
inline std::wstring stream_pipe_name(uint32_t id, const wchar_t* stream) {
    return std::wstring{L"\\\\.\\pipe\\fh6-radio-"} +
           std::to_wstring(id) + L"-" + stream;
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
