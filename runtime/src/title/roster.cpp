#include "title/roster.h"

#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"
#include "cafe/vfs.h"
#include "title/archive.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cafe::title {
namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ the title's tables (EU v16)

constexpr uint32_t kCodes = 0x107C57F4;       // 63 x char*: each fighter's three-letter code
constexpr uint32_t kRecords = 0x107B4220;     // 139 x 84 bytes: a fighter in one costume slot
constexpr uint32_t kRecordSize = 84;
constexpr uint32_t kRowRecords = 0x10873A68;  // 68 x the record of each motion row, built at start
constexpr uint32_t kMotionRows = 0x105A51C4;  // 68 x keys of {motion pack, moveset, movelist}
constexpr uint32_t kGrids = 0x1015F8E8;       // 6 x {cells, count}: the select screens' grids
constexpr uint32_t kGrid = 0x107B7668;        // grid 1, the one the title uses: 32-byte cells
constexpr uint32_t kIntroBanks[2] = {0x105C5084, 0x105C527C}; // 63 x the key of a fighter's rm_Dst bank
// 63 x the key of a fighter's rm_Dwi bank: the long win quote one win pose
// streams (sub_077CC6EC); a fighter with none (key 0) streams from a file
// with no name, which the title reports as an error (a pause).
constexpr uint32_t kWinStreams = 0x105C5180;
constexpr int kArchives = 56;                 // data000-data055, then the update's data100

// Record fields.
enum : uint32_t { kRecId = 0, kRecSlot = 4, kRecUiPosition = 8, kRecRow = 12, kRecSex = 16, kRecInit = 20, kRecInUse = 80 };
// Cell fields (s32): the fighter (63: random), the cells the cursor moves to, its place.
enum : uint32_t { kCellId = 0, kCellUp = 8, kCellDown = 12, kCellLeft = 16, kCellRight = 20, kCellColumn = 24, kCellRow = 28 };
constexpr uint32_t kTitleCells = 62;           // grid 1's own: rows 1 and 2 (cells 0-45), the top row (46-61)
constexpr uint32_t kUiPosition = 59;           // an added fighter's records: after the 59 fighters'

// The added fighters. The title's fighter ids end at 62 (59-62: the practice
// dummy, the menus' empty cell and the Fight Lab's two mobs), 63 means
// "random" or "none" throughout, and its per-fighter tables have 63 rows, so
// the added fighters take ids from 64 on, and the functions that would read
// past a table for them (or turn them into 62 or "none") are overridden
// below. Fighter k has:
// * select id 64 + k and character records 140 + 2k, 141 + 2k (139 means
//   "none"): the records live here, not in the title's table;
// * its own code and file keys, registered with the title's key registry and
//   its chara master at start-up (sub_065C3128), as the title registers its
//   own fighters';
// * a motion row: the rows no fighter has (kDirectRows) first, then rows from
//   69 on, whose keys are put in the spare row 63 while the title reads them;
// * a select grid cell (62 + k, over the unused grid 2): the free ends of
//   the top row (kCells: right of Eddy's column, then left, alternately),
//   then columns beyond the grid's ends (the cursor's place is computed from
//   the column, so -1 and 23 are left and right of the rows' ends);
// * one sound bank (its shouts, then its win quotes) under one of the four
//   bank ids no bank has, bound when a match loads it (a tag match has four
//   fighters at most).
constexpr uint32_t kFirstAddedId = 64;
constexpr uint32_t kFirstAddedRecord = 140;
constexpr uint32_t kDirectRows[] = {58, 64, 61, 62, 65};
constexpr uint32_t kSpareRow = 63, kFirstVirtualRow = 69;
struct GridPlace {
    int32_t column;
    uint32_t row;
};
constexpr GridPlace kCells[] = {{19, 0}, {2, 0}, {20, 0}, {1, 0},  {21, 0}, {0, 0},  {22, 0},
                                {23, 0}, {-1, 0}, {23, 1}, {-1, 1}, {23, 2}, {-1, 2}, {24, 1},
                                {-2, 1}, {24, 2}, {-2, 2}, {24, 0}, {-2, 0}}; // pack.py CELLS
constexpr uint32_t kFirstAddedCell = 62;
constexpr uint32_t kSpacers[2] = {0x1053138C, 0x105314CC}; // the grilles over the top row's ends: left, right
constexpr uint32_t kFreeBanks[] = {1, 122, 233, 253};

uint32_t row_of(size_t k) {
    return k < std::size(kDirectRows) ? kDirectRows[k]
                                      : kFirstVirtualRow + static_cast<uint32_t>(k - std::size(kDirectRows));
}

// The files every fighter has, after its code. The title finds a fighter's
// body files (`<code>.xml` .. `<code>.havok.xml`) by keys its key registry
// holds for the code (registered at start-up; an added fighter's are the
// CRCs of the names). Its movelist, moveset and motions are found by the keys
// in its motion row, which name them after its own code.
constexpr const char* kBodySuffixes[] = {".xml", ".nud", ".nut", ".bon", ".mop", ".havok.xml"};
constexpr const char* kMotionName = ".motion", *kMovesetName = ".moveset", *kMovelistName = ".mvl";
// The packs its item database takes costume parts from (attributes mi, ti,
// bi, pi): a part is the pack's key extended by its index; an added fighter's
// packs are `<code>/parts/<kind>`. In a pack: `parts/<kind>/<index>.<extension>`.
constexpr const char* kPartKinds[] = {"model", "texture", "bone", "mop"};

// A fighter's sound banks (NUS3BANKs, stored raw in the sound archives):
// shouts and damage voices `se_Chr_<code>`, win quotes `se_Dwi_<code>` and
// intro lines `rm_Dst_<code>`. The first two are found through bank ids
// (sub_07A5CAC8, sub_07A5CAA4) whose keys sub_07A5D01C gives. An added
// fighter's `se_Chr_<code>` holds both (its character.ini's chr_tones: the
// shouts' count, the win quotes' tones follow) under one bank id, bound per
// match (kFreeBanks). The others' keys are in tables by fighter. A pack
// without a bank has the donor's.
constexpr const char* kVoiceBanks[] = {"se_Chr_", "se_Dwi_", "rm_Dst_", "rm_Dwi_"};
constexpr const char* kSoundArchive = "content/hdd/data028";
// Moves play voices by sound id: a fighter's voice list (kVoiceLists, by
// select id, copied to the run-time lists by sub_0796F300 at start-up) holds
// the sound ids of its shouts and damage voices, and the sound table
// (sub_079E5B1C) gives each id's {tone, bank id, group, flags}. ZAKO A's list
// is a placeholder (Paul's tone 0), so the added fighter gets the donor's
// list, its ids pointing at copies of the donor's sound table entries that
// play the same tones of the added fighter's banks. New ids start above the
// ones sub_079E5CD0 remaps (0x213B-0x221D).
constexpr uint32_t kVoiceLists = 0x107D44EC;   // 63 x {u32 entries, u32 count, u32 fighter}; entry 16 B
// {0, sound id, 0x4000 | k, u16 mask, 0}; a category's entries are those with
// a bit of its mask (kVoiceMasks): a move's shout n (2 << 24 | n) is the n-th
// of category 5. An added fighter's Tekken 7 shouts past the donor's count are
// tones its pack appends (character.ini voice_extra), added to its list.
constexpr uint32_t kVoiceMasks = 0x105DEFDC;   // 10 x u32 by category
constexpr uint32_t kShoutMask = 0x20;        // category 5: shouts
constexpr uint32_t kItemSounds = 0x107D4044;   // 63 x {u32 sound ids, u32 count}
constexpr uint32_t kSoundTable = 0x107D4AC0;   // 8506 x {u32 tone, u32 bank id, u32 group, u32 flags}
constexpr uint32_t kSoundCount = 8506;
constexpr uint32_t kFirstNewSound = 0x221E;
constexpr uint32_t kChrBankIds = 0x105F2C7C;   // u8 se_Chr bank ids (sub_07A5CAC8's ranges)
constexpr uint32_t kDwiBankIds = 0x105F2C40;   // u8 se_Dwi bank ids by fighter (sub_07A5CAA4)
// A fighter's two name calls come from sub_079E5D64(id, kind) (-1 above 58):
// kind 0 its se_Chr bank's, kind 1 the select screen's from the shared bank
// se_Slt_StartChar, whose link tone `sel_syvo_name_<code>` plays the call. The
// importer adds every fighter's to a changed copy of that bank (shared/keys/).
constexpr const char* kSelectBank = "se_Slt_StartChar.nus3bank";

// The archives whose entries are stored raw (no scrambling, no NRCZ): the
// sound banks and the movies.
bool raw_archive(const std::string& name) {
    const int n = std::atoi(name.c_str() + name.size() - 3);
    return (n >= 20 && n <= 24) || (n >= 27 && n <= 50);
}

// The index of the tone named `name` in a NUS3BANK (see
// tools/characters/ttt2char/ttt2/nus3bank.py), if any.
std::optional<uint32_t> tone_index(const std::vector<uint8_t>& bank, const std::string& name) {
    auto le32 = [&](size_t at) -> uint32_t {
        return at + 4 <= bank.size() ? uint32_t(bank[at]) | uint32_t(bank[at + 1]) << 8 | uint32_t(bank[at + 2]) << 16 |
                                           uint32_t(bank[at + 3]) << 24
                                     : 0;
    };
    if (bank.size() < 24 || std::memcmp(bank.data(), "NUS3", 4) != 0) return std::nullopt;
    const uint32_t chunks = le32(20);
    size_t at = 0x14 + le32(16);
    for (uint32_t c = 0; c < chunks && at + 8 <= bank.size(); ++c) {
        const uint32_t size = le32(at + 4);
        if (std::memcmp(&bank[at], "TONE", 4) == 0) {
            const size_t tone = at + 8;
            const uint32_t count = le32(tone);
            for (uint32_t i = 0; i < count; ++i) {
                const size_t entry = tone + le32(tone + 4 + 8 * i);
                if (le32(tone + 8 + 8 * i) <= 16 || entry + 16 > bank.size()) continue;
                const size_t length = entry + (bank[entry + 4] == 0x7F ? 8 : 12); // a link tone's header is shorter
                const size_t n = bank[length];
                if (n == name.size() + 1 && length + n <= bank.size() &&
                    std::memcmp(&bank[length + 1], name.data(), name.size()) == 0) {
                    return i;
                }
            }
            return std::nullopt;
        }
        at += 8 + size;
    }
    return std::nullopt;
}

// A weapon its animations pose (the importer's weapons.py): its bone
// OPT 800 + k (k < 20: posed by the animations' extra pair k; from 20 still),
// that bone's parent (a bone index), its hit points (OPT 850 + n, each
// hitting as the held item's point slot s) and, for a weapon the fighter
// summons (Noctis's), the number of its summon (shown only while summoned).
struct WeaponPoint {
    uint32_t number = 0, slot = 0;
};
struct Weapon {
    uint32_t pair = 0, parent = 0;
    std::vector<WeaponPoint> points;
    int32_t summon = -1; // -1: always shown
};

// The added fighters, by number k (their slots).
struct Added {
    bool active = false;
    uint32_t id = 0, records[2] = {0, 0}, row = 0;
    uint32_t cell = 0;              // its select grid cell (0: none)
    int32_t column = 0;             // and the cell's column and row
    uint32_t grid_row = 0;
    uint32_t donor = 0, donor_row = 0; // the donor's select id and motion row
    bool female = true;
    std::string code;
    uint32_t code_string = 0;       // the code as a guest string
    fs::path pack;
    std::vector<Weapon> weapons;
};
std::vector<Added> g_added;
bool g_active = false; // any

// An added fighter's voice.
struct Voice {
    uint32_t keys[4] = {0, 0, 0, 0};          // the pack's own banks' keys (0: the donor's)
    uint32_t chr_tones = 0;                    // the shouts' tones in its bank (keys[0]): the win quotes' follow
    std::map<uint32_t, uint32_t> moved;        // the donor's sound ids -> the added fighter's copies
    std::vector<uint32_t> copies;              // those copies (their bank: the id bound to it)
    std::optional<uint32_t> name_tone;         // the select screen's call of its name: the tone in se_Slt_StartChar
    uint32_t name_sound = 0;                   // and its sound id (its entry is made on first use)
    bool name_made = false;
    uint32_t entries = 0;                      // its voice list's entries (the donor's, sound ids moved, then
    uint32_t count = 0;                        // its shouts past the donor's) and their count
    std::vector<uint32_t> extra;               // those shouts' tones in its bank (character.ini voice_extra),
    std::vector<uint32_t> extra_sounds;        // their sound ids
    uint32_t shout_entry = 0;                  // the donor's last shout entry (theirs copy it)
    uint32_t item_sounds = 0, items = 0;       // its item sounds (the donor's, moved) and their count
};
std::vector<Voice> g_voice;
uint32_t g_sound_table = 0, g_sound_count = 0; // the extended sound table (0: the title's)

// The slot of the added fighter with select id `id`, or -1.
int slot_of_id(uint32_t id) {
    if (id < kFirstAddedId || id - kFirstAddedId >= g_added.size()) return -1;
    return g_added[id - kFirstAddedId].active ? static_cast<int>(id - kFirstAddedId) : -1;
}
// The slot of the added fighter a record is of, or -1.
int slot_of_record(uint32_t record) {
    if (record < kFirstAddedRecord || (record - kFirstAddedRecord) / 2 >= g_added.size()) return -1;
    const size_t k = (record - kFirstAddedRecord) / 2;
    return g_added[k].active ? static_cast<int>(k) : -1;
}

uint32_t load(uint32_t address) {
    uint32_t v;
    std::memcpy(&v, guest_pointer(address), 4);
    return __builtin_bswap32(v);
}
void store(uint32_t address, uint32_t value) {
    const uint32_t v = __builtin_bswap32(value);
    std::memcpy(guest_pointer(address), &v, 4);
}
uint8_t load8(uint32_t address) { return *guest_pointer(address); }
void store8(uint32_t address, uint8_t value) { *guest_pointer(address) = value; }
uint16_t load16(uint32_t address) { return static_cast<uint16_t>(load8(address) << 8 | load8(address + 1)); }
void store16(uint32_t address, uint16_t value) {
    store8(address, static_cast<uint8_t>(value >> 8));
    store8(address + 1, static_cast<uint8_t>(value));
}

// An entry's key: CRC-32 of its name, the register starting from zero.
uint32_t key_of(const std::string& name) {
    return static_cast<uint32_t>(crc32(0xFFFFFFFFul, reinterpret_cast<const Bytef*>(name.data()),
                                       static_cast<uInt>(name.size())));
}

// The key of member `index` of a pack: the CRC continued over the index's
// base-4 digits, least significant first, a byte each.
uint32_t member_key(uint32_t pack, uint32_t index) {
    uint8_t digits[16];
    uInt n = 0;
    for (; index != 0; index >>= 2) digits[n++] = static_cast<uint8_t>(index & 3);
    return n == 0 ? pack : static_cast<uint32_t>(crc32(pack, digits, n));
}

// The key of an entry a pack names by key (`keys/6ccdfc60.bin`: a changed
// copy of one of the title's own entries), if it is one.
std::optional<uint32_t> named_key(const std::string& path) {
    if (path.size() < 13 || path.compare(0, 5, "keys/") != 0) return std::nullopt;
    const std::string hex = path.substr(5, 8);
    if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return std::nullopt;
    return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
}

// The key of an added fighter's part pack of kind k (kPartKinds).
uint32_t part_pack(const std::string& code, size_t kind) { return key_of(code + "/parts/" + kPartKinds[kind]); }

// A part's key from its path in a pack (`parts/model/7.nud`), if it is one.
std::optional<uint32_t> part_key(const std::string& path, const std::string& code) {
    for (size_t k = 0; k < std::size(kPartKinds); ++k) {
        const std::string prefix = std::string("parts/") + kPartKinds[k] + "/";
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = path.substr(prefix.size());
        const size_t digits = rest.find_first_not_of("0123456789");
        if (digits == 0 || digits == std::string::npos || rest.find('/') != std::string::npos) return std::nullopt;
        return member_key(part_pack(code, k), static_cast<uint32_t>(std::stoul(rest.substr(0, digits))));
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ packs

// The pack format this port loads (character.ini `format`; the importer's
// pack.PACK_FORMAT). A pack of another format is not loaded: its files were
// made for another design. Its place stays its own, for its re-import. A
// pack without a `format` line is of format 1, unless it has a `keys/`
// folder: the earliest packs carried their own changed copies of the
// title's entries there (made for one fighter alone, before shared/), which
// would stack with shared/'s and move every added fighter's pictures one
// place; they are format 0.
constexpr int kPackFormat = 1;
// Added fighters' places: 0 to 127 (the importer's pack.MAX_FIGHTERS).
constexpr int kMaxFighters = 128;

struct Pack {
    fs::path directory;
    int format = 0;    // character.ini `format` (0: an older importer's, see kPackFormat)
    std::string code;  // three lower-case letters
    std::string donor; // a fighter's code
    bool female = true;
    int slot = -1;     // its number, as the importer chose it (shared/ holds pictures by number)
    uint32_t chr_tones = 0; // its voice bank's shouts (the win quotes' tones come after; 0: no merged bank)
    std::vector<uint32_t> voice_extra; // its bank's shouts past the donor's (appended to the voice list)
    std::vector<Weapon> weapons;       // character.ini weapons: k:parent:points[:summon] (see pose_weapons)
};

fs::path packs_directory() {
    if (const char* dir = std::getenv("TTT2_CHARACTERS"); dir && *dir) return dir;
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return fs::path(data) / "ttt2/characters";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".local/share/ttt2/characters";
}

std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

// A character.ini number (format, slot): ASCII digits only, below 2^31, else
// none, as the importer reads it (pack.number).
std::optional<int> ini_number(const std::string& value) {
    int number = 0;
    const char* end = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), end, number);
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || error != std::errc{} ||
        stop != end) {
        return std::nullopt;
    }
    return number;
}

std::optional<Pack> read_pack(const fs::path& directory) {
    std::ifstream in(directory / "character.ini");
    if (!in) return std::nullopt;
    Pack pack;
    pack.directory = directory;
    bool has_format = false;
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        if (key == "format") {
            pack.format = ini_number(value).value_or(0);
            has_format = true;
        } else if (key == "code") pack.code = value;
        else if (key == "donor") pack.donor = value;
        else if (key == "sex") pack.female = value != "male";
        else if (key == "slot") pack.slot = ini_number(value).value_or(-1);
        else if (key == "chr_tones") pack.chr_tones = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "voice_extra") {
            std::istringstream tones(value);
            for (uint32_t tone; tones >> tone;) pack.voice_extra.push_back(tone);
        } else if (key == "weapons") { // k:parent:points[:summon], points n or n>slot, e.g. 1:9:0,1,2 or 4:9:0>0,1>1:3
            std::istringstream items(value);
            for (std::string item; items >> item;) {
                std::vector<std::string> fields;
                std::istringstream parts(item);
                for (std::string field; std::getline(parts, field, ':');) fields.push_back(field);
                if (fields.size() < 2) continue;
                Weapon weapon;
                weapon.pair = static_cast<uint32_t>(std::strtoul(fields[0].c_str(), nullptr, 10));
                weapon.parent = static_cast<uint32_t>(std::strtoul(fields[1].c_str(), nullptr, 10));
                std::istringstream points(fields.size() > 2 ? fields[2] : std::string());
                for (std::string j; std::getline(points, j, ',');) {
                    if (j.empty()) continue;
                    WeaponPoint point;
                    point.number = static_cast<uint32_t>(std::strtoul(j.c_str(), nullptr, 10));
                    const size_t to = j.find('>');
                    point.slot = to == std::string::npos ? point.number
                                                         : static_cast<uint32_t>(std::strtoul(j.c_str() + to + 1, nullptr, 10));
                    weapon.points.push_back(point);
                }
                if (fields.size() > 3 && !fields[3].empty()) weapon.summon = std::atoi(fields[3].c_str());
                pack.weapons.push_back(weapon);
            }
        }
    }
    const bool code_ok = pack.code.size() == 3 &&
                         std::all_of(pack.code.begin(), pack.code.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    if (!code_ok || pack.donor.empty()) {
        std::fprintf(stderr, "ttt2: warning: %s needs a three-letter `code` and a `donor`; skipped\n",
                     (directory / "character.ini").c_str());
        return std::nullopt;
    }
    std::error_code ec;
    if (!has_format) pack.format = fs::is_directory(directory / "keys", ec) ? 0 : 1;
    return pack;
}

// ------------------------------------------------------------------ the title's files

struct Located {
    std::string archive;
    archive::Entry entry;
};

// Every entry of the title's archives by key, as the title finds them: the
// last archive read (data100) wins.
std::map<uint32_t, Located> locate_entries() {
    std::map<uint32_t, Located> found;
    for (int i = 0; i <= kArchives; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "content/hdd/data%03d", i == kArchives ? 100 : i);
        const auto ofs = vfs::read_whole(std::string("/vol/") + name + ".ofs");
        const auto index = ofs ? archive::parse_index(*ofs) : std::nullopt;
        if (!index) continue;
        for (const archive::Entry& e : index->entries) found[e.hash] = Located{name, e};
    }
    return found;
}

std::optional<std::vector<uint8_t>> read_stored(const Located& at) {
    int32_t handle = 0;
    if (vfs::open_file("/vol/" + at.archive + ".bin", "r", handle) != vfs::kOk) return std::nullopt;
    std::vector<uint8_t> stored(at.entry.size);
    uint64_t transferred = 0;
    vfs::read_file(handle, stored.data(), stored.size(), archive::kPayload + at.entry.offset, transferred);
    vfs::close_file(handle);
    if (transferred != stored.size()) return std::nullopt;
    return stored;
}

