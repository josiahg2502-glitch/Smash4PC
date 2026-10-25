#include "os/online_identity.h"

#include "os/sect233.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string_view>
#include <vector>

namespace cafe::os::online {
namespace {

Account g_account;
Device g_device;

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

uint64_t hex(std::string_view s) {
    uint64_t v = 0;
    for (char c : s) {
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + d;
    }
    return v;
}

template <size_t N>
void hex_bytes(std::string_view s, std::array<uint8_t, N>& out) {
    for (size_t i = 0; i < N && 2 * i + 2 <= s.size(); ++i) out[i] = static_cast<uint8_t>(hex(s.substr(2 * i, 2)));
}

// account.dat: `Key=value` lines, numbers in hex (Cemu's Account::ParseFile).
void parse_account(const std::vector<uint8_t>& data) {
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    while (!text.empty()) {
        const size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "AccountId") g_account.account_id = value;
        else if (key == "AccountPasswordCache") hex_bytes(value, g_account.password_hash);
        else if (key == "PrincipalId") g_account.principal_id = static_cast<uint32_t>(hex(value));
        else if (key == "PersistentId") g_account.persistent_id = static_cast<uint32_t>(hex(value));
        else if (key == "Country") g_account.country = static_cast<uint32_t>(hex(value));
        else if (key == "SimpleAddressId") g_account.simple_address_id = static_cast<uint32_t>(hex(value));
        else if (key == "TransferableIdBase") g_account.transferable_id_base = hex(value);
        else if (key == "UtcOffset") g_account.utc_offset = hex(value);
        else if (key == "Uuid") hex_bytes(value, g_account.uuid);
        else if (key == "MiiData") hex_bytes(value, g_account.mii);
        else if (key == "MiiName") {
            for (size_t i = 0; i < g_account.mii_name.size() && 4 * i + 4 <= value.size(); ++i) {
                g_account.mii_name[i] = static_cast<uint16_t>(hex(value.substr(4 * i, 4)));
            }
        }
    }
    bool hashed = false;
    for (uint8_t b : g_account.password_hash) hashed |= b != 0;
    g_account.present = !g_account.account_id.empty() && g_account.principal_id != 0 && hashed;
}

// The device certificate from the OTP (Cemu's iosuCrypto_generateDeviceCertificate): signature, CA and MS ids,
// the NG name and key id, and the public key of the console's private key.
void make_device(const std::vector<uint8_t>& otp, const std::vector<uint8_t>& seeprom) {
    const auto otp_word = [&](int index) {
        return uint32_t(otp[index * 4]) << 24 | otp[index * 4 + 1] << 16 | otp[index * 4 + 2] << 8 | otp[index * 4 + 3];
    };
    g_device.device_id = otp_word(0x87);
    char code[9] = {}, number[17] = {};
    std::memcpy(code, seeprom.data() + 0xAC * 2, 8);
    std::memcpy(number, seeprom.data() + 0xB0 * 2, 16);
    g_device.serial = std::string(code) + number;
    g_device.region = seeprom[0xA4 * 2 + 3];

    std::array<uint8_t, 0x180> cert{};
    cert[0] = 0x00, cert[1] = 0x01, cert[2] = 0x00, cert[3] = 0x05; // ECC with SHA-256
    std::memcpy(cert.data() + 0x004, otp.data() + 0xA3 * 4, 0x3C);
    std::snprintf(reinterpret_cast<char*>(cert.data() + 0x080), 0x40, "Root-CA%08x-MS%08x", otp_word(0xA1), otp_word(0xA0));
    cert[0x0C3] = 0x02;
    std::snprintf(reinterpret_cast<char*>(cert.data() + 0x0C4), 0x40, "NG%08x", otp_word(0x87));
    std::memcpy(cert.data() + 0x104, otp.data() + 0xA2 * 4, 4);
    sect233::Bytes30 key{}, x, y;
    std::memcpy(key.data(), otp.data() + 0x88 * 4, 30);
    sect233::public_key(key, x, y);
    std::memcpy(cert.data() + 0x108, x.data(), 30);
    std::memcpy(cert.data() + 0x108 + 30, y.data(), 30);
    g_device.certificate = base64(cert.data(), cert.size());
    g_device.present = g_device.device_id != 0;
}

} // namespace

std::string base64(const uint8_t* data, size_t size) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = uint32_t(data[i]) << 16 | (i + 1 < size ? data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out += kChars[n >> 18];
        out += kChars[(n >> 12) & 63];
        out += i + 1 < size ? kChars[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kChars[n & 63] : '=';
    }
    return out;
}

void load(const std::filesystem::path& folder) {
    const std::vector<uint8_t> otp = read_file(folder / "otp.bin"), seeprom = read_file(folder / "seeprom.bin");
    if (otp.size() == 1024 && seeprom.size() == 512) make_device(otp, seeprom);
    parse_account(read_file(folder / "account.dat"));
    std::fprintf(stderr, "ttt2: online: console files %s, network account %s\n",
                 g_device.present ? "found" : "missing (otp.bin, seeprom.bin)",
                 g_account.present ? g_account.account_id.c_str() : "missing (account.dat)");
}

bool save_account(const std::filesystem::path& folder, const NewAccount& a) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    // A fresh UUID and transferable id for this account on this "console".
    std::array<uint8_t, 16> uuid{};
    std::random_device rd;
    for (uint8_t& b : uuid) b = static_cast<uint8_t>(rd());
    const uint64_t transferable = (uint64_t(rd()) << 32) | rd();
    const auto hex_bytes = [](const uint8_t* p, size_t n) {
        std::string s;
        char b[3];
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(b, sizeof b, "%02x", p[i]);
            s += b;
        }
        return s;
    };
    std::string name;
    for (uint16_t c : a.mii_name) {
        char b[5];
        std::snprintf(b, sizeof b, "%04x", c);
        name += b;
    }
    char line[96];
    std::string text = "AccountInstance_20120705\n";
    text += "PersistentId=80000001\n";
    std::snprintf(line, sizeof line, "TransferableIdBase=%llx\n", static_cast<unsigned long long>(transferable));
    text += line;
    text += "Uuid=" + hex_bytes(uuid.data(), uuid.size()) + "\n";
    text += "MiiData=" + hex_bytes(a.mii.data(), a.mii.size()) + "\n";
    text += "MiiName=" + name + "\n";
    text += "AccountId=" + a.user_id + "\n";
    text += std::string("Country=") + (a.us ? "31" : "0") + "\n";
    std::snprintf(line, sizeof line, "PrincipalId=%x\n", a.pid);
    text += line;
    text += "IsPasswordCacheEnabled=1\n";
    text += "AccountPasswordCache=" + hex_bytes(a.password_hash.data(), a.password_hash.size()) + "\n";
    std::ofstream(folder / "account.dat", std::ios::binary) << text;
    g_account = {};
    parse_account(read_file(folder / "account.dat"));
    return g_account.present;
}

const Account& account() { return g_account; }
const Device& device() { return g_device; }
bool ready() { return g_account.present && g_device.present; }

} // namespace cafe::os::online
