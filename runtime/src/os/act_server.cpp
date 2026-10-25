#include "os/act_server.h"

#include "host/digest.h"
#include "host/http.h"
#include "os/online_identity.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace cafe::os::act_server {
namespace {

constexpr const char* kServer = "https://account.pretendo.cc";

std::string hex(uint64_t v, int digits) {
    char s[32];
    std::snprintf(s, sizeof s, "%0*llx", digits, static_cast<unsigned long long>(v));
    return s;
}

// The text of the first <tag> in an XML response.
std::string tag(const std::string& xml, const std::string& name) {
    const size_t open = xml.find("<" + name + ">");
    if (open == std::string::npos) return {};
    const size_t start = open + name.size() + 2, end = xml.find("</" + name + ">", start);
    return end == std::string::npos ? std::string() : xml.substr(start, end - start);
}

std::vector<std::string> common_headers(uint64_t title_id, uint16_t title_version) {
    const online::Device& d = online::device();
    return {
        "X-Nintendo-Platform-ID: 1",
        "X-Nintendo-Device-Type: 2",
        "X-Nintendo-Client-ID: a2efa818a34fa16b8afbc8a74eba3eda",
        "X-Nintendo-Client-Secret: c91cdb5658bd4954ade78533a339cf9a",
        "Accept: */*",
        std::string("X-Nintendo-System-Version: ") + (d.region == 2 ? "0270" : "0260"),
        "X-Nintendo-Device-ID: " + std::to_string(d.device_id),
        "X-Nintendo-Serial-Number: " + d.serial,
        "X-Nintendo-Region: " + std::to_string(d.region),
        // shortcut: country from the region (US/JP/GB); map account.dat's Country code when another one matters.
        std::string("X-Nintendo-Country: ") + (d.region == 1 ? "JP" : d.region == 4 ? "GB" : "US"),
        "X-Nintendo-FPD-Version: 0000",
        "X-Nintendo-Environment: L1",
        "X-Nintendo-Title-ID: " + hex(title_id, 16),
        "X-Nintendo-Unique-ID: " + hex((title_id >> 8) & 0xFFFFF, 5),
        "X-Nintendo-Application-Version: " + hex(title_version, 4),
    };
}

// A refusal's <errors><error><code>, or a transport failure.
bool failed(const host::HttpResponse& r, Result& out, const char* what) {
    if (r.status == 0) {
        out.message = r.error;
    } else if (r.body.find("<errors>") != std::string::npos || r.status >= 400) {
        out.error_code = std::atoi(tag(r.body, "code").c_str());
        out.message = tag(r.body, "message");
    } else {
        return false;
    }
    std::fprintf(stderr, "ttt2: online: %s failed: error %d, %s (HTTP %d)\n", what, out.error_code, out.message.c_str(),
                 r.status);
    return true;
}

std::mutex g_mutex;
std::string g_oauth;
std::chrono::steady_clock::time_point g_oauth_expires;

// An access token for `user` with the console's password hash (`hash`, hex).
std::string login(uint64_t title_id, uint16_t title_version, const std::string& user, const std::string& hash,
                  Result& out, int& expires) {
    std::vector<std::string> headers = common_headers(title_id, title_version);
    headers.push_back("X-Nintendo-Device-Cert: " + online::device().certificate);
    headers.push_back("Content-type: application/x-www-form-urlencoded");
    const std::string body = "grant_type=password&user_id=" + user + "&password=" + hash + "&password_type=hash";
    const host::HttpResponse r =
        host::http_request("POST", std::string(kServer) + "/v1/api/oauth20/access_token/generate", headers, body);
    if (failed(r, out, "account login")) return {};
    expires = std::atoi(tag(r.body, "expires_in").c_str());
    const std::string token = tag(r.body, "token");
    if (token.empty()) out.message = "no token in the response";
    return token;
}

// The account's access token (cached until shortly before it expires).
std::string oauth_token(uint64_t title_id, uint16_t title_version, Result& out) {
    if (!g_oauth.empty() && std::chrono::steady_clock::now() < g_oauth_expires) return g_oauth;
    const online::Account& a = online::account();
    std::string hash;
    for (uint8_t b : a.password_hash) hash += hex(b, 2);
    int expires = 0;
    g_oauth = login(title_id, title_version, a.account_id, hash, out, expires);
    g_oauth_expires = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(expires - 30, 0));
    if (!g_oauth.empty()) std::fprintf(stderr, "ttt2: online: signed in to Pretendo as %s\n", a.account_id.c_str());
    return g_oauth;
}

} // namespace

NexToken nex_token(uint64_t title_id, uint16_t title_version, uint32_t game_server_id) {
    std::lock_guard lock(g_mutex);
    NexToken t;
    if (!online::ready()) {
        t.message = "no console files or account";
        return t;
    }
    const std::string oauth = oauth_token(title_id, title_version, t);
    if (oauth.empty()) return t;
    std::vector<std::string> headers = common_headers(title_id, title_version);
    headers.push_back("Authorization: Bearer " + oauth);
    char server[16];
    std::snprintf(server, sizeof server, "%08X", game_server_id);
    const host::HttpResponse r = host::http_request(
        "GET", std::string(kServer) + "/v1/api/provider/nex_token/@me?game_server_id=" + server, headers, {});
    if (failed(r, t, "NEX token")) return t;
    t.host = tag(r.body, "host");
    t.password = tag(r.body, "nex_password");
    t.token = tag(r.body, "token");
    t.port = static_cast<uint16_t>(std::atoi(tag(r.body, "port").c_str()));
    t.pid = static_cast<uint32_t>(std::strtoul(tag(r.body, "pid").c_str(), nullptr, 10));
    t.ok = !t.host.empty() && !t.token.empty();
    std::fprintf(stderr, "ttt2: online: NEX token for server %s: %s\n", server,
                 t.ok ? ("game server " + t.host + ":" + std::to_string(t.port)).c_str() : "empty");
    return t;
}