std::optional<std::vector<uint8_t>> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// A texture container (NTP3: "NTP3", u16 version, u16 count at 6, then the
// textures from 16 on, each starting with its u32 total size) with the
// textures of `add`, another one, after its own. The 2D layouts take their
// texture pages from the container after them by id, so a pack adds pages to
// one instead of replacing all of it.
std::optional<std::vector<uint8_t>> with_textures(std::vector<uint8_t> nut, const std::vector<uint8_t>& add) {
    auto end_of = [](const std::vector<uint8_t>& d) -> std::optional<size_t> {
        if (d.size() < 16 || std::memcmp(d.data(), "NTP3", 4) != 0) return std::nullopt;
        size_t at = 16;
        for (uint32_t i = 0, n = uint32_t(d[6]) << 8 | d[7]; i < n; ++i) {
            if (at + 4 > d.size()) return std::nullopt;
            const uint32_t total = uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
            if (total < 16 || at + total > d.size()) return std::nullopt;
            at += total;
        }
        return at;
    };
    const auto own = end_of(nut), added = end_of(add);
    if (!own || !added) return std::nullopt;
    const uint32_t count = (uint32_t(nut[6]) << 8 | nut[7]) + (uint32_t(add[6]) << 8 | add[7]);
    if (count > 0xFFFF) return std::nullopt;
    nut.insert(nut.begin() + static_cast<std::ptrdiff_t>(*own), add.begin() + 16, add.begin() + static_cast<std::ptrdiff_t>(*added));
    nut[6] = static_cast<uint8_t>(count >> 8);
    nut[7] = static_cast<uint8_t>(count);
    return nut;
}

// ------------------------------------------------------------------ the new fighter

// The select id of the fighter with `code`, if any.
std::optional<uint32_t> fighter_id(const std::string& code) {
    for (uint32_t id = 0; id < 63; ++id) {
        const char* name = reinterpret_cast<const char*>(guest_pointer(load(kCodes + 4 * id)));
        if (code == name) return id;
    }
    return std::nullopt;
}

uint32_t record_address(uint32_t record) { return kRecords + kRecordSize * record; }

// A fighter's first costume slot's record (0, Paul's, if it has none).
uint32_t first_record(uint32_t id) {
    for (uint32_t r = 0; r < 139; ++r) {
        if (load(record_address(r) + kRecId) == id && load(record_address(r) + kRecSlot) == 0 &&
            load(record_address(r) + kRecInUse) == 1) {
            return r;
        }
    }
    return 0;
}

// The motion row of a fighter's first costume slot.
std::optional<uint32_t> motion_row(uint32_t id) {
    for (uint32_t r = 0; r < 139; ++r) {
        if (load(record_address(r) + kRecId) == id && load(record_address(r) + kRecSlot) == 0 &&
            load(record_address(r) + kRecInUse) == 1) {
            return load(record_address(r) + kRecRow);
        }
    }
    return std::nullopt;
}

// An entry of the title's archives (data100 last), stored, with its NRCZ flags.
using Stored = std::pair<std::vector<uint8_t>, std::array<uint8_t, 8>>;
std::optional<Stored> title_entry(const std::map<uint32_t, Located>& located, uint32_t key) {
    const auto it = located.find(key);
    if (it == located.end()) return std::nullopt;
    auto stored = read_stored(it->second);
    if (!stored) return std::nullopt;
    std::array<uint8_t, 8> flags{};
    uint8_t raw[8];
    archive::stored_flags(*stored, key, raw);
    std::copy(std::begin(raw), std::end(raw), flags.begin());
    return std::make_pair(std::move(*stored), flags);
}

bool has_suffix(const std::string& name, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

// A file of `keys/` (a changed copy of one of the title's entries, or
// textures to add to one of its texture containers: `<key>.add`, see
// with_textures) as entries: stored ones into `entries`, raw ones (a changed
// sound bank) into `raw` by archive. False if it does not fit.
bool add_title_copy(const std::map<uint32_t, Located>& located, const std::string& name, uint32_t key,
                    const std::vector<uint8_t>& contents, std::map<uint32_t, std::vector<uint8_t>>& entries,
                    std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>>& raw) {
    if (has_suffix(name, ".add")) {
        const auto original = title_entry(located, key);
        auto unpacked = original ? archive::unpack(original->first, key) : std::nullopt;
        auto joined = unpacked ? with_textures(std::move(*unpacked), contents) : std::nullopt;
        if (!joined) return false;
        uint8_t flags[8];
        std::copy(original->second.begin(), original->second.end(), flags);
        entries[key] = archive::pack(*joined, key, flags);
        return true;
    }
    // A changed copy of a raw entry (a sound bank) stays raw, in the original's archive.
    const auto original = located.find(key);
    if (original != located.end() && raw_archive(original->second.archive)) {
        raw[original->second.archive][key] = contents;
        return true;
    }
    uint8_t flags[8] = {};
    if (const auto title = title_entry(located, key)) std::copy(title->second.begin(), title->second.end(), flags);
    entries[key] = archive::pack(contents, key, flags);
    return true;
}

// Raw entries into their archives; -> the archives they could not be added to.
std::vector<std::string> serve_raw(const std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>>& raw,
                                   const fs::path& from) {
    std::vector<std::string> failed;
    for (const auto& [name, map] : raw) {
        if (vfs::set_archive_entries(name, map)) continue;
        std::fprintf(stderr, "ttt2: warning: %s: its entries of %s could not be added\n", from.c_str(), name.c_str());
        failed.push_back(name);
    }
    return failed;
}

// The pack's files as archive entries, the donor's where the pack has none.
// Each is stored with the NRCZ flags of the donor's matching entry. Its
// pictures (`art/`), effects (`alo/`: the title loads a fighter's effect files
// and their textures as `<code>/alo/...`) and anything else are entries named
// by their path, the pictures and effects under the fighter's code.
bool add_entries(const Pack& pack, uint32_t donor_row, size_t slot) {
    Voice& voice = g_voice[slot];
    const auto located = locate_entries();
    struct File {
        std::string name;   // in the pack
        uint32_t key;       // the entry's
        uint32_t donor_key; // its counterpart's
    };
    std::vector<File> files;
    for (const char* suffix : kBodySuffixes) {
        files.push_back({pack.code + suffix, key_of(pack.code + suffix), key_of(pack.donor + suffix)});
    }
    files.push_back({pack.code + kMovelistName, key_of(pack.code + kMovelistName), key_of(pack.donor + kMovelistName)});
    files.push_back({pack.code + kMotionName, key_of(pack.code + kMotionName), load(kMotionRows + 12 * donor_row)});
    files.push_back({pack.code + kMovesetName, key_of(pack.code + kMovesetName), load(kMotionRows + 12 * donor_row + 4)});

    std::map<uint32_t, std::vector<uint8_t>> entries;
    size_t own = 0;
    for (const File& f : files) {
        const auto donor = title_entry(located, f.donor_key);
        const uint32_t key = f.key;
        uint8_t flags[8] = {};
        if (donor) std::copy(donor->second.begin(), donor->second.end(), flags);
        if (const auto contents = read_file(pack.directory / f.name)) {
            entries[key] = archive::pack(*contents, key, flags);
            ++own;
        } else if (donor) {
            auto contents = archive::unpack(donor->first, f.donor_key);
            if (!contents) return false;
            entries[key] = archive::pack(*contents, key, flags);
        } else if (f.name != pack.code + ".mop" && f.name != pack.code + ".havok.xml") {
            std::fprintf(stderr, "ttt2: warning: neither %s nor its donor has %s\n", pack.directory.c_str(),
                         f.name.c_str());
            return false;
        }
    }
    // Its own sound banks: raw entries of a sound archive.
    std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>> raw; // archive -> raw entries
    std::vector<std::string> bank_files;
    voice.chr_tones = pack.chr_tones;
    voice.extra = pack.voice_extra;
    for (size_t b = 0; b < std::size(kVoiceBanks); ++b) {
        if (b == 1) continue; // win quotes: in se_Chr_ (an older pack's own se_Dwi_ is not played)
        const std::string name = std::string(kVoiceBanks[b]) + pack.code + ".nus3bank";
        auto contents = read_file(pack.directory / name);
        if (!contents || contents->size() < 4 || std::memcmp(contents->data(), "NUS3", 4) != 0) continue;
        voice.keys[b] = key_of(name);
        raw[kSoundArchive][voice.keys[b]] = std::move(*contents);
        bank_files.push_back(name);
        ++own;
    }
    // Costume parts, pictures, effects and anything else. Changed copies of
    // the title's own entries come from `shared/` only (add_shared, made for
    // all the packs together): a `keys/` folder in a pack (an early
    // importer's, made for one fighter alone) is never read, since its
    // texture pages would be joined to the title's containers before
    // shared/'s and move every added fighter's pictures one place.
    std::error_code ec;
    for (const auto& item : fs::recursive_directory_iterator(pack.directory, ec)) {
        if (!item.is_regular_file()) continue;
        const std::string name = fs::relative(item.path(), pack.directory).generic_string();
        const bool known = name == "character.ini" ||
                           std::any_of(files.begin(), files.end(), [&](const File& f) { return f.name == name; }) ||
                           std::find(bank_files.begin(), bank_files.end(), name) != bank_files.end();
        if (known || name.compare(0, 5, "keys/") == 0) continue;
        auto contents = read_file(item.path());
        if (!contents) continue;
        ++own;
        const bool own = name.compare(0, 4, "art/") == 0 || name.compare(0, 4, "alo/") == 0;
        const std::string entry = own ? pack.code + "/" + name : name;
        const uint32_t key = part_key(name, pack.code).value_or(key_of(entry));
        uint8_t flags[8] = {};
        entries[key] = archive::pack(*contents, key, flags);
    }
    if (!vfs::set_archive_entries("content/hdd/data100", entries)) return false;
    for (const std::string& failed : serve_raw(raw, pack.directory)) {
        if (failed == kSoundArchive) std::fill(std::begin(voice.keys), std::end(voice.keys), 0u);
    }
    if (std::getenv("TTT2_TRACE_ROSTER")) {
        std::fprintf(stderr, "ttt2: roster: %s (donor %s, id %u): %zu entries, %zu from the pack\n",
                     pack.code.c_str(), pack.donor.c_str(), kFirstAddedId + static_cast<uint32_t>(slot), entries.size(),
                     own);
    }
    return true;
}

// The title entries the added fighters share (`shared/keys/`, made by the
// importer for every installed pack: layouts with each one's labels, the
// big portraits' container with their frames, the pages and HUD pictures to
// add, the 2P colour palettes, the select screen's name calls).
void add_shared(const fs::path& directory) {
    std::error_code ec;
    if (!fs::is_directory(directory / "keys", ec)) return;
    const auto located = locate_entries();
    std::map<uint32_t, std::vector<uint8_t>> entries;
    std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>> raw;
    for (const auto& item : fs::directory_iterator(directory / "keys", ec)) {
        const std::string name = "keys/" + item.path().filename().string();
        const auto key = named_key(name);
        const auto contents = key ? read_file(item.path()) : std::nullopt;
        if (!contents) continue;
        if (!add_title_copy(located, name, *key, *contents, entries, raw)) {
            std::fprintf(stderr, "ttt2: warning: %s: %s does not fit the title's entry; left out\n",
                         directory.c_str(), name.c_str());
        }
    }
    if (!vfs::set_archive_entries("content/hdd/data100", entries)) {
        std::fprintf(stderr, "ttt2: warning: %s could not be added\n", directory.c_str());
        return;
    }
    const auto failed = serve_raw(raw, directory);
    // The select screen's name calls: each added fighter's tone in the shared copy of the bank.
    char bank_name[16];
    std::snprintf(bank_name, sizeof bank_name, "%08x.bin", key_of(kSelectBank));
    const auto bank = read_file(directory / "keys" / bank_name);
    const auto where = located.find(key_of(kSelectBank));
    if (!bank || where == located.end() ||
        std::find(failed.begin(), failed.end(), where->second.archive) != failed.end()) {
        return;
    }
    for (size_t k = 0; k < g_added.size(); ++k) {
        if (g_added[k].active) g_voice[k].name_tone = tone_index(*bank, "sel_syvo_name_" + g_added[k].code);
    }
}

// The se_Chr bank id of fighter `id` (what sub_07A5CAC8 computes).
uint32_t chr_bank(uint32_t id) {
    const uint32_t index = id <= 17 ? id : id <= 44 ? id + 1 : id + 3;
    return *reinterpret_cast<const uint8_t*>(guest_pointer(kChrBankIds + index));
}

// The se_Dwi bank id of fighter `id` (what sub_07A5CAA4(2, id) gives).
uint32_t dwi_bank(uint32_t id) { return *reinterpret_cast<const uint8_t*>(guest_pointer(kDwiBankIds + id)); }

// The added fighters' voice lists and item sounds: their donors', pointing
// at the added fighters' own banks where their packs have them (see
// kVoiceLists), in one extended sound table: the title's, then each added
// fighter's copies and its select screen name call. A copy's bank is the id
// bound to the fighter in a match (bind_bank); a win quote's tone follows
// the shouts'. The title's tables have no rows for ids from 64 on: the lists
// are kept here and lent to the donor's rows while the title reads them for
// an added fighter.
void patch_sounds() {
    std::vector<std::pair<size_t, uint32_t>> extra; // (slot, the donor's sound id copied onto its bank)
    uint32_t names = 0, shouts = 0;
    for (size_t k = 0; k < g_added.size(); ++k) {
        if (!g_added[k].active) continue;
        const uint32_t donor = g_added[k].donor;
        const Voice& voice = g_voice[k];
        for (uint32_t sid = 0; sid < kSoundCount && voice.keys[0]; ++sid) {
            const uint32_t bank = load(kSoundTable + 16 * sid + 4);
            if (bank == chr_bank(donor) || (voice.chr_tones && bank == dwi_bank(donor))) extra.push_back({k, sid});
        }
        if (voice.keys[0]) shouts += static_cast<uint32_t>(voice.extra.size());
        if (voice.name_tone) ++names;
    }
    if (!extra.empty() || names || shouts) {
        g_sound_count = kFirstNewSound + static_cast<uint32_t>(extra.size()) + names + shouts;
        g_sound_table = system_alloc(16 * g_sound_count);
        std::memset(guest_pointer(g_sound_table), 0, 16 * g_sound_count);
        std::memcpy(guest_pointer(g_sound_table), guest_pointer(kSoundTable), 16 * kSoundCount);
        uint32_t next = kFirstNewSound;
        for (const auto& [k, sid] : extra) {
            const uint32_t to = g_sound_table + 16 * next;
            std::memcpy(guest_pointer(to), guest_pointer(kSoundTable + 16 * sid), 16);
            if (load(to + 4) != chr_bank(g_added[k].donor)) store(to, load(to) + g_voice[k].chr_tones);
            store(to + 4, 0xFFFFFFFF); // no bank until a match binds one
            g_voice[k].moved[sid] = next;
            g_voice[k].copies.push_back(next++);
        }
        for (size_t k = 0; k < g_added.size(); ++k) {
            if (g_added[k].active && g_voice[k].name_tone) g_voice[k].name_sound = next++;
        }
        // Shouts past the donor's: copies of its last shout's entry playing
        // the appended tones.
        for (size_t k = 0; k < g_added.size(); ++k) {
            Voice& voice = g_voice[k];
            if (!g_added[k].active || !voice.keys[0] || voice.extra.empty()) continue;
            const uint32_t from = kVoiceLists + 12 * g_added[k].donor;
            uint32_t shout = 0;
            for (uint32_t e = 0; e < load(from + 4); ++e) {
                if (load16(load(from) + 16 * e + 12) & kShoutMask) shout = load(from) + 16 * e;
            }
            if (shout == 0) continue;
            for (const uint32_t tone : voice.extra) {
                const uint32_t to = g_sound_table + 16 * next;
                std::memcpy(guest_pointer(to), guest_pointer(kSoundTable + 16 * load(shout + 4)), 16);
                store(to, tone);
                store(to + 4, 0xFFFFFFFF);
                voice.extra_sounds.push_back(next);
                voice.copies.push_back(next++);
            }
            voice.shout_entry = shout;
        }
    }
    for (size_t k = 0; k < g_added.size(); ++k) {
        if (!g_added[k].active) continue;
        const uint32_t donor = g_added[k].donor;
        Voice& voice = g_voice[k];
        auto moved_id = [&](uint32_t sid) {
            const auto it = voice.moved.find(sid);
            return it == voice.moved.end() ? sid : it->second;
        };
        // The voice list: the donor's entries with the moved sound ids, then
        // its own shouts past the donor's.
        const uint32_t from = kVoiceLists + 12 * donor;
        const uint32_t count = load(from + 4);
        voice.count = count + static_cast<uint32_t>(voice.extra_sounds.size());
        voice.entries = system_alloc(16 * std::max(voice.count, 1u));
        std::memcpy(guest_pointer(voice.entries), guest_pointer(load(from)), 16 * count);
        for (uint32_t e = 0; e < count; ++e) store(voice.entries + 16 * e + 4, moved_id(load(voice.entries + 16 * e + 4)));
        for (size_t e = 0; e < voice.extra_sounds.size(); ++e) {
            const uint32_t to = voice.entries + 16 * (count + static_cast<uint32_t>(e));
            std::memcpy(guest_pointer(to), guest_pointer(voice.shout_entry), 16);
            store(to + 4, voice.extra_sounds[e]);
            store16(to + 12, kShoutMask);
        }
        // The item sounds.
        voice.items = load(kItemSounds + 8 * donor + 4);
        if (voice.items != 0) {
            voice.item_sounds = system_alloc(4 * voice.items);
            for (uint32_t e = 0; e < voice.items; ++e) {
                store(voice.item_sounds + 4 * e, moved_id(load(load(kItemSounds + 8 * donor) + 4 * e)));
            }
        }
    }
}

// The title's records, code, motion row and intro/win streams for added
// fighter k (its records and code are registered when the title builds its
// own, see sub_065C3128).
void patch_title(size_t k) {
    const Added& added = g_added[k];
    // Its moveset, motions and movelist, in its motion row (a later fighter's
    // while the title reads its row, see sub_075DA0C8).
    if (added.row < 68) {
        store(kMotionRows + 12 * added.row, key_of(added.code + kMotionName));
        store(kMotionRows + 12 * added.row + 4, key_of(added.code + kMovesetName));
        store(kMotionRows + 12 * added.row + 8, key_of(added.code + kMovelistName));
    }
}

// The added fighters' cells (kCells), joined to the grid's: each row a ring
// left and right, each column a ring up and down, in column and row order
// (as the title's own cells are). A slot with no fighter has a random cell
// nothing moves to.
void place_cells() {
    struct Cell {
        int32_t column;
        uint32_t row, cell;
        bool operator<(const Cell& o) const { return std::tie(column, row) < std::tie(o.column, o.row); }
    };
    std::vector<Cell> cells;
    for (uint32_t c = 0; c < kTitleCells; ++c) {
        cells.push_back({static_cast<int32_t>(load(kGrid + 32 * c + kCellColumn)), load(kGrid + 32 * c + kCellRow), c});
    }
    uint32_t last = 0;
    for (size_t k = 0; k < g_added.size(); ++k) {
        const Added& added = g_added[k];
        if (k >= std::size(kCells)) break;
        const uint32_t cell = kFirstAddedCell + static_cast<uint32_t>(k), at = kGrid + 32 * cell;
        last = std::max(last, cell);
        if (!added.active || added.cell == 0) {
            store(at + kCellId, 63);
            for (uint32_t field : {kCellUp, kCellDown, kCellLeft, kCellRight}) store(at + field, cell);
            continue;
        }
        store(at + kCellId, added.id);
        store(at + kCellColumn, static_cast<uint32_t>(added.column));
        store(at + kCellRow, added.grid_row);
        cells.push_back({added.column, added.grid_row, cell});
    }
    if (last == 0) return;
    std::sort(cells.begin(), cells.end());
    auto ring = [&](auto same, uint32_t back, uint32_t ahead) {
        std::vector<Cell> sorted = cells;
        std::stable_sort(sorted.begin(), sorted.end(), [&](const Cell& a, const Cell& b) { return same(a) < same(b); });
        for (size_t i = 0; i < sorted.size();) {
            size_t j = i;
            while (j < sorted.size() && same(sorted[j]) == same(sorted[i])) ++j;
            for (size_t n = i; n < j; ++n) {
                const uint32_t next = sorted[n + 1 < j ? n + 1 : i].cell;
                store(kGrid + 32 * sorted[n].cell + ahead, next);
                store(kGrid + 32 * next + back, sorted[n].cell);
            }
            i = j;
        }
    };
    // rows: column order (cells is sorted by column, then row); columns: row order
    ring([](const Cell& c) { return static_cast<int64_t>(c.row); }, kCellLeft, kCellRight);
    std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return std::tie(a.row, a.column) < std::tie(b.row, b.column); });
    ring([](const Cell& c) { return static_cast<int64_t>(c.column); }, kCellUp, kCellDown);
    store(kGrids + 8 * 1 + 4, last + 1);
}

// ------------------------------------------------------------------ art

// The added fighters' pictures and names on the 2D screens. Their code finds
// a fighter's pictures by its UI position, its row in tables of 59, from the
// select id (sub_06F0A484 -> sub_033D5524: the first record with the id, the
// mobs' own records 120 and 121: UI position 0, Paul's) or from the record
// (sub_06F0A4A4 -> sub_033D555C: an added fighter's records' 59, which this
// code takes for "nobody"). An added fighter has no row of its own, so it
// gets Paul's from both, and while the code works on it Paul's row of every
// table holds that fighter's entries (show_added): the labels of its pictures
// in the layouts (the shared layouts have `chara_face_yoko_kzm` beside
// `chara_face_yoko_pau`), the keys of its textures, its frames of the big
// select portraits. The functions that ask for UI positions put Paul's row
// back when they return. A pack has pictures if it has a folder `art/`.

