#include "host/http.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace cafe::host {

#ifdef _WIN32
namespace {

std::wstring wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(std::max(n, 0));
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

} // namespace

HttpResponse http_request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                          const std::string& body) {
    HttpResponse r;
    const std::wstring wurl = wide(url);
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        r.error = "bad URL";
        return r;
    }
    Handle session{WinHttpOpen(L"Smash 4 PC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) session.h = WinHttpOpen(L"Smash 4 PC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 30000);
    Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring target = std::wstring(path) + extra;
    Handle request{connection.h ? WinHttpOpenRequest(connection.h, wide(method).c_str(), target.c_str(), nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr};
    if (!request.h) {
        r.error = "cannot open a connection (" + std::to_string(GetLastError()) + ")";
        return r;
    }
    std::wstring all_headers;
    for (const std::string& h : headers) all_headers += wide(h) + L"\r\n";
    if (!WinHttpSendRequest(request.h, all_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : all_headers.c_str(),
                            all_headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = "no response (" + std::to_string(GetLastError()) + ")";
        return r;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) break;
        const size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, r.body.data() + at, available, &read)) break;
        r.body.resize(at + read);
    }
    return r;
}
#else
// shortcut: no HTTP client off Windows yet; use libcurl when the Linux build gets online play.
HttpResponse http_request(const std::string&, const std::string&, const std::vector<std::string>&, const std::string&) {
    return {0, {}, "no HTTP client on this platform"};
}
#endif

} // namespace cafe::host
