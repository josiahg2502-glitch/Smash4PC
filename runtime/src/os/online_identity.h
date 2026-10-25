#pragma once

// The player's console identity for Nintendo Network services (Pretendo):
// otp.bin and seeprom.bin from their Wii U (device id, serial, region, the
// device certificate) and account.dat (the network account: id, password
// hash, principal id, Mii). Read once from a folder (online\ beside the
// game); none of it is ever logged.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace cafe::os::online {

struct Account {
    bool present = false;
    std::string account_id; // the PNID's user name
    std::array<uint8_t, 32> password_hash{};
    uint32_t principal_id = 0, persistent_id = 0, country = 0, simple_address_id = 0;
    uint64_t transferable_id_base = 0, utc_offset = 0;
    std::array<uint8_t, 16> uuid{};
    std::array<uint8_t, 96> mii{};
    std::array<uint16_t, 11> mii_name{};
};

struct Device {
    bool present = false;
    uint32_t device_id = 0;
    std::string serial;
    uint32_t region = 0;     // 1 JPN, 2 USA, 4 EUR
    std::string certificate; // the 0x180-byte device certificate, base64
};

void load(const std::filesystem::path& folder);
// account.dat in `folder` for a signed-in account (act_server::sign_in), then loaded.
struct NewAccount {
    std::string user_id;
    uint32_t pid = 0;
    std::array<uint8_t, 32> password_hash{};
    std::array<uint8_t, 96> mii{};
    std::array<uint16_t, 11> mii_name{};
    bool us = true;
};
bool save_account(const std::filesystem::path& folder, const NewAccount& account);
const Account& account();
const Device& device();
// Both present: online services can be used.
bool ready();

std::string base64(const uint8_t* data, size_t size);

} // namespace cafe::os::online