// Tables of label names (char*) by UI position, `per` entries a fighter.
struct LabelTable {
    uint32_t address, per;
};
constexpr LabelTable kLabelTables[] = {
    {0x107D1114, 1}, // chara_face_<code> (loading screen)
    {0x107D1200, 1}, // chara_name_s_<code>
    {0x107D159C, 1}, // face_<code>
    {0x107B2130, 2}, // face_p1_<code>, face_p2_<code> (select screen thumbnails)
};
// Label names as the title's string objects, built at start-up, by UI
// position: 32 bytes, the text at +4 while the capacity (+28) is under 16,
// else at the pointer at +20; the length at +24.
struct StringArray {
    uint32_t address, stride, per;
};
constexpr StringArray kLabelStrings[] = {
    {0x108EC990, 32, 1}, // chara_face_yoko_<code> (select screen, the bars of the team)
    {0x108ED0F0, 64, 2}, // p1_chara_waku_<code>, p2_chara_waku_<code> (select screen, the name over the portrait)
    {0x108E8B50, 64, 2}, // the same (loading and VS screens)
};
constexpr uint32_t kStringSize = 32, kStringText = 4, kStringPointer = 20, kStringLength = 24, kStringCapacity = 28;
// Keys of portrait textures by UI position (8 bytes a fighter: player 1's
// and 2's, or one and 0), and the pack's file that replaces Paul's (the
// entry `<code>/<file>`).
struct KeyTable {
    uint32_t address;
    const char* file;
};
constexpr KeyTable kKeyTables[] = {
    {0x10529004, "art/portrait_1p.nut"},    // loading and VS screens, red frame
    {0x10529008, "art/portrait_2p.nut"},    // blue frame
    {0x105291DC, "art/portrait_mono.nut"},  // black and white (sub_06F033B0's other kinds)
    {0x105293B4, "art/portrait_green.nut"}, // green frame
    {0x1052958C, "art/portrait_green2.nut"},
};
// The labels of the select screen's strip (`face_<code>`) by select id: 63
// records of 20 bytes, the last four empty.
constexpr uint32_t kProfiles = 0x107B078C, kProfileSize = 20;
// The big select portraits: container 6ccdfc60, two frames a UI position
// (player 1, 2; the shared copy has each added fighter's two after the 118,
// by slot),
// which sub_06F0449C lists for each player, by UI position, in two vectors
// of its object ({+4 first, +8 end}, at +4 and +20; the resources at +36).
constexpr uint32_t kPortraits = 0x6CCDFC60, kPortraitFrames = 118;
constexpr uint32_t kPortraitLists[2] = {4 + 4, 20 + 4}, kPortraitResources = 36;

struct Swap {
    uint32_t address;
    std::vector<uint8_t> ours;   // the added fighter's
    std::vector<uint8_t> theirs; // what they replaced
};
// An added fighter's pictures.
struct Art {
    bool on = false;          // its pack has pictures
    bool hud = false;         // the HUD's pictures are there (kHud)
    std::vector<Swap> swaps;  // Paul's rows' entries that become its own
    bool swaps_made = false;
    uint32_t portrait_frames[2] = {}; // its big portraits' frames (player 1, 2), as last listed
    std::vector<uint8_t> profile;     // its row of the select strip's labels (kProfiles)
};
std::vector<Art> g_art;
int g_shown = -1;                  // the slot whose entries Paul's rows hold, or -1
uint32_t g_portrait_owner = 0;     // the big portraits' object as last listed
uint32_t g_paul_frames[2] = {};    // and Paul's frames in it

uint32_t guest_string(const std::string& text) {
    const uint32_t at = system_alloc(static_cast<uint32_t>(text.size()) + 1);
    std::memcpy(guest_pointer(at), text.c_str(), text.size() + 1);
    return at;
}

// A label of Paul's with another code: "face_pau_lose" -> "face_kzm_lose".
std::string with_code(std::string label, const std::string& code) {
    const std::string paul = reinterpret_cast<const char*>(guest_pointer(load(kCodes)));
    for (size_t at = label.find(paul); at != std::string::npos; at = label.find(paul, at + 1)) {
        const size_t end = at + paul.size();
        if ((at == 0 || label[at - 1] == '_') && (end == label.size() || label[end] == '_')) {
            return label.replace(at, paul.size(), code);
        }
    }
    return label;
}

// Paul's rows, and what replaces them for slot k: made on first use, when the
// title has built its string objects.
void make_swaps(size_t k) {
    Art& art = g_art[k];
    const Added& added = g_added[k];
    art.swaps_made = true;
    auto be = [](uint32_t value) {
        std::vector<uint8_t> v(4);
        const uint32_t b = __builtin_bswap32(value);
        std::memcpy(v.data(), &b, 4);
        return v;
    };
    for (const LabelTable& t : kLabelTables) {
        for (uint32_t i = 0; i < t.per; ++i) {
            const uint32_t at = t.address + 4 * i;
            const std::string paul = reinterpret_cast<const char*>(guest_pointer(load(at)));
            art.swaps.push_back({at, be(guest_string(with_code(paul, added.code))), {}});
        }
    }
    for (const StringArray& a : kLabelStrings) {
        for (uint32_t i = 0; i < a.per; ++i) {
            const uint32_t at = a.address + kStringSize * i;
            const bool inline_text = load(at + kStringCapacity) < 16;
            const uint32_t text = inline_text ? at + kStringText : load(at + kStringPointer);
            const std::string label = with_code(reinterpret_cast<const char*>(guest_pointer(text)), added.code);
            std::vector<uint8_t> object(guest_pointer(at), guest_pointer(at) + kStringSize);
            auto put = [&](uint32_t offset, uint32_t value) {
                const uint32_t b = __builtin_bswap32(value);
                std::memcpy(&object[offset], &b, 4);
            };
            const uint32_t length = static_cast<uint32_t>(label.size());
            put(kStringPointer, guest_string(label));
            put(kStringLength, length);
            put(kStringCapacity, std::max<uint32_t>(length, 16)); // the text at the pointer
            art.swaps.push_back({at, std::move(object), {}});
        }
    }
    std::error_code ec;
    for (const KeyTable& t : kKeyTables) {
        if (fs::is_regular_file(added.pack / t.file, ec)) art.swaps.push_back({t.address, be(key_of(added.code + "/" + t.file)), {}});
    }
}

// Paul's rows hold the entries of slot k's fighter (k >= 0) or Paul's (-1).
void show_added(int k) {
    if (k >= 0 && !g_art[k].on) k = -1;
    if (k == g_shown) return;
    if (g_shown >= 0) {
        for (const Swap& s : g_art[g_shown].swaps) std::memcpy(guest_pointer(s.address), s.theirs.data(), s.theirs.size());
    }
    if (k >= 0) {
        Art& art = g_art[k];
        if (!art.swaps_made) make_swaps(static_cast<size_t>(k));
        for (Swap& s : art.swaps) {
            uint8_t* at = guest_pointer(s.address);
            s.theirs.assign(at, at + s.ours.size());
            std::memcpy(at, s.ours.data(), s.ours.size());
        }
    }
    g_shown = k;
}

bool any_art() {
    return std::any_of(g_art.begin(), g_art.end(), [](const Art& a) { return a.on; });
}

void prepare_art(const Pack& pack, size_t slot, const fs::path& shared) {
    std::error_code ec;
    if (!fs::is_directory(pack.directory / "art", ec)) return;
    Art& art = g_art[slot];
    art.on = true;
    // its row of the strip's labels: Paul's, with its own label (lent to Paul's row, see sub_0294C400)
    art.profile.resize(kProfileSize);
    std::memcpy(art.profile.data(), guest_pointer(kProfiles), kProfileSize);
    const uint32_t label = __builtin_bswap32(guest_string("face_" + pack.code));
    std::memcpy(art.profile.data(), &label, 4);
    art.hud = fs::is_regular_file(shared / "keys" / "7c476bde.add", ec);
}

// ------------------------------------------------------------------ costume chooser

// The select screen's costume chooser (CHANGE COSTUME) lists a fighter's
// costumes (sub_0580D690, from rows {entries, count} at kCostumeLists by
// select id: 59 rows, none for an added fighter) with a picture each, from a
// texture container whose key is in kCostumePreviews (59 keys by select id;
// the added fighters' places are past the table, in a string the title does
// not read there), then five customisation slots. An added fighter has one
// costume: its list is the donor's first costume (costume 1), its pictures
// the pack's (`<code>/` + kCostumeFile, else the donor's), its key lent to the
// table's place only while the chooser's functions run, and the chooser
// leaves out the customisation slots (customisation is made per fighter in a
// mode an added fighter is not in; loading a slot's items would read past
// tables).
constexpr uint32_t kCostumeLists = 0x103B4B84, kCostumePreviews = 0x10161590;
constexpr const char* kCostumeFile = "art/costumes.nut";
constexpr uint32_t kCostumeOne = 0;                          // the type of costume 1 in a list
constexpr uint32_t kChooserPending = 324, kChooserCount = 88, kChooserEntries = 92, kChooserEntrySize = 16;
constexpr uint32_t kChooserSlot = 5;                         // an entry's kind: a customisation slot
// An added fighter's costume list (one entry {type, u16 value, u16 0}) and the key of its pictures.
struct Costume {
    uint32_t entries = 0, key = 0;
};
std::vector<Costume> g_costumes;

void prepare_costumes(const Pack& pack, uint32_t donor, size_t slot) {
    std::error_code ec;
    Costume& costume = g_costumes[slot];
    costume.key = fs::is_regular_file(pack.directory / kCostumeFile, ec) ? key_of(pack.code + "/" + kCostumeFile)
                                                                         : load(kCostumePreviews + 4 * donor);
    // the donor's costume 1 (its first entry of that type), else a costume 1 of no value
    const uint32_t entries = load(kCostumeLists + 8 * donor), count = load(kCostumeLists + 8 * donor + 4);
    costume.entries = system_alloc(8);
    store(costume.entries, kCostumeOne);
    store(costume.entries + 4, 0);
    for (uint32_t i = 0; i < count; ++i) {
        if (load(entries + 8 * i) == kCostumeOne) {
            store(costume.entries + 4, load(entries + 8 * i + 4));
            break;
        }
    }
}

// ------------------------------------------------------------------ extra bones (Kazumi's tiger)

// A fighter's extra bones (OPT bones, numbered in their names: `cha:OPT600_..`)
// have records in the model's bone container (model +80): 144 bytes, the
// bone's local transform first (4x4 row-major, rows the local axes, row 3 the
// translation). The title animates some fighters' extra bones (Devil Jin's
// wings) from part 3 of their motion pack: a move property (0x82de) starts one
// of its animations on the fighter's animator at player +7944 ({+8 the part-3
// MOTA, +12 0x4000 + its index, +16 the frame}); the animator's clock
// (sub_05BDC714) and the code posing the bones (called from sub_069D8B30) are
// written per fighter, for those fighters only. For the added fighter this
// does both: its part-3 animations (raw, 0xC8) have three channels for every
// extra bone from number 600 on (translation, rotation in radians as
// R = Rz Ry Rx, scale), and their last frame puts the bones back where they
// wait (the pack's tiger: far below the floor).
constexpr uint32_t kPlayerSelectId = 84, kPlayerAnimator = 7944;
constexpr uint32_t kAnimatorSet = 8, kAnimatorValue = 12, kAnimatorFrame = 16;
constexpr uint32_t kPart3 = 0x4000;         // an animator's value: an animation of part 3 (index below)
constexpr uint32_t kModelInterface = 12948; // the model's interface: its method +68 gives the player
constexpr uint32_t kModelBones = 80;
constexpr uint32_t kFirstExtraBone = 600;
std::map<uint32_t, uint32_t> g_extra_shown; // player -> the part-3 animation its extra bones show

float loadf(uint32_t address) {
    const uint32_t v = load(address);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void storef(uint32_t address, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    store(address, v);
}

// The animator's part-3 animation, if it plays a raw one.
uint32_t part3_animation(uint32_t animator) {
    const uint32_t value = load(animator + kAnimatorValue), set = load(animator + kAnimatorSet);
    if (!(value & kPart3) || set == 0) return 0;
    const uint32_t index = value & (kPart3 - 1);
    if (index >= load(set + 0xC)) return 0;
    const uint32_t animation = set + load(set + 0x14 + 4 * index);
    return (load(animation) >> 16) == 0xC8 ? animation : 0;
}

// A bone record's local transform from a translation, rotation and scale.
void store_local(uint32_t record, const float t[3], const float r[3], const float s[3]) {
    const float cx = std::cos(r[0]), sx = std::sin(r[0]), cy = std::cos(r[1]), sy = std::sin(r[1]);
    const float cz = std::cos(r[2]), sz = std::sin(r[2]);
    const float m[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                           {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                           {-sy, cy * sx, cy * cx}};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) storef(record + 4 * (4 * row + col), m[col][row] * s[row]);
        storef(record + 4 * (4 * row + 3), 0.0f);
        storef(record + 4 * (12 + row), t[row]);
    }
    storef(record + 60, 1.0f);
}

} // namespace

void prepare_roster() {
    const fs::path directory = packs_directory();
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return;
    std::vector<fs::path> candidates;
    for (const auto& item : fs::directory_iterator(directory, ec)) {
        // (a name starting with a dot: the importer's pack being written, or the one it replaces)
        if (item.path().filename().string().compare(0, 1, ".") == 0) continue;
        if (item.is_directory() && fs::is_regular_file(item.path() / "character.ini", ec)) {
            candidates.push_back(item.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());
    // Numbers: the one a pack names (the importer's choice, which the shared pictures follow), else the first free.
    // Packs of another format keep theirs (the importer's pack.places_of counts them too), unloaded.
    std::vector<Pack> packs;
    for (const fs::path& candidate : candidates) {
        if (auto pack = read_pack(candidate)) packs.push_back(std::move(*pack));
    }
    std::vector<const Pack*> at;
    for (const Pack& pack : packs) {
        if (pack.slot < 0 || pack.slot >= kMaxFighters) continue;
        if (at.size() <= static_cast<size_t>(pack.slot)) at.resize(pack.slot + 1, nullptr);
        if (!at[pack.slot]) at[pack.slot] = &pack;
    }
    for (const Pack& pack : packs) {
        if (std::find(at.begin(), at.end(), &pack) != at.end()) continue;
        const auto free = std::find(at.begin(), at.end(), nullptr);
        if (free != at.end()) {
            *free = &pack;
        } else if (at.size() < static_cast<size_t>(kMaxFighters)) {
            at.push_back(&pack);
        } else {
            std::fprintf(stderr, "ttt2: warning: %s: all %d places of added fighters are taken; skipped\n",
                         pack.directory.c_str(), kMaxFighters);
        }
    }
    g_added.assign(at.size(), Added{});
    g_voice.assign(at.size(), Voice{});
    g_art.assign(at.size(), Art{});
    g_costumes.assign(at.size(), Costume{});
    for (size_t k = 0; k < at.size(); ++k) {
        if (!at[k]) continue;
        const Pack& pack = *at[k];
        if (pack.format < kPackFormat) {
            std::fprintf(stderr,
                         "ttt2: warning: %s was made by an older importer (pack format %d, this game loads %d): add "
                         "it again (the launcher offers it); skipped\n",
                         pack.directory.c_str(), pack.format, kPackFormat);
            continue;
        }
        if (pack.format > kPackFormat) {
            std::fprintf(stderr,
                         "ttt2: warning: %s was made by a newer importer (pack format %d, this game loads %d): "
                         "update the game; skipped\n",
                         pack.directory.c_str(), pack.format, kPackFormat);
            continue;
        }
        const auto donor = fighter_id(pack.donor);
        const auto donor_row = donor ? motion_row(*donor) : std::nullopt;
        if (!donor || *donor >= 59 || !donor_row) {
            std::fprintf(stderr, "ttt2: warning: %s: no fighter `%s` to borrow from; skipped\n",
                         pack.directory.c_str(), pack.donor.c_str());
            continue;
        }
        if (fighter_id(pack.code) || std::any_of(g_added.begin(), g_added.end(), [&](const Added& a) {
                return a.active && a.code == pack.code;
            })) {
            std::fprintf(stderr, "ttt2: warning: %s: the code `%s` is a fighter's already; skipped\n",
                         pack.directory.c_str(), pack.code.c_str());
            continue;
        }
        Added& added = g_added[k];
        added.id = kFirstAddedId + static_cast<uint32_t>(k);
        added.records[0] = kFirstAddedRecord + 2 * static_cast<uint32_t>(k);
        added.records[1] = added.records[0] + 1;
        added.row = row_of(k);
        added.donor = *donor;
        added.donor_row = *donor_row;
        added.female = pack.female;
        added.code = pack.code;
        added.pack = pack.directory;
        added.weapons = pack.weapons;
        if (!add_entries(pack, *donor_row, k)) {
            std::fprintf(stderr, "ttt2: warning: %s could not be added\n", pack.directory.c_str());
            continue;
        }
        added.code_string = guest_string(pack.code);
        added.active = true;
        if (k < std::size(kCells)) {
            added.cell = kFirstAddedCell + static_cast<uint32_t>(k);
            added.column = kCells[k].column;
            added.grid_row = kCells[k].row;
        } else {
            std::fprintf(stderr, "ttt2: warning: %s: no free cell left in the select grid\n", pack.directory.c_str());
        }
        patch_title(k);
        prepare_art(pack, k, directory / "shared");
        prepare_costumes(pack, *donor, k);
        g_active = true;
        std::fprintf(stderr, "ttt2: added the fighter %s from %s (id %u)\n", pack.code.c_str(), pack.directory.c_str(),
                     added.id);
    }
    if (!g_active) return;
    add_shared(directory / "shared");
    patch_sounds();
    place_cells();
}

} // namespace cafe::title

using namespace cafe;
using namespace cafe::title;

// ------------------------------------------------------------------ calls into the title

// Calls guest function `f` with arguments; -> r3. The caller's registers and
// LR are kept (callers check LR after their calls).
uint32_t guest_call(PPCContext& ctx, uint8_t* base, void (*f)(PPCContext&, uint8_t*), std::initializer_list<uint32_t> args) {
    const PPCContext saved = ctx;
    uint32_t r = 3;
    for (uint32_t a : args) ctx.r[r++] = a;
    f(ctx, base);
    const uint32_t result = ctx.r[3];
    ctx = saved;
    return result;
}

// ------------------------------------------------------------------ registering the added fighters

// The title's key registry (*0x107A3FB4: per code, the keys of its body files
// and part packs by type, see facts) and chara master (*0x107A71E8: a map by
// select id of {id, motion row, name, code}, which loadPlayer searches by
// code) are built at start-up by sub_065CCD24, the registry by four
// functions of which sub_065C3128 runs last. After it, each added fighter is
// registered the same way: its code, its files' keys (the CRCs of their
// names) and its part packs' keys (`<code>/parts/<kind>`).
constexpr uint32_t kRegistry = 0x107A3FB4, kCharaMaster = 0x107A71E8;
PPC_FUNC(sub_06640368); // CodeString(this, char*)
PPC_FUNC(sub_0661CF78); // its destructor, then
PPC_FUNC(sub_0663F7C8);
PPC_FUNC(sub_0663F888); // registry: add a code
PPC_FUNC(sub_0663B054); // registry: (code, type, &key)
PPC_FUNC(sub_0663C34C); // the same
PPC_FUNC(sub_0663D644); // registry: (code, type, &key, index)
PPC_FUNC(sub_065DFA4C); // the chara master's strings: init(this)
PPC_FUNC(sub_065E8604); // assign(this, char*)
PPC_FUNC(sub_065E52A4); // destroy(this, 0)
PPC_FUNC(sub_06685CB8); // chara master: add(this, id, row, &name, &code)

void register_added(PPCContext& ctx, uint8_t* base) {
    static const uint32_t scratch = system_alloc(128);
    const uint32_t code = scratch, key = scratch + 32, name = scratch + 64, text = scratch + 96;
    for (const Added& added : g_added) {
        if (!added.active) continue;
        const uint32_t registry = load(kRegistry);
        guest_call(ctx, base, sub_06640368, {code, added.code_string});
        guest_call(ctx, base, sub_0663F888, {registry, code});
        // types 0-4: <code>.nud .nut .bon .mop .havok.xml; 6: <code>.xml
        const char* files[] = {".nud", ".nut", ".bon", ".mop", ".havok.xml"};
        for (uint32_t t = 0; t < 5; ++t) {
            store(key, key_of(added.code + files[t]));
            guest_call(ctx, base, sub_0663B054, {registry, code, t, key});
        }
        // 6 the item database, 7-9 and 11 the part packs (model, texture, bone, mop), 12 the bone pack again,
        // 15 the texture pack again, 10 and 13 keys nothing has
        const std::pair<uint32_t, uint32_t> keys[] = {
            {6, key_of(added.code + ".xml")}, {7, part_pack(added.code, 0)}, {8, part_pack(added.code, 1)},
            {9, part_pack(added.code, 2)}, {10, key_of(added.code + "/10")}, {11, part_pack(added.code, 3)},
            {13, key_of(added.code + "/13")}, {15, part_pack(added.code, 1)}, {12, part_pack(added.code, 2)}};
        for (const auto& [t, k] : keys) {
            store(key, k);
            guest_call(ctx, base, sub_0663C34C, {registry, code, t, key});
        }
        for (uint32_t i = 0; i < 20; ++i) {
            store(key, key_of(added.code + "/14/" + std::to_string(i)));
            guest_call(ctx, base, sub_0663D644, {registry, code, 14, key, i});
        }
        guest_call(ctx, base, sub_0661CF78, {code, 0});
        guest_call(ctx, base, sub_0663F7C8, {code, 0});
        // the chara master's record
        const uint32_t master = load(kCharaMaster);
        std::memcpy(guest_pointer(text), added.code.c_str(), 4);
        guest_call(ctx, base, sub_065DFA4C, {name});
        guest_call(ctx, base, sub_065E8604, {name, text});
        guest_call(ctx, base, sub_065DFA4C, {code});
        guest_call(ctx, base, sub_065E8604, {code, added.code_string});
        guest_call(ctx, base, sub_06685CB8, {master, added.id, added.row, name, code});
        guest_call(ctx, base, sub_065E52A4, {name, 0});
        guest_call(ctx, base, sub_065E52A4, {code, 0});
    }
}

PPC_FUNC(sub_065C3128_orig);
PPC_FUNC(sub_065C3128) {
    sub_065C3128_orig(ctx, base);
    if (g_active) register_added(ctx, base);
}

// ------------------------------------------------------------------ the added fighters' records

// The title's character database: a fighter's records (by select id) and a
// record's fields. The added fighters' records (140 on) are answered here.
PPC_FUNC(sub_033D502C_orig);
PPC_FUNC(sub_033D502C) { // records of id
    if (slot_of_id(ctx.r[3]) >= 0) {
        ctx.r[3] = 2;
        return;
    }
    sub_033D502C_orig(ctx, base);
}
PPC_FUNC(sub_033D504C_orig);
PPC_FUNC(sub_033D504C) { // first record of id
    if (const int k = slot_of_id(ctx.r[3]); k >= 0) {
        ctx.r[3] = g_added[k].records[0];
        return;
    }
    sub_033D504C_orig(ctx, base);
}
PPC_FUNC(sub_033D5064_orig);
PPC_FUNC(sub_033D5064) { // record i of id
    if (const int k = slot_of_id(ctx.r[3]); k >= 0) {
        ctx.r[3] = g_added[k].records[ctx.r[4] < 2 ? ctx.r[4] : 0];
        return;
    }
    sub_033D5064_orig(ctx, base);
}
// A record answered by an added fighter's records: k, its costume slot, or -1.
int g_proxy_record = -1;   // a record that answers the proxy id meanwhile (see sub_044BE1EC)
uint32_t g_proxy_id = 0;
#define RECORD_FIELD(name, value)                                                                                \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        if (const int k = slot_of_record(ctx.r[3]); k >= 0) {                                                    \
            const Added& added = g_added[k];                                                                     \
            const uint32_t costume = ctx.r[3] - added.records[0];                                                \
            (void)costume;                                                                                       \
            ctx.r[3] = (value);                                                                                  \
            return;                                                                                              \
        }                                                                                                        \
        name##_orig(ctx, base);                                                                                  \
    }
