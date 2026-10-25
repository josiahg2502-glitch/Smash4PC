#pragma once

// The account server (Pretendo's account.pretendo.cc): an OAuth token for the
// player's network account, then the tokens titles ask nn_act for, such as
// the NEX token naming a game server and the credentials to log in to it.
// Requests as the console makes them (after Cemu's napi_act.cpp, MPL 2.0).

#include <array>
#include <cstdint>
#include <string>

namespace cafe::os::act_server {

struct Result {
    bool ok = false;
    int error_code = 0; // the server's, when it refused
    std::string message;
};

struct NexToken : Result {
    std::string host, password, token;
    uint16_t port = 0;
    uint32_t pid = 0;
};

NexToken nex_token(uint64_t title_id, uint16_t title_version, uint32_t game_server_id);

struct ServiceToken : Result {
    std::string token;
};
ServiceToken service_token(uint64_t title_id, uint16_t title_version, const std::string& client_id);

// Signing in with a Pretendo ID and its password (the launcher's Online page), with the console's
// files loaded: the account's principal id, the console's password hash for it (what account.dat
// keeps; the password itself is never stored) and its Mii from the profile.
struct SignIn : Result {
    std::string user_id;
    uint32_t pid = 0;
    std::array<uint8_t, 32> password_hash{};
    std::array<uint8_t, 96> mii{};
    std::array<uint16_t, 11> mii_name{};
    std::string country; // "US", ...
};
SignIn sign_in(const std::string& user, const std::string& password);

} // namespace cafe::os::act_server
