#include "fh6/net/http_get.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <memory>

namespace fh6::net {

namespace {

constexpr int kHttpTimeoutMs = 5000;

struct WinHttpDeleter {
    void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

} // namespace

std::optional<std::string> http_get(std::string_view url, std::string_view extra_header) {
    const std::wstring wurl = subprocess::widen(url);

    URL_COMPONENTS comp{};
    comp.dwStructSize      = sizeof(comp);
    comp.dwHostNameLength  = (DWORD)-1;
    comp.dwUrlPathLength   = (DWORD)-1;
    comp.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &comp) || !comp.lpszHostName) {
        log::error("[http] invalid url '{}'", url);
        return std::nullopt;
    }
    const std::wstring host(comp.lpszHostName, comp.dwHostNameLength);
    // path and the ?query that follows it are contiguous in wurl.
    std::wstring path(comp.lpszUrlPath,
                      comp.dwUrlPathLength + comp.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    WinHttpHandle session{WinHttpOpen(L"FH6 Universal Radio/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);

    WinHttpHandle conn{WinHttpConnect(session.get(), host.c_str(), comp.nPort, 0)};
    if (!conn) return std::nullopt;

    WinHttpHandle req{WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         comp.nScheme == INTERNET_SCHEME_HTTPS
                                             ? WINHTTP_FLAG_SECURE : 0)};
    if (!req) return std::nullopt;

    if (!extra_header.empty()) {
        const std::wstring h = subprocess::widen(extra_header);
        WinHttpAddRequestHeaders(req.get(), h.c_str(), (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        log::error("[http] GET {} send/receive failed (err {})", url, GetLastError());
        return std::nullopt;
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail) || avail == 0) break;
        const std::size_t off = body.size();
        body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), body.data() + off, avail, &got)) break;
        body.resize(off + got);
        if (got == 0) break;
    }

    if (status != 200) {
        log::error("[http] GET {} -> HTTP {}", url, status);
        return std::nullopt;
    }
    return body;
}

} // namespace fh6::net