RECORD_FIELD(sub_033D50A8, g_proxy_record == static_cast<int>(ctx.r[3]) ? g_proxy_id : added.id) // its fighter
RECORD_FIELD(sub_033D50C8, costume)                                                              // its costume slot
RECORD_FIELD(sub_033D50E8, added.row)                                                            // its motion row
#undef RECORD_FIELD
// Its init callback (the fighters' one returns 1).
PPC_FUNC(sub_033D5108_orig);
PPC_FUNC(sub_033D5108) {
    if (slot_of_record(ctx.r[3]) >= 0) {
        ctx.r[3] = 1;
        return;
    }
    sub_033D5108_orig(ctx, base);
}

// Sets a player up from its record (player r3, record at +88): select id at
// +84, motion row at +80, the record's words +24..+76 at +1036 on (+1044 on:
// the look-at's limits in degrees, which sub_03BF7C7C turns into the
// look-at's state when the fight loads). An added fighter's record has none
// of those words, so it is set up from its donor's record, the look-at's yaw
// limits widened: a Tekken 7 stance turns the chest further from the opponent
// than a TTT2 one, and while the fighter stands the look-at replaces the
// animation's neck and head.
constexpr uint32_t kPlayerRecord = 88, kPlayerRow = 80;
constexpr uint32_t kPlayerLookYawMax = 1060, kPlayerLookYawMin = 1064; // degrees, the neck's and the head's each
constexpr float kAddedLookYaw = 60.0f;                                  // TTT2's fighters: 30
PPC_FUNC(sub_033D5BBC_orig);
PPC_FUNC(sub_033D5BBC) {
    const uint32_t player = ctx.r[3];
    const int k = slot_of_record(load(player + kPlayerRecord));
    if (std::getenv("TTT2_TRACE_ROSTER")) {
        std::fprintf(stderr, "ttt2: roster: player %08X set up: select id %u, record %08X (slot %d), row %u\n", player,
                     load(player + kPlayerSelectId), load(player + kPlayerRecord), k, load(player + kPlayerRow));
    }
    if (k < 0) {
        sub_033D5BBC_orig(ctx, base);
        return;
    }
    const uint32_t record = load(player + kPlayerRecord);
    store(player + kPlayerRecord, first_record(g_added[k].donor)); // the donor's record does the rest
    sub_033D5BBC_orig(ctx, base);
    store(player + kPlayerRecord, record);
    store(player + kPlayerSelectId, g_added[k].id);
    store(player + kPlayerRow, g_added[k].row);
    storef(player + kPlayerLookYawMax, kAddedLookYaw);
    storef(player + kPlayerLookYawMin, -kAddedLookYaw);
}

// Builds each fighter's list of records and each motion row's record: an
// added fighter's row's record is its first.
PPC_FUNC(sub_033D512C_orig);
PPC_FUNC(sub_033D512C) {
    sub_033D512C_orig(ctx, base);
    for (const Added& added : g_added) {
        if (added.active && added.row < 68) store(kRowRecords + 4 * added.row, added.records[0]);
    }
}

// Whether a fighter can be picked; never for ids above 58.
PPC_FUNC(sub_033D52B4_orig);
PPC_FUNC(sub_033D52B4) {
    if (slot_of_id(ctx.r[3]) >= 0) {
        ctx.r[3] = 1;
        return;
    }
    sub_033D52B4_orig(ctx, base);
}

// ------------------------------------------------------------------ loading an added fighter

// Issues `loadPlayer PLAYERn ... <code>` for a loader slot (loader r3, slot
// r4, record r5): the code from the code table by select id, refused for ids
// from 63 on. For an added fighter's record it runs with a proxy: Paul's id
// for the record and the added fighter's code in Paul's place of the code
// table, then the player gets the added fighter's id.
PPC_FUNC(sub_05C5EF74); // the player of a slot
PPC_FUNC(sub_044BE1EC_orig);
PPC_FUNC(sub_044BE1EC) {
    const int k = slot_of_record(ctx.r[5]);
    if (k < 0) {
        sub_044BE1EC_orig(ctx, base);
        return;
    }
    const uint32_t slot = ctx.r[4], paul_code = load(kCodes);
    store(kCodes, g_added[k].code_string);
    g_proxy_record = static_cast<int>(ctx.r[5]);
    g_proxy_id = 0;
    sub_044BE1EC_orig(ctx, base);
    g_proxy_record = -1;
    store(kCodes, paul_code);
    if (const uint32_t player = guest_call(ctx, base, sub_05C5EF74, {slot})) store(player + kPlayerSelectId, g_added[k].id);
}

// Reads a motion row's keys (holder r3, row r4): a later added fighter's row
// (69 on) has its keys put in the spare row meanwhile.
PPC_FUNC(sub_075DA0C8_orig);
PPC_FUNC(sub_075DA0C8) {
    const uint32_t row = ctx.r[4];
    const auto it = std::find_if(g_added.begin(), g_added.end(), [&](const Added& a) { return a.active && a.row == row; });
    if (row < kFirstVirtualRow || it == g_added.end()) {
        sub_075DA0C8_orig(ctx, base);
        return;
    }
    store(kMotionRows + 12 * kSpareRow, key_of(it->code + kMotionName));
    store(kMotionRows + 12 * kSpareRow + 4, key_of(it->code + kMovesetName));
    store(kMotionRows + 12 * kSpareRow + 8, key_of(it->code + kMovelistName));
    ctx.r[4] = kSpareRow;
    sub_075DA0C8_orig(ctx, base);
}

// The team's fighter ids as bytes in the match set-up (a side's struct +640,
// a member every 4288 bytes): the title caps them at 62; an added fighter's
// id (64 on) fits a byte, and is put back after the setters.
constexpr uint32_t kMatchObject = 0x10872CB8, kMatchSides = 244, kSideIds = 640, kSideMember = 4288;
uint32_t match_side(uint32_t player) {
    const uint32_t object = load(kMatchObject);
    const uint32_t sides = object ? load(object + kMatchSides) : 0;
    return sides ? load(sides + 60 + 4 * player) : 0;
}
// The picks, as the select screen commits them (player, member): the id
// whose byte the setters below may have capped (only ZAKO B, of the Fight
// Lab, is 62 otherwise).
uint32_t g_picks[2][2] = {{0, 0}, {0, 0}};
void fix_side_ids() {
    for (uint32_t p = 0; p < 2; ++p) {
        const uint32_t side = match_side(p);
        if (!side) continue;
        for (uint32_t m = 0; m < 2; ++m) {
            const uint32_t at = side + kSideIds + kSideMember * m;
            if (slot_of_id(g_picks[p][m]) >= 0 && load8(at) == 62) store8(at, static_cast<uint8_t>(g_picks[p][m]));
        }
    }
}
// The modes that set the match's fighters up themselves, drawing the CPU's:
// after their set-ups, the picks are the mode manager's records (side +
// 2 * member). The mode manager: *(*(*0x10872CB8 + 228) + 228) (sub_06ECB1B0).
constexpr uint32_t kModeKind = 444, kModeRecords = 428, kModeCpuSide = 667;
constexpr uint32_t kArcade = 0, kAttract = 2, kSurvival = 10, kTimeAttack = 11, kGhostBattle = 12, kTeamBattle = 14,
                   kTekkenBall = 22, kMushroom = 23; // the mode kinds (online ones: 16, 17, 24)
uint32_t mode_manager() {
    const uint32_t object = load(kMatchObject);
    const uint32_t match = object ? load(object + 228) : 0;
    return match ? load(match + 228) : 0;
}
bool drawing_mode(uint32_t kind) {
    for (uint32_t k : {kArcade, kAttract, kSurvival, kTimeAttack, kGhostBattle, kTeamBattle, kTekkenBall, kMushroom}) {
        if (kind == k) return true;
    }
    return false;
}
void sync_picks() {
    const uint32_t mode = mode_manager();
    if (!g_active || !mode || !drawing_mode(load(mode + kModeKind))) return;
    for (uint32_t p = 0; p < 2; ++p) {
        for (uint32_t m = 0; m < 2; ++m) {
            const int k = slot_of_record(load(mode + kModeRecords + 4 * (p + 2 * m)));
            g_picks[p][m] = k >= 0 ? g_added[k].id : 0;
        }
    }
    fix_side_ids();
}
// The select's cursors' masks of the ids still free (bit id of the 64 bits
// +352:+356): an added fighter's id (64 + k) is bit k there, TTT2 fighter k's.
// So an added fighter's pick keeps the mask as it was, and while a pick of
// an added fighter is checked (sub_033E265C) bit k says whether it is free.
constexpr uint32_t kCursorMaskHigh = 352, kCursorMaskLow = 356, kCursorPicked = 364, kCursorRecords = 372;
constexpr uint32_t kCursorTeamMax = 8;
bool mask_bit(uint32_t cursor, uint32_t id) {
    if (id >= 64) return false;
    return (load(cursor + (id < 32 ? kCursorMaskLow : kCursorMaskHigh)) >> (id & 31) & 1) != 0;
}
void set_mask_bit(uint32_t cursor, uint32_t id, bool on) {
    if (id >= 64) return;
    const uint32_t at = cursor + (id < 32 ? kCursorMaskLow : kCursorMaskHigh), bit = 1u << (id & 31);
    store(at, on ? load(at) | bit : load(at) & ~bit);
}
// The added fighters a cursor's team has picked.
std::set<uint32_t> team_added(uint32_t cursor) {
    std::set<uint32_t> out;
    const uint32_t picked = std::min(load(cursor + kCursorPicked), kCursorTeamMax);
    for (uint32_t j = 0; j < picked; ++j) {
        if (const int k = slot_of_record(load(cursor + kCursorRecords + 4 * j)); k >= 0) out.insert(g_added[k].id);
    }
    return out;
}
int g_lent = -1;                // the added fighter whose bit is lent (sub_033E265C), or -1
bool g_lent_bit = false;        // and TTT2 fighter k's own bit meanwhile
// Select screen: a pick (player r3, record r4).
PPC_FUNC(sub_033E2530_orig);
PPC_FUNC(sub_033E2530) {
    const uint32_t player = ctx.r[3], member = load(player + 364), record = ctx.r[4], number = load(player + 340);
    const uint32_t masks[2] = {load(player + kCursorMaskHigh), load(player + kCursorMaskLow)};
    sub_033E2530_orig(ctx, base);
    const int k = slot_of_record(record);
    if (k >= 0) {
        store(player + kCursorMaskHigh, masks[0]);
        store(player + kCursorMaskLow, masks[1]);
    } else if (g_lent >= 0 && guest_call(ctx, base, sub_033D50A8, {record}) == static_cast<uint32_t>(g_lent)) {
        g_lent_bit = false; // TTT2 fighter k picked while the bit was lent
    }
    if (number < 2 && member < 2) g_picks[number][member] = k >= 0 ? g_added[k].id : 0;
    fix_side_ids();
}
// A player's side (sides r3, player r4; a thunk to sub_03800838), as the
// screens after the select (VS) find it: its members' bytes as the picks were.
PPC_FUNC(sub_06EEAEE4_orig);
PPC_FUNC(sub_06EEAEE4) {
    if (g_active) fix_side_ids();
    sub_06EEAEE4_orig(ctx, base);
}
// Both members' ids (side r3, ids r4 and r5).
PPC_FUNC(sub_04FA571C_orig);
PPC_FUNC(sub_04FA571C) {
    const uint32_t side = ctx.r[3], ids[2] = {ctx.r[4], ctx.r[5]};
    sub_04FA571C_orig(ctx, base);
    for (uint32_t p = 0; p < 2; ++p) {
        if (side == 0 || match_side(p) != side) continue;
        for (uint32_t m = 0; m < 2; ++m) g_picks[p][m] = slot_of_id(ids[m]) >= 0 ? ids[m] : g_picks[p][m];
    }
    for (uint32_t m = 0; m < 2; ++m) {
        if (slot_of_id(ids[m]) >= 0 && side) store8(side + kSideIds + kSideMember * m, static_cast<uint8_t>(ids[m]));
    }
}
// The other functions that write the bytes.
#define SIDE_IDS(name)                                                                                           \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        name##_orig(ctx, base);                                                                                  \
        if (g_active) fix_side_ids();                                                                            \
    }
SIDE_IDS(sub_021F0124)
SIDE_IDS(sub_02675744)
SIDE_IDS(sub_02676034)
SIDE_IDS(sub_038014DC)
SIDE_IDS(sub_04FA7A14)
SIDE_IDS(sub_04FA7B84)
SIDE_IDS(sub_06EE967C)
SIDE_IDS(sub_07542DC0)
#undef SIDE_IDS
// A side's profile (profile r3, player r4, kind r5: 5 the CPU's, ids r6 and
// r7, as the records say): its bytes are written before it is the side's, so
// they are put back here; a CPU's ids are its picks (none left from a select).
constexpr uint32_t kCpuProfile = 5;
PPC_FUNC(sub_076D6CB0_orig);
PPC_FUNC(sub_076D6CB0) {
    const uint32_t profile = ctx.r[3], player = ctx.r[4], kind = ctx.r[5] & 0xFF, ids[2] = {ctx.r[6], ctx.r[7]};
    sub_076D6CB0_orig(ctx, base);
    if (!g_active) return;
    for (uint32_t m = 0; m < 2; ++m) {
        const bool added = slot_of_id(ids[m]) >= 0;
        if (added && profile) store8(profile + kSideIds + kSideMember * m, static_cast<uint8_t>(ids[m]));
        if (player < 2 && (added || kind == kCpuProfile)) g_picks[player][m] = added ? ids[m] : 0;
    }
    fix_side_ids();
}
// Team Battle's round (battle r3, side r4): the side's next two members to
// the mode manager's records, and their bytes.
PPC_FUNC(sub_0292C084_orig);
PPC_FUNC(sub_0292C084) {
    sub_0292C084_orig(ctx, base);
    sync_picks();
    if (g_active) fix_side_ids();
}

// ------------------------------------------------------------------ the added fighters as CPU opponents

// The modes that draw the CPU's fighters at random draw TTT2's ids (0..62,
// less those a mode leaves out). Each such draw is widened here so every
// added fighter whose pack loaded has exactly the chance of an eligible TTT2
// fighter. A draw of n fighters, in order and none twice, from `eligible`
// TTT2 fighters is the first n of a uniform order of them; the first n of a
// uniform order of those and the added fighters is that order and the added
// fighters' (shuffled) interleaved at random: the next comes from the added
// ones with probability (added left) / (all left). Fixed fights (Arcade's and
// Tekken Ball's bosses) are not draws and stay. TTT2_FORCE_ADDED=<code>[,..]
// takes those fighters first (testing); TTT2_TRACE_ROSTER logs each draw.
std::mt19937& draws() {
    static std::mt19937 generator{std::random_device{}()};
    return generator;
}
uint32_t below(uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(draws()); }

std::vector<uint32_t> added_pool(const std::set<uint32_t>& left_out) {
    std::vector<uint32_t> out;
    for (const Added& added : g_added) {
        if (added.active && left_out.count(added.id) == 0) out.push_back(added.id);
    }
    return out;
}

std::vector<uint32_t> widen(const char* mode, const std::vector<uint32_t>& picks, uint32_t eligible,
                            std::vector<uint32_t> added, size_t n) {
    std::shuffle(added.begin(), added.end(), draws());
    size_t forced = 0;
    if (const char* env = std::getenv("TTT2_FORCE_ADDED")) {
        std::stringstream codes(env);
        for (std::string code; std::getline(codes, code, ',');) {
            const auto it = std::find_if(added.begin() + static_cast<long>(forced), added.end(),
                                         [&](uint32_t id) { return g_added[id - kFirstAddedId].code == code; });
            if (it != added.end()) std::iter_swap(added.begin() + static_cast<long>(forced++), it);
        }
    }
    std::vector<uint32_t> out;
    size_t next_pick = 0, next_added = 0;
    uint32_t left = std::max<uint32_t>(eligible, static_cast<uint32_t>(picks.size()));
    while (out.size() < n) {
        const uint32_t more = static_cast<uint32_t>(added.size() - next_added);
        if (more && (next_added < forced || next_pick >= picks.size() || below(left + more) < more)) {
            out.push_back(added[next_added++]);
        } else if (next_pick < picks.size()) {
            out.push_back(picks[next_pick++]);
            --left;
        } else {
            break;
        }
    }
    if (std::getenv("TTT2_TRACE_ROSTER")) {
        std::string was, now;
        for (uint32_t id : picks) was += ' ' + std::to_string(id);
        for (uint32_t id : out) now += ' ' + std::to_string(id);
        std::fprintf(stderr, "ttt2: roster: %s draw: %u TTT2 + %zu added fighters:%s ->%s\n", mode, eligible,
                     added.size(), was.c_str(), now.c_str());
    }
    return out;
}

// Whether TTT2's own fighter `id` can be picked (sub_033D52B4).
bool available(PPCContext& ctx, uint8_t* base, uint32_t id) { return guest_call(ctx, base, sub_033D52B4, {id}) != 0; }

// Arcade (and Time Attack, which runs Arcade's code; the online modes do too
// and are left alone), Tekken Ball and Mushroom Battle draw alike as they
// start: ids[63] = 0..62 shuffled, a vector of the ids left out (the player's,
// the locked ones, one of each look-alike pair, ...), then the first n ids not
// left out, in the array's order. The vector's size() is asked before each
// id's check: at the first id's, the array is reordered so the widened draw
// comes first.
struct ShuffleDraw {
    uint32_t lr;                          // the first id's size() call returns here
    uint32_t index, array, candidate, mode; // registers: the id's index, ids[63], the id, the mode manager
    uint32_t kinds[2];
    uint32_t pairs;                       // n: 2 * the mode's +780 (Arcade: 6 random stages), or 2 if 0
    const char* name;
};
constexpr uint32_t kShuffleIds = 63, kModeRandomStages = 780;
constexpr uint32_t kCombot = 49, kLastFighter = 58; // the draws of ids 0..58 leave Combot out
constexpr ShuffleDraw kArcadeDraw = {0x06ED0538, 27, 29, 28, 30, {kArcade, kTimeAttack}, kModeRandomStages, "arcade"};
constexpr ShuffleDraw kMushroomDraw = {0x028AEC78, 26, 27, 28, 31, {kMushroom, kMushroom}, 0, "mushroom battle"};
constexpr ShuffleDraw kTekkenBallDraw = {0x02B2F750, 26, 29, 27, 30, {kTekkenBall, kTekkenBall}, kModeRandomStages,
                                         "tekken ball"};

