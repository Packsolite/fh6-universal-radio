#include "fh6/net/http_get.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <vector>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

namespace fh6::net {

std::optional<std::string> http_get(std::string_view url, std::string_view extra_header) {
    std::string full_url = std::string(url);
    
    // trim trailing whitespace/newlines
    while (!full_url.empty() && std::isspace(static_cast<unsigned char>(full_url.back()))) {
        full_url.pop_back();
    }
    if (full_url.empty()) return std::nullopt;
    
    // local paths to absolute paths
    if (full_url[0] == '/') {
        full_url = "http://127.0.0.1:8420" + full_url;
    }

    log::info("[http] GET '{}'", full_url);

    std::wstring wurl = subprocess::widen(full_url);

    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);
    
    wchar_t hostName[256];
    wchar_t urlPath[1024];
    
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = sizeof(hostName) / sizeof(hostName[0]);
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(urlPath[0]);

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        log::error("[http] WinHttpCrackUrl failed for {}", full_url);
        return std::nullopt;
    }

    HINTERNET hSession = WinHttpOpen(L"FH6 Universal Radio/1.0", 
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, 
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        log::error("[http] WinHttpOpen failed");
        return std::nullopt;
    }

    struct SessionGuard {
        HINTERNET h;
        ~SessionGuard() { if (h) WinHttpCloseHandle(h); }
    } sg{hSession};

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        log::error("[http] WinHttpConnect failed to host");
        return std::nullopt;
    }
    SessionGuard cg{hConnect};

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        log::error("[http] WinHttpOpenRequest failed");
        return std::nullopt;
    }
    SessionGuard rg{hRequest};

    std::wstring wheaders;
    if (!extra_header.empty()) {
        wheaders = subprocess::widen(std::string(extra_header));
    }

    if (!WinHttpSendRequest(hRequest, 
                            wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
                            wheaders.empty() ? 0 : -1,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        log::error("[http] WinHttpSendRequest failed");
        return std::nullopt;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        log::error("[http] WinHttpReceiveResponse failed");
        return std::nullopt;
    }

    // verify status code is 200
    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, 
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                        WINHTTP_HEADER_NAME_BY_INDEX, 
                        &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        log::error("[http] Non-200 HTTP Response ({}): {}", statusCode, full_url);
        return std::nullopt;
    }

    std::string body;
    DWORD dwDownloaded = 0;
    do {
        DWORD dwAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwAvailable)) {
            log::error("[http] WinHttpQueryDataAvailable failed");
            break;
        }
        if (dwAvailable == 0) break;

        std::vector<char> buffer(dwAvailable);
        if (!WinHttpReadData(hRequest, buffer.data(), dwAvailable, &dwDownloaded)) {
            log::error("[http] WinHttpReadData failed");
            break;
        }
        body.append(buffer.data(), dwDownloaded);
    } while (dwDownloaded > 0);

    return body;
}

} // namespace fh6::net
