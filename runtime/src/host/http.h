#pragma once

// Blocking HTTP(S) requests from the host (WinHTTP on Windows): for the
// runtime's own calls to network services, not the title's sockets.

#include <string>
#include <vector>

namespace cafe::host {

struct HttpResponse {
    int status = 0;    // 0: no response (error says why)
    std::string body;
    std::string error;
};

// `headers` are "Name: value" lines. Server certificates are verified.
HttpResponse http_request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                          const std::string& body);

} // namespace cafe::host