void widen_shuffle(PPCContext& ctx, const ShuffleDraw& draw) {
    const uint32_t mode = ctx.r[draw.mode], array = ctx.r[draw.array], left_out_vector = ctx.r[3];
    const uint32_t kind = load(mode + kModeKind);
    if (!g_active || ctx.lr != draw.lr || ctx.r[draw.index] != 0 || (kind != draw.kinds[0] && kind != draw.kinds[1]))
        return;
    std::set<uint32_t> left_out;
    for (uint32_t at = load(left_out_vector + 4), end = load(left_out_vector + 8); at && at < end; at += 4)
        left_out.insert(load(at));
    std::vector<uint32_t> ids(kShuffleIds), order;
    for (uint32_t i = 0; i < kShuffleIds; ++i) {
        ids[i] = load(array + 4 * i);
        if (left_out.count(ids[i]) == 0) order.push_back(ids[i]);
    }
    const size_t n = draw.pairs ? 2 * std::min<uint32_t>(load(mode + draw.pairs), 9) : 2;
    const std::vector<uint32_t> picks(order.begin(), order.begin() + static_cast<long>(std::min(n, order.size())));
    const std::vector<uint32_t> drawn =
        widen(draw.name, picks, static_cast<uint32_t>(order.size()), added_pool(left_out), n);
    if (drawn == picks) return;
    std::vector<uint32_t> now = drawn;
    for (uint32_t id : ids) {
        if (now.size() < kShuffleIds && std::find(drawn.begin(), drawn.end(), id) == drawn.end()) now.push_back(id);
    }
    for (uint32_t i = 0; i < kShuffleIds; ++i) store(array + 4 * i, now[i]);
    ctx.r[draw.candidate] = now[0];
}
#define SHUFFLE_DRAW(name, draw)                                                                                 \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        widen_shuffle(ctx, draw);                                                                                \
        name##_orig(ctx, base);                                                                                  \
    }
SHUFFLE_DRAW(sub_06ED4ACC, kArcadeDraw)     // the vectors' size(), each mode its own
SHUFFLE_DRAW(sub_028B2EF4, kMushroomDraw)
SHUFFLE_DRAW(sub_02B34468, kTekkenBallDraw)
#undef SHUFFLE_DRAW

// The modes' stage set-ups (mode r3) that put the drawn fighters' records in
// the mode manager's: Arcade's and Time Attack's (sub_06ED06B8), Mushroom
// Battle's, Tekken Ball's.
#define STAGE_SET_UP(name)                                                                                       \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        name##_orig(ctx, base);                                                                                  \
        sync_picks();                                                                                            \
    }
STAGE_SET_UP(sub_06ED06B8)
STAGE_SET_UP(sub_028AED64)
STAGE_SET_UP(sub_02B2F9DC)
STAGE_SET_UP(sub_028A42A8) // Ghost Battle's
#undef STAGE_SET_UP

// Ghost Battle draws its ghosts' fighters with sub_028A2DA8(mode r3, strong
// r4, ids left out r5..r7 (63: none)): for the three cards of the next choice
// (kGhostCards; strong 1 in 7) and the first stage's fighter (kGhostFirst).
// It is an id 0..58 again until available, not Combot, not left out; a strong
// card's is one with customisation items left to win, else (none such) the
// same. Added fighters have no items: they join the draws that are uniform.
constexpr uint32_t kGhostCards = 0x028A30E0, kGhostFirst = 0x028A4400;
PPC_FUNC(sub_028A2748); // the customisation items a fighter (r3) has left to win (levels below r4)
PPC_FUNC(sub_028A2DA8_orig);
PPC_FUNC(sub_028A2DA8) {
    const uint32_t mode = ctx.r[3], strong = ctx.r[4], lr = ctx.lr;
    const std::set<uint32_t> left_out = {ctx.r[5], ctx.r[6], ctx.r[7]};
    sub_028A2DA8_orig(ctx, base);
    if (!g_active || (lr != kGhostCards && lr != kGhostFirst) || load(mode + kModeKind) != kGhostBattle) return;
    uint32_t eligible = 0, rewarding = 0;
    for (uint32_t id = 0; id <= kLastFighter; ++id) {
        if (id == kCombot || left_out.count(id) || !available(ctx, base, id)) continue;
        ++eligible;
        if (strong && guest_call(ctx, base, sub_028A2748, {id, 5, 0}) != 0) ++rewarding;
    }
    if (rewarding) return; // a strong card of TTT2's fighters with items left (or the player's own fighter)
    const std::vector<uint32_t> drawn = widen("ghost battle", {ctx.r[3]}, eligible, added_pool(left_out), 1);
    if (!drawn.empty()) ctx.r[3] = drawn[0];
}

// A ghost's entry (out r3, ghosts r4, id r5, rank band r6, flag r7) from the
// ghost list (*(ghosts + 16): 59 fighters x 10 bands x {id, partner, rank,
// rank}), also Arcade's CPU's: an added fighter's is its donor's ghost's, in
// Ghost Battle with the partner too (none if the donor's ghost fights alone),
// without the strong ghosts' reward (whose counters, by id % 63, are another
// fighter's).
constexpr uint32_t kGhostPartner = 1, kGhostFlag = 12;
bool ghost_battle() {
    const uint32_t mode = mode_manager();
    return mode && load(mode + kModeKind) == kGhostBattle;
}
PPC_FUNC(sub_04DC9F20_orig);
PPC_FUNC(sub_04DC9F20) {
    const uint32_t out = ctx.r[3], id = ctx.r[5];
    const int k = slot_of_id(id);
    if (k < 0) {
        sub_04DC9F20_orig(ctx, base);
        return;
    }
    ctx.r[5] = g_added[k].donor;
    sub_04DC9F20_orig(ctx, base);
    if (!out) return;
    store8(out, static_cast<uint8_t>(id));
    if (!ghost_battle() || load8(out + kGhostPartner) == g_added[k].donor) store8(out + kGhostPartner, static_cast<uint8_t>(id));
    store8(out + kGhostFlag, 0);
}

// A ghost's data: the band file of its rank (59 fighters' records, each two
// GOST blocks: its first member's, its partner's) holds per block the
// fighter (+20), rank (+24), wins and losses (+28, +32), the player's name
// (+0x1118) and the inputs the CPU replays by situation. Ghost Battle copies a
// ghost's (ghosts r3: its entry at +20, see sub_04DC9F20) into the CPU's
// members' blocks (r4) with sub_04DCAD34 (the first member's, fighter r5)
// and sub_04DCAE7C (the partner's, fighter r5: the entry's again), both
// refusing fighters from 59, so an added ghost fought with no name, record
// or partner. Its donor's ghost is copied instead, as the added fighter (the
// recorded inputs are buttons and directions, any fighter's), with the
// donor's ghost's partner, or none when that ghost fights alone. Other modes
// (Arcade's CPU) keep the refusal: their fighters are drawn, not a ghost's.
constexpr uint32_t kGhostEntry = 20, kBlockFighter = 20;
void trace_ghost_block(const char* member, uint32_t block) {
    if (!std::getenv("TTT2_TRACE_ROSTER") || !block) return;
    char name[32] = {};
    std::memcpy(name, guest_pointer(block + 0x1118), sizeof name - 1);
    std::fprintf(stderr, "ttt2: roster: ghost %s: fighter %u, rank %u, %u wins %u losses, \"%s\"\n", member,
                 load(block + kBlockFighter), load(block + 24), load(block + 28), load(block + 32), name);
}
PPC_FUNC(sub_04DCAD34_orig);
PPC_FUNC(sub_04DCAD34) {
    const uint32_t block = ctx.r[4];
    const int k = slot_of_id(ctx.r[5]);
    if (k < 0 || !ghost_battle()) {
        sub_04DCAD34_orig(ctx, base);
        return;
    }
    ctx.r[5] = g_added[k].donor;
    sub_04DCAD34_orig(ctx, base);
    if (ctx.r[3] && block) store(block + kBlockFighter, g_added[k].id);
    trace_ghost_block("member", block);
}
PPC_FUNC(sub_04DCAE7C_orig);
PPC_FUNC(sub_04DCAE7C) {
    const uint32_t ghosts = ctx.r[3], block = ctx.r[4];
    const int k = slot_of_id(ctx.r[5]);
    if (k < 0 || !ghost_battle()) {
        sub_04DCAE7C_orig(ctx, base);
        return;
    }
    ctx.r[5] = g_added[k].donor;
    sub_04DCAE7C_orig(ctx, base);
    const uint32_t partner = load8(ghosts + kGhostEntry + kGhostPartner);
    if (ctx.r[3] && block && (partner == g_added[k].id || load(block + kBlockFighter) == g_added[k].donor))
        store(block + kBlockFighter, g_added[k].id); // alone: the partner's block is its own
    trace_ghost_block("partner", block);
}

// The ghost cards' faces: the label face_<code>, a string of the array of
// the 59 fighters' (0x10864000, 32 bytes each) passed to sub_029522EC(ui,
// node, string r5) at kGhostFaces (the card's fighter, its partner): for an
// added fighter, one of its own (the layouts have its label beside Paul's).
constexpr uint32_t kGhostFaceStrings = 0x10864000, kGhostFaces[] = {0x0297381C, 0x02973868};
std::vector<uint32_t> g_face_strings; // by slot: its string object, 0 until made
PPC_FUNC(sub_029522EC_orig);
PPC_FUNC(sub_029522EC) {
    const uint32_t string = ctx.r[5];
    if (g_active && (ctx.lr == kGhostFaces[0] || ctx.lr == kGhostFaces[1]) && string >= kGhostFaceStrings + kStringSize * 59 &&
        (string - kGhostFaceStrings) % kStringSize == 0) {
        const int k = slot_of_id((string - kGhostFaceStrings) / kStringSize);
        if (k < 0) {
            ctx.r[5] = kGhostFaceStrings; // never a string past the array
        } else {
            g_face_strings.resize(g_added.size());
            if (g_face_strings[k] == 0) {
                const std::string label = "face_" + g_added[k].code;
                const uint32_t object = system_alloc(kStringSize);
                std::memcpy(guest_pointer(object), guest_pointer(kGhostFaceStrings), kStringSize);
                store(object + kStringPointer, guest_string(label));
                store(object + kStringLength, static_cast<uint32_t>(label.size()));
                store(object + kStringCapacity, std::max<uint32_t>(static_cast<uint32_t>(label.size()), 16));
                g_face_strings[k] = object;
            }
            ctx.r[5] = g_face_strings[k];
        }
    }
    sub_029522EC_orig(ctx, base);
}

// Survival's stage set-up (mode r3) draws the next two: ids 0..58 again until
// one is available, not Combot, not one of the stage before's two (+780,
// +784) and not the first; their records (costume 0) go to the CPU side's.
constexpr uint32_t kSurvivalIds = 780;
PPC_FUNC(sub_02929460_orig);
PPC_FUNC(sub_02929460) {
    const uint32_t mode = ctx.r[3];
    const uint32_t before[2] = {load(mode + kSurvivalIds), load(mode + kSurvivalIds + 4)};
    sub_02929460_orig(ctx, base);
    if (!g_active || load(mode + kModeKind) != kSurvival) return;
    const std::vector<uint32_t> picks = {load(mode + kSurvivalIds), load(mode + kSurvivalIds + 4)};
    if (picks[0] != before[0] && picks[0] != before[1]) { // drawn now (an id is never the stage before's)
        const std::set<uint32_t> last = {before[0], before[1]};
        uint32_t eligible = 0;
        for (uint32_t id = 0; id <= kLastFighter; ++id) {
            if (id != kCombot && last.count(id) == 0 && available(ctx, base, id)) ++eligible;
        }
        const std::vector<uint32_t> drawn = widen("survival", picks, eligible, added_pool(last), 2);
        if (drawn.size() == 2 && drawn != picks) {
            const uint32_t cpu = load8(mode + kModeCpuSide);
            for (uint32_t m = 0; m < 2; ++m) {
                store(mode + kSurvivalIds + 4 * m, drawn[m]);
                store(mode + kModeRecords + 4 * (cpu + 2 * m), guest_call(ctx, base, sub_033D5064, {drawn[m], 0}));
            }
        }
    }
    sync_picks();
}

// Team Battle: the members nobody picked (all of the CPU's team) are drawn as
// the select ends (sub_033E942C), one at a time: an id 0..62 again until it is
// available, not Combot for the CPU, and free in the cursor's mask; its bit is
// cleared and sub_0292BC10(battle, side, member, record, 1) adds it, called
// at kTeamDraw (cursor r25, side r24, member r31, the CPU's r21, id r22).
// There an added fighter not in the team takes the place with probability
// (added left) / (all left), the TTT2 fighter's bit set again.
constexpr uint32_t kTeamDraw = 0x033E967C;
std::set<uint32_t> g_team_drawn[2]; // the added fighters drawn into a side's team
PPC_FUNC(sub_0292BC10_orig);
PPC_FUNC(sub_0292BC10) {
    const uint32_t mode = mode_manager();
    if (g_active && ctx.lr == kTeamDraw && ctx.r[7] == 1 && mode && load(mode + kModeKind) == kTeamBattle) {
        const uint32_t cursor = ctx.r[25], side = ctx.r[24] & 1, member = ctx.r[31], id = ctx.r[22];
        const bool cpu = ctx.r[21] != 0;
        if (member == load(cursor + kCursorPicked)) g_team_drawn[side].clear(); // its first drawn member
        std::set<uint32_t> team = team_added(cursor);
        team.insert(g_team_drawn[side].begin(), g_team_drawn[side].end());
        uint32_t eligible = 1; // the drawn one
        for (uint32_t i = 0; i < kShuffleIds; ++i) {
            if (i != id && mask_bit(cursor, i) && !(cpu && i == kCombot) && available(ctx, base, i)) ++eligible;
        }
        const std::vector<uint32_t> drawn = widen("team battle", {id}, eligible, added_pool(team), 1);
        if (!drawn.empty() && drawn[0] != id) {
            set_mask_bit(cursor, id, true);
            ctx.r[6] = g_added[slot_of_id(drawn[0])].records[0];
            g_team_drawn[side].insert(drawn[0]);
        }
    }
    sub_0292BC10_orig(ctx, base);
}

// The attract demo's set-up (sub_06ED6740) draws four fighters, sides' members
// (index r20), each from sub_04596A74(settings r3): ids 0..57 its unlocks and a
// history of the last 19 (settings +124) allow, called at kAttractDraw. An
// added fighter not yet in the demo takes the place as above (its eligible
// count: the available ids not in the history).
constexpr uint32_t kAttractDraw = 0x06ED6820, kAttractHistory = 124, kAttractHistoryLength = 19;
constexpr uint32_t kAttractLast = 57;
std::set<uint32_t> g_attract_drawn;
PPC_FUNC(sub_04596A74_orig);
PPC_FUNC(sub_04596A74) {
    const uint32_t settings = ctx.r[3], lr = ctx.lr, index = ctx.r[20];
    sub_04596A74_orig(ctx, base);
    const uint32_t mode = mode_manager();
    if (!g_active || lr != kAttractDraw || !mode || load(mode + kModeKind) != kAttract) return;
    if (index == 0) g_attract_drawn.clear();
    const uint32_t id = ctx.r[3];
    std::set<uint32_t> history;
    for (uint32_t j = 0; j < kAttractHistoryLength; ++j) history.insert(load(settings + kAttractHistory + 4 * j));
    uint32_t eligible = 1;
    for (uint32_t i = 0; i <= kAttractLast; ++i) {
        if (i != id && history.count(i) == 0 && available(ctx, base, i)) ++eligible;
    }
    const std::vector<uint32_t> drawn = widen("attract", {id}, eligible, added_pool(g_attract_drawn), 1);
    if (!drawn.empty() && drawn[0] != id) {
        ctx.r[3] = drawn[0];
        g_attract_drawn.insert(drawn[0]);
    }
}
PPC_FUNC(sub_06ED6740_orig);
PPC_FUNC(sub_06ED6740) {
    sub_06ED6740_orig(ctx, base);
    sync_picks();
}

// ------------------------------------------------------------------ tables by fighter, read past their ends

// Functions that index a table of the 63 fighters by an id (in r4) with no
// bound: an added fighter's donor's row.
#define DONOR_ARGUMENT(name)                                                                                     \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        if (const int k = slot_of_id(ctx.r[4]); k >= 0) ctx.r[4] = g_added[k].donor;                             \
        name##_orig(ctx, base);                                                                                  \
    }
DONOR_ARGUMENT(sub_0241D69C)
DONOR_ARGUMENT(sub_0241D840)
DONOR_ARGUMENT(sub_0267E7A4)
DONOR_ARGUMENT(sub_0267ED3C)
#undef DONOR_ARGUMENT

// Functions that read a player's select id (+84) and index such a table: the
// donor's id meanwhile.
void with_donor_id(PPCContext& ctx, uint8_t* base, uint32_t player, void (*f)(PPCContext&, uint8_t*)) {
    const int k = player ? slot_of_id(load(player + kPlayerSelectId)) : -1;
    if (k < 0) {
        f(ctx, base);
        return;
    }
    store(player + kPlayerSelectId, g_added[k].donor);
    f(ctx, base);
    if (load(player + kPlayerSelectId) == g_added[k].donor) store(player + kPlayerSelectId, g_added[k].id);
}
PPC_FUNC(sub_03D6B9F4_orig);
PPC_FUNC(sub_03D6B9F4) { with_donor_id(ctx, base, ctx.r[3], sub_03D6B9F4_orig); }
PPC_FUNC(sub_0399221C_orig);
PPC_FUNC(sub_0399221C) { with_donor_id(ctx, base, load(0x10874864), sub_0399221C_orig); }
// The CPU's play (sub_039A6214, for the player at 0x10874864) reads a table of
// 67 fighters (0x101B9E70, 16-byte rows) by the player's motion row (+80) with
// no bound: added fighters from place 5 on have rows 69 and up, and the
// attract demo with Lidia (row 76) read past it and crashed. The donor's row
// meanwhile.
void with_donor_row(PPCContext& ctx, uint8_t* base, uint32_t player, void (*f)(PPCContext&, uint8_t*)) {
    const int k = player ? slot_of_id(load(player + kPlayerSelectId)) : -1;
    if (k < 0 || load(player + kPlayerRow) != g_added[k].row) {
        f(ctx, base);
        return;
    }
    store(player + kPlayerRow, g_added[k].donor_row);
    f(ctx, base);
    if (load(player + kPlayerRow) == g_added[k].donor_row) store(player + kPlayerRow, g_added[k].row);
}
PPC_FUNC(sub_039A6214_orig);
PPC_FUNC(sub_039A6214) { with_donor_row(ctx, base, load(0x10874864), sub_039A6214_orig); }
// The select strip's labels by select id (kProfiles), read with a bound (Paul's
// row past it) or none (sub_0294C400, id r4): the added fighter's row lent to
// Paul's, and Paul's id asked.
void with_profile(PPCContext& ctx, uint8_t* base, void (*f)(PPCContext&, uint8_t*)) {
    const int k = slot_of_id(ctx.r[4]);
    if (k < 0 || g_art[k].profile.empty()) {
        if (k >= 0) ctx.r[4] = 0;
        f(ctx, base);
        return;
    }
    std::vector<uint8_t> paul(guest_pointer(kProfiles), guest_pointer(kProfiles) + kProfileSize);
    std::memcpy(guest_pointer(kProfiles), g_art[k].profile.data(), kProfileSize);
    ctx.r[4] = 0;
    f(ctx, base);
    std::memcpy(guest_pointer(kProfiles), paul.data(), kProfileSize);
}
PPC_FUNC(sub_0294C400_orig);
PPC_FUNC(sub_0294C400) { with_profile(ctx, base, sub_0294C400_orig); }
PPC_FUNC(sub_0294A5E4_orig);
PPC_FUNC(sub_0294A5E4) { with_profile(ctx, base, sub_0294A5E4_orig); }
PPC_FUNC(sub_0294C0C8_orig);
PPC_FUNC(sub_0294C0C8) { with_profile(ctx, base, sub_0294C0C8_orig); }

// ------------------------------------------------------------------ voice

// A fighter's voice: the number of its entries of a category (id r3, category
// r4) and its k-th one (r5), from the run-time voice records (63 x 2608 bytes,
// +0 the entries) by select id: an added fighter's are the donor's record with
// the added fighter's entries lent to it.
constexpr uint32_t kVoiceRecords = 0x108EF378, kVoiceRecordSize = 2608;
// A record: +0 the entries, +4 + 260 * category {count, ...}, +2344 the
// entries' count (sub_0796F300 builds them at start-up).
constexpr uint32_t kVoiceRecordCounts = 4, kVoiceRecordCategory = 260, kVoiceRecordTotal = 2344;
uint32_t voice_count(const Voice& voice, uint32_t category) {
    if (category >= 10) return 0;
    const uint32_t mask = load(kVoiceMasks + 4 * category);
    uint32_t n = 0;
    for (uint32_t e = 0; e < voice.count; ++e) n += (load16(voice.entries + 16 * e + 12) & mask) != 0;
    return n;
}
PPC_FUNC(sub_0796F4C0_orig);
PPC_FUNC(sub_0796F4C0) {
    const int k = slot_of_id(ctx.r[3]);
    if (k >= 0 && g_voice[k].entries != 0) {
        ctx.r[3] = voice_count(g_voice[k], ctx.r[4]);
        return;
    }
    if (k >= 0) ctx.r[3] = g_added[k].donor;
    sub_0796F4C0_orig(ctx, base);
}
PPC_FUNC(sub_0796F218_orig);
PPC_FUNC(sub_0796F218) {
    const int k = slot_of_id(ctx.r[3]);
    if (k < 0 || g_voice[k].entries == 0) {
        if (k >= 0) ctx.r[3] = g_added[k].donor;
        sub_0796F218_orig(ctx, base);
        return;
    }
    // (category 6 is searched from its last entry: its count is read too)
    const uint32_t record = kVoiceRecords + kVoiceRecordSize * g_added[k].donor;
    const uint32_t fields[3] = {record, record + kVoiceRecordTotal,
                                record + kVoiceRecordCounts + kVoiceRecordCategory * 6};
    const uint32_t theirs[3] = {load(fields[0]), load(fields[1]), load(fields[2])};
    store(fields[0], g_voice[k].entries);
    store(fields[1], g_voice[k].count);
    store(fields[2], voice_count(g_voice[k], 6));
    ctx.r[3] = g_added[k].donor;
    sub_0796F218_orig(ctx, base);
    for (int i = 0; i < 3; ++i) store(fields[i], theirs[i]);
}
// Item sounds (kind r4 1: by the player's (r5) select id).
PPC_FUNC(sub_077537D0_orig);
PPC_FUNC(sub_077537D0) {
    const uint32_t player = ctx.r[5];
    const int k = ctx.r[4] == 1 && player ? slot_of_id(load(player + kPlayerSelectId)) : -1;
    if (k < 0) {
        sub_077537D0_orig(ctx, base);
        return;
    }
    const uint32_t row = kItemSounds + 8 * g_added[k].donor, theirs[2] = {load(row), load(row + 4)};
    store(row, g_voice[k].item_sounds);
    store(row + 4, g_voice[k].items);
    with_donor_id(ctx, base, player, sub_077537D0_orig);
    store(row, theirs[0]);
    store(row + 4, theirs[1]);
}
// The keys of a fighter's intro lines (id r3; sub_077CC348) and streamed win
// quote (sub_077CC6EC), by select id from tables of 63: the donor's rows with
// the added fighter's keys meanwhile.
void with_stream_keys(PPCContext& ctx, uint8_t* base, void (*f)(PPCContext&, uint8_t*)) {
    const int k = slot_of_id(ctx.r[3]);
    if (k < 0) {
        f(ctx, base);
        return;
    }
    const uint32_t donor = g_added[k].donor;
    const Voice& voice = g_voice[k];
    const uint32_t rows[3] = {kIntroBanks[0] + 4 * donor, kIntroBanks[1] + 4 * donor, kWinStreams + 4 * donor};
    const uint32_t theirs[3] = {load(rows[0]), load(rows[1]), load(rows[2])};
    if (voice.keys[2]) store(rows[0], voice.keys[2]), store(rows[1], voice.keys[2]);
    if (voice.keys[3]) store(rows[2], voice.keys[3]);
    ctx.r[3] = donor;
    f(ctx, base);
    for (int i = 0; i < 3; ++i) store(rows[i], theirs[i]);
}
PPC_FUNC(sub_077CC348_orig);
PPC_FUNC(sub_077CC348) { with_stream_keys(ctx, base, sub_077CC348_orig); }
PPC_FUNC(sub_077CC6EC_orig);
PPC_FUNC(sub_077CC6EC) { with_stream_keys(ctx, base, sub_077CC6EC_orig); }