ServiceToken service_token(uint64_t title_id, uint16_t title_version, const std::string& client_id) {
    std::lock_guard lock(g_mutex);
    ServiceToken t;
    if (!online::ready()) {
        t.message = "no console files or account";
        return t;
    }
    const std::string oauth = oauth_token(title_id, title_version, t);
    if (oauth.empty()) return t;
    std::vector<std::string> headers = common_headers(title_id, title_version);
    headers.push_back("Authorization: Bearer " + oauth);
    const host::HttpResponse r = host::http_request(
        "GET", std::string(kServer) + "/v1/api/provider/service_token/@me?client_id=" + client_id, headers, {});
    if (failed(r, t, "service token")) return t;
    t.token = tag(r.body, "token");
    t.ok = !t.token.empty();
    return t;
}

namespace {

std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += "%" + hex(c, 2);
    }
    return out;
}

std::vector<uint8_t> base64_decode(const std::string& text) {
    std::vector<uint8_t> out;
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        const int v = c >= 'A' && c <= 'Z'   ? c - 'A'
                      : c >= 'a' && c <= 'z' ? c - 'a' + 26
                      : c >= '0' && c <= '9' ? c - '0' + 52
                      : c == '+'             ? 62
                      : c == '/'             ? 63
                                             : -1;
        if (v < 0) continue;
        bits = bits << 6 | static_cast<uint32_t>(v);
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(static_cast<uint8_t>(bits >> count));
        }
    }
    return out;
}

// XML text escapes Pretendo uses in names.
std::string unescape(std::string s) {
    static const std::pair<const char*, const char*> kEscapes[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : kEscapes) {
        const size_t from_len = std::strlen(from), to_len = std::strlen(to);
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to_len) s.replace(at, from_len, to);
    }
    return s;
}

} // namespace

SignIn sign_in(const std::string& user, const std::string& password) {
    std::lock_guard lock(g_mutex);
    SignIn r;
    if (!online::device().present) {
        r.message = "the console's otp.bin and seeprom.bin are needed first";
        return r;
    }
    constexpr uint64_t kTitle = 0x0005001010040100ull; // the system's account settings, where a console signs in
    // 1. The account's principal id (the password hash is salted with it).
    const host::HttpResponse ids = host::http_request(
        "GET",
        std::string(kServer) + "/v1/api/admin/mapped_ids?input_type=user_id&output_type=pid&input=" + url_encode(user),
        common_headers(kTitle, 0), {});
    if (failed(ids, r, "Pretendo ID lookup")) return r;
    r.pid = static_cast<uint32_t>(std::strtoul(tag(ids.body, "out_id").c_str(), nullptr, 10));
    if (r.pid == 0) {
        r.message = "no Pretendo ID named " + user;
        return r;
    }
    // 2. The console's password hash: SHA-256 of the pid (little-endian), 02 65 43 46 and the password.
    std::vector<uint8_t> salted = {uint8_t(r.pid), uint8_t(r.pid >> 8), uint8_t(r.pid >> 16), uint8_t(r.pid >> 24),
                                   0x02, 0x65, 0x43, 0x46};
    salted.insert(salted.end(), password.begin(), password.end());
    host::sha256(salted.data(), salted.size(), r.password_hash.data());
    std::string hash;
    for (uint8_t b : r.password_hash) hash += hex(b, 2);
    // 3. Sign in; 4. the profile: the exact user id, the Mii and the country.
    int expires = 0;
    const std::string token = login(kTitle, 0, user, hash, r, expires);
    if (token.empty()) {
        if (r.error_code == 106) r.message = "wrong Pretendo ID or password";
        return r;
    }
    std::vector<std::string> headers = common_headers(kTitle, 0);
    headers.push_back("Authorization: Bearer " + token);
    const host::HttpResponse profile =
        host::http_request("GET", std::string(kServer) + "/v1/api/people/@me/profile", headers, {});
    if (failed(profile, r, "profile")) return r;
    r.user_id = unescape(tag(profile.body, "user_id"));
    if (r.user_id.empty()) r.user_id = user;
    r.country = tag(profile.body, "country");
    const size_t mii = profile.body.find("<mii>");
    const std::string mii_xml = mii == std::string::npos ? std::string() : profile.body.substr(mii);
    const std::vector<uint8_t> data = base64_decode(tag(mii_xml, "data"));
    std::memcpy(r.mii.data(), data.data(), std::min(data.size(), r.mii.size()));
    // The Mii's name, UTF-8 to UTF-16 (at most 10 characters).
    const std::string name = unescape(tag(mii_xml, "name"));
    size_t n = 0;
    for (size_t i = 0; i < name.size() && n < 10;) {
        uint32_t c = static_cast<unsigned char>(name[i++]);
        if (c >= 0xE0 && i + 1 < name.size()) {
            c = (c & 0x0F) << 12 | (name[i] & 0x3F) << 6 | (name[i + 1] & 0x3F);
            i += 2;
        } else if (c >= 0xC0 && i < name.size()) {
            c = (c & 0x1F) << 6 | (name[i] & 0x3F);
            ++i;
        }
        r.mii_name[n++] = static_cast<uint16_t>(c);
    }
    r.ok = true;
    g_oauth.clear(); // the next request signs in with the saved account
    return r;
}

} // namespace cafe::os::act_server
