#include "host/rooms.h"

#include "host/http.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

namespace cafe::host::netplay {

std::string room_server() {
    const char* server = std::getenv("SMASH_ROOM_SERVER");
    return server && *server ? server : kRoomServer;
}

uint32_t random_id() {
    std::random_device device;
    return std::uniform_int_distribution<uint32_t>(1, 0xFFFFFF)(device);
}

std::string ip_text(uint32_t ip) {
    char text[16];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return text;
}

namespace {

uint32_t parse_ip(const std::string& text) {
    unsigned a, b, c, d;
    return std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? (a << 24 | b << 16 | c << 8 | d) : 0;
}

} // namespace

// The server's protocol (server/worker/src/index.js): text lines each way.
CheckInReply check_in(const CheckIn& c) {
    char line[160];
    std::string body = "v 1\n";
    std::snprintf(line, sizeof line, "id %06x\n", c.id);
    body += line;
    if (c.pub.ip) body += "pub " + ip_text(c.pub.ip) + " " + std::to_string(c.pub.port) + "\n";
    if (c.lan.ip) body += "lan " + ip_text(c.lan.ip) + " " + std::to_string(c.lan.port) + "\n";
    if (c.room.players > 0) {
        std::string name;
        for (char ch : c.room.name) name += (ch == '\t' || ch == '\n' || ch == '\r') ? ' ' : ch;
        body += "room " + std::to_string(c.room.max) + " " + std::to_string(c.room.players) + " " + kRoomBuild + "\t" +
                name.substr(0, 40) + "\n";
    }
    if (c.want) {
        std::snprintf(line, sizeof line, "want %06x\n", c.want);
        body += line;
    }
    if (c.list) body += "list 1\n";
    CheckInReply reply;
    const HttpResponse r = http_request("POST", room_server() + "/hello", {"Content-Type: text/plain"}, body);
    if (r.status != 200) return reply;
    std::istringstream in(r.body);
    std::string text;
    while (std::getline(in, text)) {
        const size_t tab = text.find('\t');
        std::istringstream words(text.substr(0, tab));
        std::string kind;
        words >> kind;
        if (kind == "ok") {
            reply.ok = true;
        } else if (kind == "peer") {
            std::string id, pub_ip, lan_ip;
            unsigned pub_port = 0, lan_port = 0;
            words >> id >> pub_ip >> pub_port >> lan_ip >> lan_port;
            reply.peers.push_back({static_cast<uint32_t>(std::strtoul(id.c_str(), nullptr, 16)),
                                   {parse_ip(pub_ip), static_cast<uint16_t>(pub_port)},
                                   {parse_ip(lan_ip), static_cast<uint16_t>(lan_port)}});
        } else if (kind == "room" && tab != std::string::npos) {
            std::string id, build;
            Room room;
            words >> id >> room.max >> room.players >> build;
            if (build != kRoomBuild) continue;
            room.id = static_cast<uint32_t>(std::strtoul(id.c_str(), nullptr, 16));
            room.name = text.substr(tab + 1);
            reply.rooms.push_back(room);
        }
    }
    return reply;
}

bool list_rooms(std::vector<Room>& rooms) {
    static const uint32_t id = random_id();
    CheckIn c;
    c.id = id;
    c.list = true;
    const CheckInReply r = check_in(c);
    if (!r.ok) return false;
    rooms = r.rooms;
    return true;
}

} // namespace cafe::host::netplay