// The bank ids of a match's added fighters: kFreeBanks[i] holds the bank of
// g_bound[i] (an added fighter's slot, -1: none yet). A fight load asks each
// member's banks (sub_079660FC, lists 3..7; a fighter twice in a match is
// asked once); an id is given to another fighter only when no list holds it
// and its bank object holds no bank (the title unloads a match's banks when
// it ends), so a fighter always loads its own bank on a newly bound id.
int g_bound[std::size(kFreeBanks)] = {-1, -1, -1, -1};
constexpr uint32_t kSoundLists = 0x107D449C; // 13 x {?, items, end, ...}: items 20 B {key, bank id, ...}
constexpr uint32_t kBankLoadAsk = 0x07966154, kBankLoadAskDwi = 0x07966164; // sub_079660FC's calls
constexpr uint32_t kBankPurgeAsk = 0x07968124;                            // sub_07967F58's
PPC_FUNC(sub_078D0088); // the bank object of a bank id
bool bank_in_use(PPCContext& ctx, uint8_t* base, uint32_t id) {
    const uint32_t lists = load(kSoundLists);
    for (uint32_t list = 3; lists && list <= 7; ++list) {
        for (uint32_t item = load(lists + 20 * list + 4); item && item < load(lists + 20 * list + 8); item += 20) {
            if (load(item + 4) == id) return true;
        }
    }
    const uint32_t bank = guest_call(ctx, base, sub_078D0088, {id});
    return bank != 0 && load(bank) != 0;
}
uint32_t bind_bank(PPCContext& ctx, uint8_t* base, size_t k) {
    int at = -1;
    for (size_t i = 0; i < std::size(kFreeBanks) && at < 0; ++i) {
        if (g_bound[i] == static_cast<int>(k)) at = static_cast<int>(i);
    }
    for (size_t i = 0; i < std::size(kFreeBanks) && at < 0; ++i) {
        if (g_bound[i] < 0) at = static_cast<int>(i);
    }
    for (size_t i = 0; i < std::size(kFreeBanks) && at < 0; ++i) {
        if (!bank_in_use(ctx, base, kFreeBanks[i])) at = static_cast<int>(i);
    }
    if (at < 0) {
        std::fprintf(stderr, "ttt2: warning: no sound bank id left for %s in this match\n", g_added[k].code.c_str());
        return 0xFFFFFFFF;
    }
    g_bound[at] = static_cast<int>(k);
    for (uint32_t sid : g_voice[k].copies) store(g_sound_table + 16 * sid + 4, kFreeBanks[at]);
    if (std::getenv("TTT2_TRACE_ROSTER")) {
        std::fprintf(stderr, "ttt2: roster: %s's sound bank as id %u (%zu sounds)\n", g_added[k].code.c_str(),
                     kFreeBanks[at], g_voice[k].copies.size());
    }
    return kFreeBanks[at];
}
uint32_t bound_bank(size_t k) {
    for (size_t i = 0; i < std::size(kFreeBanks); ++i) {
        if (g_bound[i] == static_cast<int>(k)) return kFreeBanks[i];
    }
    return 0xFFFFFFFF;
}

// A fighter's bank ids: shouts (se_Chr, sub_07A5CAC8(id)) and win quotes
// (sub_07A5CAA4(2, id): se_Dwi). An added fighter's one bank (asked once at
// a fight load), else the donor's.
bool g_purging = false; // sub_07967F58 releases the fighters' bank files
PPC_FUNC(sub_07A5CAC8_orig);
PPC_FUNC(sub_07A5CAC8) {
    if (ctx.lr == kBankPurgeAsk) g_purging = true;
    if (const int k = slot_of_id(ctx.r[3]); k >= 0) {
        if (g_voice[k].keys[0]) {
            ctx.r[3] = ctx.lr == kBankLoadAsk ? bind_bank(ctx, base, k) : bound_bank(k);
            return;
        }
        ctx.r[3] = g_added[k].donor;
    }
    sub_07A5CAC8_orig(ctx, base);
}
PPC_FUNC(sub_07A5CAA4_orig);
PPC_FUNC(sub_07A5CAA4) {
    if (const int k = slot_of_id(ctx.r[4]); k >= 0) {
        if (ctx.r[3] == 2 && g_voice[k].keys[0] && g_voice[k].chr_tones) {
            ctx.r[3] = ctx.lr == kBankLoadAskDwi ? 0xFFFFFFFF : bound_bank(k);
            return;
        }
        ctx.r[4] = g_added[k].donor;
    }
    sub_07A5CAA4_orig(ctx, base);
}
// Plays win quote r4 of the se_Dwi bank (r3 3) of the player r5: an added
// fighter's are after its shouts.
PPC_FUNC(sub_07753DA0_orig);
PPC_FUNC(sub_07753DA0) {
    const uint32_t player = ctx.r[5];
    if (const int k = player ? slot_of_id(load(player + kPlayerSelectId)) : -1; k >= 0 && g_voice[k].chr_tones) {
        ctx.r[4] += g_voice[k].chr_tones;
    }
    sub_07753DA0_orig(ctx, base);
}
// Between matches the title releases its fighters' cached bank files
// (sub_07967F58, by the ids 0..62): the added fighters' too.
PPC_FUNC(sub_08B06474); // release a cached file (cache r3, &key r4)
constexpr uint32_t kFileCache = 0x1084FEF4;
uint32_t g_key_scratch = 0;
PPC_FUNC(sub_07967F58_orig);
PPC_FUNC(sub_07967F58) {
    g_purging = false;
    sub_07967F58_orig(ctx, base);
    if (!g_purging || !g_active) return;
    g_purging = false;
    if (g_key_scratch == 0) g_key_scratch = system_alloc(4);
    const uint32_t r3 = ctx.r[3];
    for (size_t k = 0; k < g_added.size(); ++k) {
        if (!g_added[k].active || g_voice[k].keys[0] == 0) continue;
        store(g_key_scratch, g_voice[k].keys[0]);
        guest_call(ctx, base, sub_08B06474, {kFileCache, g_key_scratch});
    }
    ctx.r[3] = r3;
}
// The sound table (and its count): the extended one when an added fighter
// has banks of its own.
PPC_FUNC(sub_079E5B1C_orig);
PPC_FUNC(sub_079E5B1C) {
    if (g_sound_table == 0) {
        sub_079E5B1C_orig(ctx, base);
        return;
    }
    if (ctx.r[3] != 0) store(ctx.r[3], g_sound_count);
    ctx.r[3] = g_sound_table;
}

// A fighter's name calls (see kSelectBank): an added fighter's are the
// donor's moved to its own se_Chr bank, and the select screen's call its own
// tone in the changed se_Slt_StartChar, or none (not the donor's name).
PPC_FUNC(sub_079E5D64_orig);
PPC_FUNC(sub_079E5D64) {
    const int k = slot_of_id(ctx.r[3]);
    if (k < 0) {
        sub_079E5D64_orig(ctx, base);
        return;
    }
    Voice& voice = g_voice[k];
    const uint32_t kind = ctx.r[4];
    ctx.r[3] = g_added[k].donor;
    sub_079E5D64_orig(ctx, base);
    const uint32_t sid = ctx.r[3];
    if (sid == 0xFFFFFFFF) return;
    if (kind == 0) {
        const auto it = voice.moved.find(sid);
        if (it != voice.moved.end()) ctx.r[3] = it->second;
        return;
    }
    if (voice.name_sound == 0) {
        ctx.r[3] = 0xFFFFFFFF;
        return;
    }
    if (!voice.name_made && sid < kSoundCount) {
        // the donor's call's entry (bank, group, flags) with the added fighter's tone
        std::memcpy(guest_pointer(g_sound_table + 16 * voice.name_sound), guest_pointer(kSoundTable + 16 * sid), 16);
        store(g_sound_table + 16 * voice.name_sound, *voice.name_tone);
        voice.name_made = true;
    }
    ctx.r[3] = voice.name_made ? voice.name_sound : 0xFFFFFFFF;
}

// A bank id's key: a bound id's, its fighter's bank.
PPC_FUNC(sub_07A5D01C_orig);
PPC_FUNC(sub_07A5D01C) {
    for (size_t i = 0; i < std::size(kFreeBanks); ++i) {
        if (ctx.r[3] == kFreeBanks[i] && g_bound[i] >= 0) {
            ctx.r[3] = g_voice[g_bound[i]].keys[0];
            return;
        }
    }
    sub_07A5D01C_orig(ctx, base);
}

// Shows the select grid's portraits and the grilles over its empty ends;
// the grilles over the added fighters' cells are hidden again after every
// call.
PPC_FUNC(sub_06EF9C4C);
PPC_FUNC(sub_06F4D7B8_orig);
PPC_FUNC(sub_06F4D7B8) {
    const uint32_t ui = ctx.r[3];
    sub_06F4D7B8_orig(ctx, base);
    bool ends[2] = {false, false}; // added cells at the left end, the right end
    for (const Added& added : g_added) {
        if (added.active && added.cell && added.grid_row == 0) ends[added.column > 10 ? 1 : 0] = true;
    }
    for (int end = 0; end < 2; ++end) {
        if (!ends[end]) continue;
        ctx.r[3] = ui;
        ctx.r[4] = kSpacers[end];
        ctx.r[5] = 0;
        sub_06EF9C4C(ctx, base);
    }
}

// ------------------------------------------------------------------ art (see show_added)

// The select screen's cursors (sub_06F4C480, r19 the player, r30 its object)
// ask for both team members' UI positions first and set their pictures
// after, so Paul's rows cannot follow one question: each member's pictures
// show the entries of the added fighter its question was about (by the call
// sites of the questions and of the jumps to labels). A member's pictures
// change only when its UI position does (kept at +172, +188 for the first
// member, +180, +196 for the second): moving between Paul and an added
// fighter, all 0, resets them to 59 ("nobody").
struct SelectSite {
    uint32_t lr;
    uint32_t member;
};
constexpr SelectSite kSelectQueries[] = {
    {0x06F4CE0C, 0}, {0x06F4CE2C, 0}, {0x06F4CEC4, 0}, {0x06F4CF00, 0},
    {0x06F4CE4C, 1}, {0x06F4CE70, 1}, {0x06F4CED0, 1}, {0x06F4CF40, 1},
};
constexpr SelectSite kSelectLabels[] = {
    {0x06F4D068, 0}, {0x06F4D150, 0}, {0x06F4D290, 0}, // chara_face_yoko, p<n>_chara_waku (main)
    {0x06F4D0B4, 1}, {0x06F4D430, 1},                  // chara_face_yoko, p<n>_chara_waku (sub)
};
constexpr uint32_t kSelectPositions[2][2] = {{172, 188}, {180, 196}};
constexpr uint32_t kNobody = 59;
int g_select_slot[2][2] = {{-1, -1}, {-1, -1}}; // per player and member: the added fighter it shows, or -1

void note_select_question(uint32_t lr, const PPCContext& ctx, int slot) {
    for (const SelectSite& s : kSelectQueries) {
        if (s.lr != lr) continue;
        const uint32_t player = ctx.r[19] & 1;
        int& member = g_select_slot[player][s.member];
        if (member != slot) {
            for (uint32_t offset : kSelectPositions[s.member]) store(ctx.r[30] + offset + 4 * player, kNobody);
            member = slot;
        }
        return;
    }
}

// A fighter's UI position from its select id, for the 2D screens: an added
// fighter's is Paul's (its mob's record's), with its entries in Paul's rows.
PPC_FUNC(sub_06F0A484_orig);
PPC_FUNC(sub_06F0A484) {
    const int k = slot_of_id(ctx.r[3]);
    const int slot = k >= 0 && g_art[k].on ? k : -1;
    const uint32_t lr = ctx.lr;
    sub_06F0A484_orig(ctx, base);
    show_added(slot);
    if (any_art()) note_select_question(lr, ctx, slot);
}

// The loading and VS screens take a team whose members have the same UI
// position for one fighter alone (sub_06F248CC, and inline in sub_06F24960):
// two added fighters (both Paul's) would show as the first alone. At those
// questions, whose answers are only compared, an added fighter has a
// position of its own, beyond the tables.
constexpr uint32_t kTeamQuestions[] = {0x06F24904, 0x06F24920, 0x06F251D8, 0x06F251E8};
constexpr uint32_t kOwnPositions = 0x1000;

// A fighter's UI position from a record: Paul's, not 59 ("nobody"), for an
// added fighter's records, with its entries in Paul's rows.
PPC_FUNC(sub_06F0A4A4_orig);
PPC_FUNC(sub_06F0A4A4) {
    const uint32_t lr = ctx.lr;
    const int k = slot_of_record(ctx.r[3]);
    const int slot = k >= 0 && g_art[k].on ? k : -1;
    if (k >= 0 && std::find(std::begin(kTeamQuestions), std::end(kTeamQuestions), lr) != std::end(kTeamQuestions)) {
        ctx.r[3] = kOwnPositions + static_cast<uint32_t>(k);
        return;
    }
    if (slot >= 0) {
        ctx.r[3] = 0;
    } else {
        sub_06F0A4A4_orig(ctx, base);
    }
    show_added(slot);
    if (any_art()) note_select_question(lr, ctx, slot);
}

// The cursors hide a team's second member when both members have the same
// UI position (a fighter cannot be picked twice): two different added
// fighters both have Paul's. Before that check (the generated code calls this
// before 0x06F4D014, where the title tests r26: nonzero = check; r25 and r16
// the members' UI positions, r19 the player), the check is left out for them.
PPC_FUNC(cafe_mid_hook_06F4D014) {
    (void)base;
    const uint32_t player = ctx.r[19] & 1;
    const int first = g_select_slot[player][0], second = g_select_slot[player][1];
    if (ctx.r[25] == 0 && ctx.r[16] == 0 && first >= 0 && second >= 0 && first != second) ctx.r[26] = 0;
}

// Jumps to a label: on the select screen, with the entries of the member the
// label is for (see kSelectLabels).
PPC_FUNC(sub_06EFA17C_orig);
PPC_FUNC(sub_06EFA17C) {
    if (any_art()) {
        for (const SelectSite& s : kSelectLabels) {
            if (s.lr != ctx.lr) continue;
            const int shown = g_shown;
            show_added(g_select_slot[ctx.r[19] & 1][s.member]);
            sub_06EFA17C_orig(ctx, base);
            show_added(shown);
            return;
        }
    }
    sub_06EFA17C_orig(ctx, base);
}

// The functions that ask for UI positions leave Paul's rows as they found
// them: select screen (sub_06F4C480 the cursors, sub_06F4C07C the picked
// fighters), loading screen (sub_06F039E0 and sub_06F033B0 the portraits,
// sub_06F0B740 and sub_06F0C90C the names and faces), and the others.
#define ART_SCOPE(name)                                                                                          \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        const int shown = g_shown;                                                                               \
        name##_orig(ctx, base);                                                                                  \
        show_added(shown);                                                                                       \
    }
ART_SCOPE(sub_06F033B0)
ART_SCOPE(sub_06F039E0)
ART_SCOPE(sub_06F0B740)
ART_SCOPE(sub_06F0C90C)
ART_SCOPE(sub_06F23B10)
ART_SCOPE(sub_06F248CC)
ART_SCOPE(sub_06F24960)
ART_SCOPE(sub_06F25788)
ART_SCOPE(sub_06F33D8C)
ART_SCOPE(sub_06F3B734)
ART_SCOPE(sub_06F4391C)
ART_SCOPE(sub_06F4C07C)
ART_SCOPE(sub_06F4C480)
ART_SCOPE(sub_074AB214)
#undef ART_SCOPE

// The fights' HUD draws a fighter's name and faces from its texture
// container (7c476bde, resource 176) by numbers from a table by UI position
// ({name, face of player 1, of player 2}; Paul's 30, 105, 164), the UI
// position from the fighter's record (sub_033D555C: 59, "nobody", for an
// added fighter's records, which the HUD skips). The shared pictures add each
// added fighter's three after the container's 259, by slot
// (keys/7c476bde.add): while the HUD works on an added fighter, its records
// give Paul's row and Paul's numbers become its own. The HUD's code accepts
// numbers under 259 only, the container's own draw and lookup (sub_052EA7D8,
// sub_04C98550) any it has.
constexpr uint32_t kHudResource = 176, kHudTextures = 259;
constexpr uint32_t kHudPaul[3] = {30, 105, 164};
constexpr uint32_t kHudQuestions[] = {0x04A28E2C, 0x052F9C00, 0x052FA01C};
int g_hud_slot = -1; // the added fighter the HUD works on, or -1

uint32_t hud_number(uint32_t resource, uint32_t number) {
    if (g_hud_slot < 0 || resource != kHudResource) return number;
    for (uint32_t k = 0; k < 3; ++k) {
        if (number == kHudPaul[k]) return kHudTextures + 3 * static_cast<uint32_t>(g_hud_slot) + k;
    }
    return number;
}

// A fighter's UI position from its record.
PPC_FUNC(sub_033D555C_orig);
PPC_FUNC(sub_033D555C) {
    if (std::find(std::begin(kHudQuestions), std::end(kHudQuestions), ctx.lr) != std::end(kHudQuestions)) {
        const int k = slot_of_record(ctx.r[3]);
        g_hud_slot = k >= 0 && g_art[k].hud ? k : -1;
        if (g_hud_slot >= 0) {
            ctx.r[3] = 0;
            return;
        }
    }
    if (slot_of_record(ctx.r[3]) >= 0) {
        ctx.r[3] = kUiPosition; // "nobody": the 2D screens' code asks for an added fighter's pictures elsewhere
        return;
    }
    sub_033D555C_orig(ctx, base);
}

PPC_FUNC(sub_052EA7D8_orig);
PPC_FUNC(sub_052EA7D8) {
    ctx.r[5] = hud_number(ctx.r[4], ctx.r[5]);
    sub_052EA7D8_orig(ctx, base);
}

PPC_FUNC(sub_04C98550_orig);
PPC_FUNC(sub_04C98550) {
    ctx.r[4] = hud_number(ctx.r[3], ctx.r[4]);
    sub_04C98550_orig(ctx, base);
}

// The HUD's functions that ask (kHudQuestions) end the added fighter's turn.
#define HUD_SCOPE(name)                                                                                          \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        name##_orig(ctx, base);                                                                                  \
        g_hud_slot = -1;                                                                                         \
    }
HUD_SCOPE(sub_04A28840)
HUD_SCOPE(sub_052F9B34)
HUD_SCOPE(sub_052F9E34)
#undef HUD_SCOPE

// Lists the big select portraits' frames (see kPortraits); notes where the
// added fighters' are when the shared container has them.
PPC_FUNC(sub_08B07B24);
PPC_FUNC(sub_06F0449C_orig);
PPC_FUNC(sub_06F0449C) {
    const uint32_t object = ctx.r[3];
    sub_06F0449C_orig(ctx, base);
    g_portrait_owner = 0;
    if (!any_art()) return;
    // The container, found the way sub_06F0449C finds it: the entry of its key among the screen's resources.
    static const uint32_t key_slot = system_alloc(4);
    const PPCContext saved = ctx;
    store(key_slot, kPortraits);
    ctx.r[3] = load(object + kPortraitResources);
    ctx.r[4] = key_slot;
    sub_08B07B24(ctx, base);
    const uint32_t container = ctx.r[3] != 0 ? load(ctx.r[3]) : 0;
    ctx = saved;
    if (container == 0) return;
    for (uint32_t p = 0; p < 2; ++p) {
        const uint32_t first = load(object + kPortraitLists[p]);
        if (first == 0) return;
        g_paul_frames[p] = load(first);
    }
    for (size_t k = 0; k < g_added.size(); ++k) {
        const uint32_t frame = kPortraitFrames + 2 * static_cast<uint32_t>(k);
        const bool there = g_art[k].on && load(container) >= frame + 2;
        for (uint32_t p = 0; p < 2; ++p) {
            g_art[k].portrait_frames[p] = there ? container + load(container + 4 + 8 * (frame + p)) : 0;
        }
    }
    g_portrait_owner = object;
}

// Shows a big select portrait (object, UI position, slot): the added
// fighter's frames are in Paul's place of the lists meanwhile.
PPC_FUNC(sub_06F046D4_orig);
PPC_FUNC(sub_06F046D4) {
    const uint32_t object = ctx.r[3];
    const uint32_t* frames = g_shown >= 0 ? g_art[g_shown].portrait_frames : nullptr;
    if (!frames || !frames[0] || object != g_portrait_owner || ctx.r[4] != 0) {
        sub_06F046D4_orig(ctx, base);
        return;
    }
    uint32_t first[2];
    for (uint32_t p = 0; p < 2; ++p) {
        first[p] = load(object + kPortraitLists[p]);
        if (load(first[p]) == g_paul_frames[p]) store(first[p], frames[p]);
    }
    sub_06F046D4_orig(ctx, base);
    for (uint32_t p = 0; p < 2; ++p) {
        if (load(first[p]) == frames[p]) store(first[p], g_paul_frames[p]);
    }
}

// A fighter's list of costumes: an added fighter's (see kCostumeLists), in
// the place of Paul's row while the function reads it.
PPC_FUNC(sub_0580D690_orig);
PPC_FUNC(sub_0580D690) {
    const int k = slot_of_id(ctx.r[4]);
    if (k < 0 || g_costumes[k].entries == 0) {
        sub_0580D690_orig(ctx, base);
        return;
    }
    const uint32_t paul[2] = {load(kCostumeLists), load(kCostumeLists + 4)};
    store(kCostumeLists, g_costumes[k].entries);
    store(kCostumeLists + 4, 1);
    ctx.r[4] = 0;
    sub_0580D690_orig(ctx, base);
    store(kCostumeLists, paul[0]);
    store(kCostumeLists + 4, paul[1]);
}

// The chooser's functions that read kCostumePreviews: the added fighters'
// keys in their places while they run (they may call each other).
int g_costume_lent = 0;
std::vector<uint32_t> g_costume_places; // what the added fighters' places of kCostumePreviews held

void lend_costume_keys(bool on) {
    if (on && g_costume_lent++ != 0) return;
    if (!on && --g_costume_lent != 0) return;
    g_costume_places.resize(g_added.size());
    for (size_t k = 0; k < g_added.size(); ++k) {
        if (!g_added[k].active || g_costumes[k].key == 0) continue;
        const uint32_t place = kCostumePreviews + 4 * g_added[k].id;
        if (on) {
            g_costume_places[k] = load(place);
            store(place, g_costumes[k].key);
        } else {
            store(place, g_costume_places[k]);
        }
    }
}

#define COSTUME_SCOPE(name)                                                                                      \
    PPC_FUNC(name##_orig);                                                                                       \
    PPC_FUNC(name) {                                                                                             \
        lend_costume_keys(true);                                                                                 \
        name##_orig(ctx, base);                                                                                  \
        lend_costume_keys(false);                                                                                \
    }
COSTUME_SCOPE(sub_033E233C) // requests the pictures (CHANGE COSTUME pressed)
#undef COSTUME_SCOPE
// The same for a pick (cursor r4, select id r7), which first checks the id's
// bit in the cursor's mask: for an added fighter, whether its team has it.
PPC_FUNC(sub_033E265C_orig);
PPC_FUNC(sub_033E265C) {
    const uint32_t cursor = ctx.r[4];
    const int k = slot_of_id(ctx.r[7]);
    const bool lend = k >= 0 && k < 32 && cursor != 0 && g_lent < 0;
    if (lend) {
        g_lent = k;
        g_lent_bit = mask_bit(cursor, static_cast<uint32_t>(k));
        set_mask_bit(cursor, static_cast<uint32_t>(k), team_added(cursor).count(ctx.r[7]) == 0);
    }
    lend_costume_keys(true);
    sub_033E265C_orig(ctx, base);
    lend_costume_keys(false);
    if (lend) {
        set_mask_bit(cursor, static_cast<uint32_t>(k), g_lent_bit);
        g_lent = -1;
    }
}

// Notes the fighter a player's cursor is on for the chooser (object r3,
// player r4, select id r5), asserting that the id is below 63: an added
// fighter's donor's.
PPC_FUNC(sub_02669380_orig);
PPC_FUNC(sub_02669380) {
    if (const int k = slot_of_id(ctx.r[5]); k >= 0) ctx.r[5] = g_added[k].donor;
    sub_02669380_orig(ctx, base);
}

// Builds the chooser's entries (cursor r3, select id r4) once the pictures
// are there: for an added fighter without the customisation slots.
PPC_FUNC(sub_033E3460_orig);
PPC_FUNC(sub_033E3460) {
    const uint32_t cursor = ctx.r[3];
    const int k = slot_of_id(ctx.r[4]);
    const bool added = k >= 0 && g_costumes[k].key != 0;
    const bool pending = load8(cursor + kChooserPending) != 0;
    lend_costume_keys(true);
    sub_033E3460_orig(ctx, base);
    lend_costume_keys(false);
    if (!added || !pending || load8(cursor + kChooserPending) != 0) return; // not built this time
    const uint32_t count = load8(cursor + kChooserCount);
    uint32_t kept = 0;
    while (kept < count && load(cursor + kChooserEntries + kChooserEntrySize * kept) != kChooserSlot) ++kept;
    store8(cursor + kChooserCount, static_cast<uint8_t>(kept));
}

// ------------------------------------------------------------------ extra bones (see kPlayerAnimator)

PPC_FUNC(sub_06A5C5FC); // a bone record (container r3, number r4), 0 if the model has no such bone
PPC_FUNC(sub_04E1A4C8); // the player's skeleton (the hit test's)

// The animator's clock, for the fighters with posed extra bones: also moves
// the added fighter's part-3 animation on, a frame a call, and ends it after
// its last frame.
PPC_FUNC(sub_05BDC714_orig);
PPC_FUNC(sub_05BDC714) {
    const uint32_t animator = ctx.r[3];
    sub_05BDC714_orig(ctx, base);
    if (slot_of_id(load(load(animator) + kPlayerSelectId)) < 0) return;
    const uint32_t animation = part3_animation(animator);
    if (animation == 0) return;
    const float frame = loadf(animator + kAnimatorFrame) + 1.0f;
    if (frame < static_cast<float>(load(animation + 4))) {
        storef(animator + kAnimatorFrame, frame);
    } else {
        store(animator + kAnimatorValue, 0);
        storef(animator + kAnimatorFrame, 0.0f);
    }
}

// Weapons (the importer's weapons.py, character.ini weapons): the added
// fighter's extra bone OPT 800 + k takes the extra pair k (translation,
// rotation) of its body animation's channels (after the 23 body channels).
// Every frame the title samples a fighter's current body animation with all
// its channels (sub_069DF470 -> sub_069DCBD0 -> the sampler sub_0689C42C, 16
// bytes a channel: (x, y, z, 1), rotations as the game's quaternion) and
// copies the extra ones into the model (+13984; +13216 1 when there are any,
// +13220 how many, 64 at most), which only some fighters' code reads. (The
// sampling when a move starts, sub_06897540, is once a move: a pair taken from
// it stays at its first frame, and a thrown weapon never leaves the hand.)
// The pair is the bone's local transform, as TTT2
// writes Yoshimitsu's katanas' (bones 1200, 1210) from his: the translation
// as it is, the record's rotation the quaternion's matrix. Without its pair,
// the bone goes back to its bind (the local the record had before any write
// here); OPT 820 on are never posed.
// The weapon's points (extra bones OPT 850 + n under it) hit as TTT2's hit
// locations 70 + s (s the point's slot), the points of a held item, which the
// hit test (sub_03C74F50) reads from the player (+3416 + 12 s; items write
// them, nothing does for the added fighter). The title keeps no world matrices
// for still extra bones (spring flag 8: their records' stay their bind's), so
// the points' positions are made here from the parent's world matrix (the hit
// test's skeleton, sub_04E1A4C8(player) +32: 144-byte records, the body bones
// by bone index, +64 world, rows the axes, row 3 the position) and the locals.
// Those records are a frame behind the animation (as the hit test's own
// bones), so the points take the local drawn the frame before: with this
// frame's, a thrown sword's points (1 to 4 m from a swinging hand) flew
// over the opponent's head while the sword drawn went through him.
// Summoned weapons (Noctis's sword, daggers, spear, shield): a move summons
// weapon n (Tekken 7's 0x82a7, the importer's 0x822E) and dismisses it or all
// (0x82a8 -> 0x822F, value 16: all); a weapon not summoned is collapsed into
// its bone's origin (its axes 0), its points not written. Weapons of one hand
// share slots: a slot takes the summoned weapon's point, else the first
// weapon's with that slot. A thrown weapon that hits stays where it hit
// (Noctis's Warp-Strike: Tekken 7 dismisses the sword as it hits, with
// particles of its mesh that linger there, which the port does not draw, and
// summons it again for the warp): the move a hit leads to starts with 0x822B,
// which holds each summoned weapon farther than kThrown from its parent where
// it was drawn last in the world, drawn though dismissed, until the animation
// has it summoned nearer the hand than that (the warp that takes it back), a
// dismissal of all, or kHoldFrames.
namespace {
constexpr uint32_t kFirstWeaponBone = 800, kStillWeapons = 20, kFirstWeaponPoint = 850, kWeaponPoints = 4;
constexpr uint32_t kModelExtraSampled = 13216, kModelExtraCount = 13220, kModelExtra = 13984, kModelExtraMax = 64;
constexpr uint32_t kPlayerItemPoints = 3416, kSkeletonRecords = 32, kRecordSize = 144, kRecordWorld = 64;
constexpr uint32_t kSummonProp = 0x822E, kDismissProp = 0x822F, kDismissAll = 16, kHoldProp = 0x822B;
constexpr float kThrown = 400.0f; // mm from the parent bone: a weapon thrown, not held
constexpr int kHoldFrames = 120;   // a held weapon let go after these frames whatever comes
constexpr float kHeldReach = 1500.0f; // mm: a weapon always in hand (no summon) ignores a pair farther out (the
                                      // channels of another animation's use: Tekken 7's spin reactions, 20 m paths)
struct WeaponBone {
    std::array<uint32_t, 16> bind{}, last{}, drawn{};
    bool shown = false; // drawn last frame (with `drawn`, not collapsed)
};
std::map<uint32_t, WeaponBone> g_weapon_bones; // record -> its bind, the local written last, the one drawn
std::map<uint32_t, uint64_t> g_summoned; // player -> its summoned weapons (bit n: summon n)
// Row-vector 4x4 matrices (rows the axes, row 3 the position), as the title's records keep them.
using Matrix = std::array<float, 16>;
struct WeaponHold {
    struct Held {
        Matrix world; // where it is held
        int frames = 0;
    };
    bool requested = false;          // 0x822B ran: hold the thrown weapons at the next posing
    std::map<size_t, Held> held;     // by weapon index
    std::map<size_t, Matrix> last;   // weapon index -> the local it was drawn with last frame
    std::map<size_t, Matrix> parent; // weapon index -> its parent's record at the last posing
};
std::map<uint32_t, WeaponHold> g_weapon_holds; // by player

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) v += a[4 * r + k] * b[4 * k + c];
            out[4 * r + c] = v;
        }
    }
    return out;
}
Matrix rigid_inverse(const Matrix& m) { // axes orthonormal (scale 1)
    Matrix out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[4 * r + c] = m[4 * c + r];
    }
    for (int c = 0; c < 3; ++c) {
        out[12 + c] = -(m[12] * out[c] + m[13] * out[4 + c] + m[14] * out[8 + c]);
    }
    out[15] = 1.0f;
    return out;
}

uint32_t bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
float value(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void summon_property(uint32_t player, uint32_t id, uint32_t n) {
    if (id == kHoldProp) {
        g_weapon_holds[player].requested = true;
        return;
    }
    uint64_t& summoned = g_summoned[player];
    if (id == kSummonProp && n < 64) {
        summoned |= uint64_t{1} << n;
    } else if (id == kDismissProp && n == kDismissAll) {
        summoned = 0;
        g_weapon_holds[player].held.clear();
    } else if (id == kDismissProp && n < 64) {
        summoned &= ~(uint64_t{1} << n);
    }
}

bool summoned(const Weapon& weapon, uint32_t player) {
    if (weapon.summon < 0) return true;
    const auto it = g_summoned.find(player);
    return weapon.summon < 64 && it != g_summoned.end() && (it->second >> weapon.summon & 1) != 0;
}

// A weapon's local transform held where it was thrown to (see above): parent the parent bone's world matrix (the
// hit test's record, +64), which is a frame behind the animation: it places the local drawn last frame, and the
// parent this frame is extrapolated from it and the one before. -> whether it is drawn (summoned or held).
bool hold_weapon(uint32_t player, size_t w, bool summoned, uint32_t parent, std::array<uint32_t, 16>& local) {
    WeaponHold& hold = g_weapon_holds[player];
    if (!summoned && !hold.held.count(w)) {
        hold.last.erase(w);
        hold.parent.erase(w);
        return false;
    }
    Matrix p, l;
    for (int i = 0; i < 16; ++i) {
        p[i] = loadf(parent + 4 * i);
        l[i] = value(local[i]);
    }
    p[3] = p[7] = p[11] = 0.0f;
    p[15] = 1.0f;
    const auto reach = [](const Matrix& m) { return std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]); };
    const auto last = hold.last.find(w);
    if (hold.requested && last != hold.last.end() && !hold.held.count(w) && reach(last->second) > kThrown) {
        hold.held[w] = {multiply(last->second, p), 0}; // where it was drawn last frame
    }
    if (const auto held = hold.held.find(w); held != hold.held.end()) {
        Matrix now = p; // the parent this frame: moved as it did since the frame before
        if (const auto before = hold.parent.find(w); before != hold.parent.end()) {
            now = multiply(p, multiply(rigid_inverse(before->second), p));
        }
        const Matrix there = multiply(held->second.world, rigid_inverse(now)); // the held place in the parent's frame
        if ((summoned && reach(l) < reach(there)) || ++held->second.frames > kHoldFrames) { // the warp takes it back
            hold.held.erase(held);
            if (!summoned) {
                hold.last.erase(w);
                hold.parent.erase(w);
                return false;
            }
        } else {
            for (int i = 0; i < 16; ++i) local[i] = bits(there[i]);
            l = there;
        }
    }
    hold.last[w] = l;
    hold.parent[w] = p;
    return true;
}

void pose_weapons(PPCContext& ctx, uint8_t* base, uint32_t model, uint32_t player, const std::vector<Weapon>& weapons) {
    if (weapons.empty()) return;
    std::vector<uint32_t> extra; // the current frame's extra channels, 4 words each
    if (load(model + kModelExtraSampled) != 0) {
        const uint32_t channels = std::min(load(model + kModelExtraCount), kModelExtraMax);
        for (uint32_t i = 0; i < 4 * channels; ++i) extra.push_back(load(model + kModelExtra + 4 * i));
    }
    const uint32_t skeleton = guest_call(ctx, base, sub_04E1A4C8, {player});
    const uint32_t records = skeleton ? load(skeleton + kSkeletonRecords) : 0;
    std::vector<std::array<uint32_t, 16>> locals(weapons.size()); // the points' (as drawn last frame, not collapsed)
    std::vector<bool> shown(weapons.size(), false);
    for (size_t w = 0; w < weapons.size(); ++w) {
        const Weapon& weapon = weapons[w];
        ctx.r[3] = load(model + kModelBones);
        ctx.r[4] = kFirstWeaponBone + weapon.pair;
        sub_06A5C5FC(ctx, base);
        const uint32_t record = ctx.r[3];
        if (record == 0) continue;
        std::array<uint32_t, 16> now;
        for (uint32_t i = 0; i < 16; ++i) now[i] = load(record + 4 * i);
        auto [it, fresh] = g_weapon_bones.try_emplace(record);
        WeaponBone& bone = it->second;
        if (fresh || (now != bone.last && now != bone.bind)) bone = WeaponBone{now}; // a new model's record
        std::array<uint32_t, 16> local = bone.bind;
        const uint32_t* t =
            weapon.pair < kStillWeapons && 8 * weapon.pair + 8 <= extra.size() ? &extra[8 * weapon.pair] : nullptr;
        if (t != nullptr && weapon.summon < 0 && std::hypot(value(t[0]), value(t[1]), value(t[2])) > kHeldReach) {
            t = nullptr;
        }
        if (t != nullptr) {
            const float x = value(t[4]), y = value(t[5]), z = value(t[6]), w = value(t[7]);
            const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                                   {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                                   {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) local[4 * row + col] = bits(m[row][col]);
                local[4 * row + 3] = bits(0.0f);
                local[12 + row] = t[row];
            }
            local[15] = bits(1.0f);
        }
        shown[w] = summoned(weapon, player);
        if (records != 0) {
            shown[w] = hold_weapon(player, w, shown[w], records + kRecordSize * weapon.parent + kRecordWorld, local);
        }
        locals[w] = bone.shown ? bone.drawn : local; // the parent's record is a frame behind the animation
        bone.drawn = local;
        bone.shown = shown[w];
        if (!shown[w]) {
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) local[4 * row + col] = bits(0.0f);
            }
        }
        if (local != now) {
            for (uint32_t i = 0; i < 16; ++i) store(record + 4 * i, local[i]);
        }
        bone.last = local;
    }
    if (const auto hold = g_weapon_holds.find(player); hold != g_weapon_holds.end()) hold->second.requested = false;
    if (records == 0) return;
    // the points: position = p L R + t R + T (rows: L the local axes, R the parent's world axes; p the point's
    // local translation); the summoned weapons' first, then a slot none of them wrote from the first weapon with it
    uint32_t written = 0;
    auto write_points = [&](size_t w, bool only_unwritten) {
        const Weapon& weapon = weapons[w];
        const std::array<uint32_t, 16>& local = locals[w];
        const uint32_t parent = records + kRecordSize * weapon.parent + kRecordWorld;
        float axes[3][3], origin[3];
        for (int c = 0; c < 3; ++c) {
            origin[c] = loadf(parent + 48 + 4 * c);
            for (int row = 0; row < 3; ++row) {
                axes[row][c] = 0.0f;
                for (int i = 0; i < 3; ++i) axes[row][c] += value(local[4 * row + i]) * loadf(parent + 16 * i + 4 * c);
                origin[c] += value(local[12 + row]) * loadf(parent + 16 * row + 4 * c);
            }
        }
        for (const WeaponPoint& point : weapon.points) {
            if (point.slot >= kWeaponPoints || (only_unwritten && (written >> point.slot & 1))) continue;
            ctx.r[3] = load(model + kModelBones);
            ctx.r[4] = kFirstWeaponPoint + point.number;
            sub_06A5C5FC(ctx, base);
            if (ctx.r[3] == 0) continue;
            const float at[3] = {loadf(ctx.r[3] + 48), loadf(ctx.r[3] + 52), loadf(ctx.r[3] + 56)};
            for (int c = 0; c < 3; ++c) {
                storef(player + kPlayerItemPoints + 12 * point.slot + 4 * c,
                       origin[c] + at[0] * axes[0][c] + at[1] * axes[1][c] + at[2] * axes[2][c]);
            }
            if (!only_unwritten) written |= 1u << point.slot;
        }
    };
    for (size_t w = 0; w < weapons.size(); ++w) {
        if (shown[w]) write_points(w, false);
    }
    for (size_t w = 0; w < weapons.size(); ++w) {
        if (!shown[w]) {
            write_points(w, true);
            for (const WeaponPoint& point : weapons[w].points) {
                if (point.slot < kWeaponPoints) written |= 1u << point.slot;
            }
        }
    }
}

} // namespace


// A fighter's model, every frame (the per-fighter posing of extra bones among
// the rest): then the added fighter's extra bones take the frame of its
// part-3 animation, and the last frame of one that has just ended.
PPC_FUNC(sub_069D8B30_orig);
PPC_FUNC(sub_069D8B30) {
    const uint32_t model = ctx.r[3];
    sub_069D8B30_orig(ctx, base);
    if (!g_active) return;
    const uint32_t r3 = ctx.r[3], lr = ctx.lr; // the caller checks LR after the calls below
    ctx.r[3] = model;
    cafe_ppc_lookup(load(load(model + kModelInterface) + 68))(ctx, base);
    const uint32_t player = ctx.r[3];
    if (slot_of_id(load(player + kPlayerSelectId)) >= 0) {
        const uint32_t animator = load(player + kPlayerAnimator);
        uint32_t animation = part3_animation(animator);
        float frame = loadf(animator + kAnimatorFrame);
        if (animation != 0) {
            g_extra_shown[player] = animation;
        } else if (const auto shown = g_extra_shown.find(player); shown != g_extra_shown.end()) {
            animation = shown->second;
            frame = 1e9f;
            g_extra_shown.erase(shown);
        }
        if (animation != 0) {
            const uint32_t channels = load(animation) & 0xFFFF, frames = load(animation + 4);
            const uint32_t f = std::min(frames - 1, static_cast<uint32_t>(std::max(0.0f, frame)));
            const uint32_t values = animation + 8 + 4 * channels + 12 * channels * f;
            for (uint32_t c = 0; c + 3 <= channels; c += 3) {
                ctx.r[3] = load(model + kModelBones);
                ctx.r[4] = kFirstExtraBone + c / 3;
                sub_06A5C5FC(ctx, base);
                if (ctx.r[3] == 0) continue;
                float t[3], r[3], sc[3];
                for (int i = 0; i < 3; ++i) {
                    t[i] = loadf(values + 12 * c + 4 * i);
                    r[i] = loadf(values + 12 * (c + 1) + 4 * i);
                    sc[i] = loadf(values + 12 * (c + 2) + 4 * i);
                }
                store_local(ctx.r[3], t, r, sc);
            }
        }
        pose_weapons(ctx, base, model, player, g_added[slot_of_id(load(player + kPlayerSelectId))].weapons);
    }
    ctx.r[3] = r3;
    ctx.lr = lr;
}


// ------------------------------------------------------------------ 2D (Street Fighter style) arcs

// Tekken 7 moves some fighters (Akuma, Eliza, Geese, Noctis) through the air
// by physics: move property 0x8226 (value bytes A, B, C, 0) starts an arc of
// take-off speed A mm/frame and gravity B/10 mm/frame^2 (C: the horizontal
// speed in percent), an air move cancelled out of it (0x8227) continues it,
// and such moves hold their last frame until the arc lands. The importer
// bakes the arcs of moves that start on the ground into their animations
// (TTT2's own jumps carry their arcs so; the player stays at y = 0) and marks
// them with 0x822D at frame 1 (value A, B, C, the arc's first frame); it keeps
// 0x8226 on air moves and turns 0x8227 into 0x822C at 0x8001. TTT2's property
// interpreter does nothing with these ids. Here: an air move (0x822C) cancelled
// out of a baked arc continues it with the player's own height and vertical
// speed (player +4, +360), which TTT2's physics (sub_04336854: vy -= 6 dt and
// y += vy dt while y > 0, both to 0 at the floor) moves on, with the arc's
// gravity instead of TTT2's; the move holds its ground_fall frame while
// airborne (player +424, read by the frame advance sub_0414FA10) and goes to
// it at the touchdown (as TTT2's own landing does, sub_04336854 @0x43371ac),
// so its landing cancel runs. Chained air moves (0x822C again) keep the arc;
// 0x8226 in an air move restarts it from where the player is (dive kicks).
namespace {
constexpr uint32_t kPlayerY = 4, kPlayerFrame = 212, kPlayerFrame2 = 240, kPlayerFrameF = 248,
                   kPlayerFrameF2 = 252, kPlayerFrameFixed = 256, kPlayerMove = 280, kPlayerVy = 360,
                   kPlayerHold = 424, kPlayerHover = 901, kPlayerGravityMode = 132, kPlayerSlow1 = 1712,
                   kPlayerSlow2 = 1720;
constexpr uint32_t kMoveGroundFall = 0x48;
constexpr uint32_t kSlowMotionScale = 0x1030CD80; // f32 (sub_04E1A410)
constexpr float kTitleGravity = 6.0f;             // 0x102520E4
constexpr uint32_t kArcProp = 0x8226, kArcBakedProp = 0x822D, kArcAirMoveProp = 0x822C;
constexpr uint32_t kHeld = 2;                     // player +424 while an air move waits for the touchdown

struct Arc2D {
    enum Mode { kNone, kBaked, kPhysics } mode = kNone;
    float a = 0, g = 0;
    uint32_t move = 0;      // kBaked: the baked move; kPhysics: the air move
    int32_t f0 = 0, n = 0;  // kBaked: the arc's first frame, the frame of the last step - f0
    float y = 0, vy = 0;    // kPhysics: after the last step
};
std::map<uint32_t, Arc2D> g_arcs; // by player

float slow_motion(uint32_t player) { // sub_04E1A410
    const float k = loadf(kSlowMotionScale);
    return static_cast<float>(static_cast<int32_t>(load(player + kPlayerSlow1))) * k *
           (static_cast<float>(static_cast<int32_t>(load(player + kPlayerSlow2))) * k);
}

void set_frame(uint32_t player, int32_t frame) { // as sub_04336854 @0x43371ac
    store(player + kPlayerFrame, static_cast<uint32_t>(frame));
    store(player + kPlayerFrame2, static_cast<uint32_t>(frame));
    store(player + kPlayerFrameFixed, static_cast<uint32_t>(frame) << 12);
    storef(player + kPlayerFrameF, static_cast<float>(frame));
    storef(player + kPlayerFrameF2, static_cast<float>(frame));
}

float arc_height(const Arc2D& s, int32_t n) {
    const float f = static_cast<float>(n);
    return f * s.a - s.g * f * (f + 1.0f) / 2.0f;
}

void arc_property(uint32_t player, uint32_t id, uint32_t value) {
    Arc2D& s = g_arcs[player];
    const uint32_t move = load(player + kPlayerMove);
    const float a = static_cast<float>(value >> 24), g = static_cast<float>((value >> 16) & 0xFF) / 10.0f;
    if (id == kArcBakedProp) {
        s = Arc2D{};
        s.mode = Arc2D::kBaked;
        s.a = a;
        s.g = g;
        s.move = move;
        s.f0 = static_cast<int32_t>(value & 0xFF);
        s.n = static_cast<int32_t>(load(player + kPlayerFrame)) - s.f0;
    } else if (id == kArcAirMoveProp) {
        if (s.move == move) return; // the property runs again
        if (s.mode == Arc2D::kBaked) {
            const int32_t n = std::max(0, s.n);
            storef(player + kPlayerY, std::max(1.0f, arc_height(s, n)));
            storef(player + kPlayerVy, s.a - s.g * static_cast<float>(n));
        } else if (s.mode == Arc2D::kPhysics) {
            storef(player + kPlayerY, s.y);
            storef(player + kPlayerVy, s.vy);
        } else {
            return;
        }
        store8(player + kPlayerHover, 0);
        s.mode = Arc2D::kPhysics;
        s.move = move;
    } else { // kArcProp: an arc from where the player is
        if (s.mode == Arc2D::kPhysics && s.move != move) { // a new move: undo the move change's height
            storef(player + kPlayerY, s.y);
            storef(player + kPlayerVy, s.vy);
        }
        if (s.mode != Arc2D::kPhysics) s = Arc2D{};
        s.mode = Arc2D::kPhysics;
        s.move = move;
        s.g = g;
        storef(player + kPlayerY, std::max(1.0f, loadf(player + kPlayerY)));
        storef(player + kPlayerVy, a);
        store8(player + kPlayerHover, 0);
    }
}
} // namespace

PPC_FUNC(sub_03AE86A0_orig);
PPC_FUNC(sub_03AE86A0) { // a move property (player r3, {id, value} r4, pass r5)
    const uint32_t player = ctx.r[3], id = load(ctx.r[4]);
    if ((id == kArcProp || id == kArcBakedProp || id == kArcAirMoveProp) && ctx.r[5] == 0 &&
        slot_of_id(load(player + kPlayerSelectId)) >= 0)
        arc_property(player, id, load(ctx.r[4] + 4));
    if ((id == kSummonProp || id == kDismissProp || id == kHoldProp) && ctx.r[5] == 0 &&
        slot_of_id(load(player + kPlayerSelectId)) >= 0)
        summon_property(player, id, load(ctx.r[4] + 4));
    sub_03AE86A0_orig(ctx, base);
}

PPC_FUNC(sub_04336854_orig);
PPC_FUNC(sub_04336854) { // a fighter's physics, once a frame (r3; it recurses into the objects at +164, +168)
    const uint32_t player = ctx.r[3];
    const bool own = ctx.lr < 0x04336854 || ctx.lr >= 0x0433AF5C;
    const auto it = own ? g_arcs.find(player) : g_arcs.end();
    if (it == g_arcs.end() || it->second.mode == Arc2D::kNone) {
        sub_04336854_orig(ctx, base);
        return;
    }
    Arc2D& s = it->second;
    if (s.mode == Arc2D::kPhysics) {
        const uint32_t mode = load(player + kPlayerGravityMode);
        if ((mode == 0 || mode == 2) && loadf(player + kPlayerY) > 0.0f && load8(player + kPlayerHover) == 0)
            storef(player + kPlayerVy, loadf(player + kPlayerVy) - (s.g - kTitleGravity) * slow_motion(player));
    }
    sub_04336854_orig(ctx, base);
    const uint32_t move = load(player + kPlayerMove);
    if (s.mode == Arc2D::kBaked) {
        if (move == s.move) {
            s.n = static_cast<int32_t>(load(player + kPlayerFrame)) - s.f0;
        } else {
            s.mode = Arc2D::kNone; // into a move that does not continue it (the landing, a hit)
        }
    } else if (move != s.move) { // got hit, or a move that does not continue the arc
        if (load(player + kPlayerHold) == kHeld) store(player + kPlayerHold, 0xFFFFFFFF);
        s.mode = Arc2D::kNone;
    } else if (const float y = loadf(player + kPlayerY); y > 0.0f) {
        store(player + kPlayerHold, kHeld);
        s.y = y;
        s.vy = loadf(player + kPlayerVy);
    } else { // the touchdown
        store(player + kPlayerHold, 0xFFFFFFFF);
        const int32_t ground_fall = static_cast<int32_t>(load(move + kMoveGroundFall));
        if (ground_fall > 0 && static_cast<int32_t>(load(player + kPlayerFrame)) < ground_fall)
            set_frame(player, ground_fall);
        s.mode = Arc2D::kNone;
    }
}

// ------------------------------------------------------------------ effect room

// The effect library's pools of loaded resources are sized once, at start-up
// (sub_06D2BB54 sets the configuration, sub_08C0F368 makes them): 200 effect
// files (sub_08BF4D30: r4 at +24), 200 models (sub_08BF4D38: r5 at +56) and
// 500 textures (sub_08BF4D44: r5 at +40), for the common effects, the stage's,
// the mode's and the four fighters of a match (a solo fighter's partner slot
// loads it again). Registering past a pool's end fails silently, and the stage
// loads last: an added fighter's effects (converted from Tekken 7's, 30-50
// files) left a stage without its floor. Each pool is made three times its
// size while added fighters are installed (a few kB of nodes).
constexpr uint32_t kEffectRoom = 3;

PPC_FUNC(sub_08BF4D30_orig);
PPC_FUNC(sub_08BF4D30) {
    if (g_active) ctx.r[4] *= kEffectRoom;
    sub_08BF4D30_orig(ctx, base);
}

PPC_FUNC(sub_08BF4D38_orig);
PPC_FUNC(sub_08BF4D38) {
    if (g_active) ctx.r[5] *= kEffectRoom;
    sub_08BF4D38_orig(ctx, base);
}

PPC_FUNC(sub_08BF4D44_orig);
PPC_FUNC(sub_08BF4D44) {
    if (g_active) ctx.r[5] *= kEffectRoom;
    sub_08BF4D44_orig(ctx, base);
}

// ------------------------------------------------------------------ level effects

// A fighter's effect sits on a body part (move property 0x801f) in that bone's
// own frame: every frame its placement (sub_06BE756C) hands the bone's place
// to the library's sub_08BE5688(effect, x, y, z) (+944) and its world matrix
// (sub_06BE6CFC: bone record + 64) to sub_08BE56E4(effect, matrix) (+960,
// the axes count). Tekken 7 places its effects at the bone in the
// fighter's level facing frame (x ahead, z up), and converted ones are made
// for that (a hand's frame turns a hadouken's flash to the floor). The
// converted effects carry kLevelMark in their file's root (+700, zero in the
// title's files and kept there once loaded): for them the setter gets the
// bone's position with the model root's rotation made level. The root's
// rotation is the hips' world rotation without their own (bone record 0:
// local rows at +0, world rows at +64; rows are the axes, P = L^T W).
//
// A projectile's look (convert/particles.py projectile and thrown: also
// kProjectileMark) rides the projectile itself: it takes the place of the
// player's projectile spawned last when it starts (sub_041CE354(slot, row,
// player) initialises one of the player's slots: + 7968, 96 bytes each,
// position at +0, +32 -1 when free, row +60), follows it every frame and goes
// out of sight (far below the stage) when the projectile is gone (a hit, the
// end of its life), so that it ends where the projectile does.
//
// Tekken 7 places some emitters from the floor's height rather than the bone's
// (ground smoke, a low wave): the converter puts them in a second file of the
// effect, which kFloorMark marks; its height is the floor's under the fighter.
constexpr uint32_t kLevelMark = 0x54374658, kLevelMarkAt = 700;       // "T7FX"
constexpr uint32_t kProjectileMark = 0x50524F4A, kProjectileMarkAt = 704; // "PROJ"
constexpr uint32_t kFloorMark = 0x464C4F52, kFloorMarkAt = 708;           // "FLOR"
constexpr uint32_t kPlayerFloor = 72;                                      // player: f32 the floor's height
constexpr uint32_t kEffectFile = 932, kEffectPlayer = 720;      // effect object: its loaded file, its slot
constexpr uint32_t kRigBones = 32, kBoneLocal = 0, kBoneWorld = 64; // sub_04E1A518's model: its bone records
constexpr uint32_t kProjectileFree = 32, kProjectileRow = 60;
constexpr float kOutOfSight = -100000.0f;
struct ProjectileLook {
    uint32_t file, owner, slot, row; // owner: the effect's player slot (+720) the projectile was looked up for
    float before[3]; // the slot's place when the look started (its last projectile's until the first update)
    bool moved;
};
std::map<uint32_t, uint32_t> g_last_projectile;    // player -> the slot initialised last
std::map<uint32_t, ProjectileLook> g_projectile_looks; // effect object -> the projectile it rides

// sub_08BE5650(effect, file, ...): the library sets an effect object up for a new effect (objects are reused).
PPC_FUNC(sub_08BE5650_orig);
PPC_FUNC(sub_08BE5650) {
    if (g_active) g_projectile_looks.erase(ctx.r[3]);
    sub_08BE5650_orig(ctx, base);
}

PPC_FUNC(sub_041CE354_orig);
PPC_FUNC(sub_041CE354) {
    if (g_active) g_last_projectile[ctx.r[5]] = ctx.r[3];
    sub_041CE354_orig(ctx, base);
}

// A projectile look's place: where its projectile is, or out of sight once it is gone. -> false for other effects.
bool projectile_look_place(PPCContext& ctx, uint8_t* base, uint32_t effect, uint32_t file, float place[3]) {
    if (load(file + kProjectileMarkAt) != kProjectileMark) return false;
    // The effect's player slot is set after its first placement (still 0 then: player 1's first member), so a
    // look is looked up again when it changes (a tag partner's).
    const uint32_t owner = load(effect + kEffectPlayer);
    auto look = g_projectile_looks.find(effect);
    if (look == g_projectile_looks.end() || look->second.file != file || look->second.owner != owner) {
        const uint32_t player = guest_call(ctx, base, sub_05C5EF74, {owner});
        const auto last = g_last_projectile.find(player); // a new look: the player's newest projectile
        const uint32_t slot = last == g_last_projectile.end() ? 0 : last->second;
        ProjectileLook started{file, owner, slot, slot ? load(slot + kProjectileRow) : 0, {}, false};
        for (int i = 0; i < 3 && slot; ++i) started.before[i] = loadf(slot + 4 * i);
        look = g_projectile_looks.insert_or_assign(effect, started).first;
    }
    ProjectileLook& l = look->second;
    const uint32_t slot = l.slot;
    if (slot && load(slot + kProjectileFree) != 0xFFFFFFFF && load(slot + kProjectileRow) == l.row) {
        float at[3];
        for (int i = 0; i < 3; ++i) at[i] = loadf(slot + 4 * i);
        l.moved = l.moved || at[0] != l.before[0] || at[1] != l.before[1] || at[2] != l.before[2];
        if (l.moved) std::copy(at, at + 3, place); // (until then the body part's place)
    } else {
        l.slot = 0; // gone: never again, even when the slot is used anew
        place[1] = kOutOfSight;
    }
    return true;
}

PPC_FUNC(sub_04E1A518); // a player's model

// The hips' bone record of the fighter an effect belongs to (its slot's player), or 0.
uint32_t effect_hips(PPCContext& ctx, uint8_t* base, uint32_t effect) {
    const uint32_t player = guest_call(ctx, base, sub_05C5EF74, {load(effect + kEffectPlayer)});
    const uint32_t model = player ? guest_call(ctx, base, sub_04E1A518, {player}) : 0;
    return model ? load(model + kRigBones) : 0;
}

// A floor part's place (kFloorMark): its height that of the floor under the fighter (the player's +72: 7000 mm
// on a raised stage, its toes 40 mm above). -> false for other effects.
bool floor_place(PPCContext& ctx, uint8_t* base, uint32_t effect, uint32_t file, float place[3]) {
    if (load(file + kFloorMarkAt) != kFloorMark) return false;
    if (place[1] == kOutOfSight) return true;
    if (const uint32_t player = guest_call(ctx, base, sub_05C5EF74, {load(effect + kEffectPlayer)})) {
        place[1] = loadf(player + kPlayerFloor);
    }
    return true;
}

uint32_t level_file(uint32_t effect) {
    const uint32_t file = effect ? load(effect + kEffectFile) : 0;
    return file && load(file + kLevelMarkAt) == kLevelMark ? file : 0;
}

// sub_08BE5688(effect, x, y, z): the effect's place (+944), set before its matrix every frame.
PPC_FUNC(sub_08BE5688_orig);
PPC_FUNC(sub_08BE5688) {
    if (g_active) {
        if (const uint32_t file = level_file(ctx.r[3])) {
            float place[3] = {float(ctx.f[1].ps0), float(ctx.f[2].ps0), float(ctx.f[3].ps0)};
            const bool rides = projectile_look_place(ctx, base, ctx.r[3], file, place);
            if (floor_place(ctx, base, ctx.r[3], file, place) || rides) {
                for (int i = 0; i < 3; ++i) ctx.f[1 + i].ps0 = place[i];
            }
        }
    }
    sub_08BE5688_orig(ctx, base);
}

PPC_FUNC(sub_08BE56E4_orig);
PPC_FUNC(sub_08BE56E4) {
    if (g_active) {
        const uint32_t effect = ctx.r[3], matrix = ctx.r[4];
        if (const uint32_t file = level_file(effect)) {
            float place[3] = {loadf(matrix + 48), loadf(matrix + 52), loadf(matrix + 56)};
            projectile_look_place(ctx, base, effect, file, place);
            floor_place(ctx, base, effect, file, place);
            static const uint32_t level = system_alloc(64);
            for (int i = 0; i < 12; ++i) store(level + 4 * i, load(matrix + 4 * i)); // (no model: the bone's axes)
            if (const uint32_t hips = effect_hips(ctx, base, effect)) {
                float ahead[3] = {0, 0, 0}; // the root's z row: sum over k of L[k][2] W[k]
                for (int k = 0; k < 3; ++k) {
                    const float l = loadf(hips + kBoneLocal + 16 * k + 8);
                    for (int j = 0; j < 3; ++j) ahead[j] += l * loadf(hips + kBoneWorld + 16 * k + 4 * j);
                }
                const float n = std::sqrt(ahead[0] * ahead[0] + ahead[2] * ahead[2]);
                if (n > 1e-3f) {
                    const float x = ahead[0] / n, z = ahead[2] / n;
                    const float rows[12] = {z, 0, -x, 0, 0, 1, 0, 0, x, 0, z, 0}; // left = up x ahead, up, ahead
                    for (int i = 0; i < 12; ++i) storef(level + 4 * i, rows[i]);
                }
            }
            for (int i = 0; i < 3; ++i) storef(level + 48 + 4 * i, place[i]);
            storef(level + 60, 1.0f);
            ctx.r[4] = level;
        }
    }
    sub_08BE56E4_orig(ctx, base);
}

// ------------------------------------------------------------------ skinning room

// The title skins the fighters on screen into one of four stream-out buffers
// a frame and, in a match's first frame, all four fighters, which sets up the
// per-vertex state its skinning shader carries from frame to frame (a tag
// partner left out there is never reset: from its tag-in on it reads other
// fighters' vertices as its state and draws black, then blotched). That first
// frame stops at a buffer's size, made for the title's fighters (1-2.6 MB of
// skinned vertices each); added fighters take up to 3.3 MB (Noctis with his
// weapons 5.5 MB: two of him and two others need 17.7 MB), so the buffers'
// total in the configuration sub_08977AE8 is given (r4: u8 +16 the count, u32
// +8 the total, 28 MiB) is tripled while it allocates them (from the title's
// 384 MiB graphics pool).
constexpr uint32_t kSkinningRoom = 3;
// The resource manager's pool (sub_08A7ED8C: r3 the manager, 0x1084FEF0; r5
// its size, 20 MiB, allocated there) holds the effect files a match copies in
// ("alo copy": the common ones, the stage's and each fighter's; Geese's and
// Noctis's together took 7.5 MB), textures while they load and much else: two
// fighters with many effects filled it and the match did not load (an
// allocation of the effect copies failed, sub_08A7A460 @0x08A7C5E4). With added
// fighters it is twice the size.
constexpr uint32_t kResourcePool = 0x1400000, kResourcePoolRoom = 2;
PPC_FUNC(sub_08A7ED8C_orig);
PPC_FUNC(sub_08A7ED8C) {
    if (g_active && ctx.r[5] == kResourcePool) ctx.r[5] = kResourcePoolRoom * kResourcePool;
    sub_08A7ED8C_orig(ctx, base);
}

PPC_FUNC(sub_08977AE8_orig);
PPC_FUNC(sub_08977AE8) {
    const uint32_t config = ctx.r[4];
    const uint32_t total = config ? load(config + 8) : 0;
    if (!g_active || total == 0) {
        sub_08977AE8_orig(ctx, base);
        return;
    }
    store(config + 8, kSkinningRoom * total);
    sub_08977AE8_orig(ctx, base);
    store(config + 8, total);
}
