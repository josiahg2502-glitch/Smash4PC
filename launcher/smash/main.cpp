// Smash 4 PC's launcher: a full-window menu over a scene from the game
// (Play, Settings, Mods, Quit) driven by a pad, the keyboard or the mouse.
// It finds the game and its update, writes display.ini (what the game reads
// when it starts) and runs smash.exe beside it.
//
// Files beside the executable: smash.exe, title-logo.png, launcher-background
// .jpg/.png (optional), launcher.ini (game_folder = the folder holding the
// game and update folders), display.ini, cache\, mods\, fonts\launcher.ttf
// (optional, else Bahnschrift / Segoe UI / DejaVu Sans).

#include "host/cpu_level.h"
#include "host/display_settings.h"
#include "host/rooms.h"
#include "os/act_server.h"
#include "os/online_identity.h"
#include "host/paint.h" // before the stb_truetype implementation below

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace display = cafe::host::display;
namespace online = cafe::os::online;
namespace act_server = cafe::os::act_server;
using namespace cafe::host::paint;

namespace {

constexpr const char* kVersion = "0.1.1";
constexpr const char* kUpdateId = "0005000E10144F00"; // v304
constexpr const char* kGameId = "0005000010144F00";   // USA


// ------------------------------------------------------------------ files

fs::path exe_dir() { return fs::path(SDL_GetBasePath()); }

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// launcher.ini: `name = value` lines.
std::map<std::string, std::string> read_ini(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::istringstream in(read_text(path).value_or(""));
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

constexpr const char* kDlcId = "0005000C10144F00";

// A title folder's code/app.xml: its title id and version (both hex there).
struct TitleInfo {
    std::string id;
    int version = 0;
};
std::optional<TitleInfo> title_of(const fs::path& folder) {
    const auto xml = read_text(folder / "code" / "app.xml");
    if (!xml) return std::nullopt;
    const auto tag = [&](const char* name) -> std::string {
        const size_t open = xml->find(std::string("<") + name);
        const size_t start = open == std::string::npos ? open : xml->find('>', open);
        const size_t end = start == std::string::npos ? start : xml->find('<', start);
        return end == std::string::npos ? "" : xml->substr(start + 1, end - start - 1);
    };
    TitleInfo t{tag("title_id"), static_cast<int>(std::strtol(tag("title_version").c_str(), nullptr, 16))};
    std::transform(t.id.begin(), t.id.end(), t.id.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return t;
}

// The three folders the launcher needs (DLC optional), as chosen.
enum Slot { kGame, kUpdate, kDlc, kSlots };
constexpr const char* kSlotIds[kSlots] = {kGameId, kUpdateId, kDlcId};
constexpr const char* kSlotKeys[kSlots] = {"game", "update", "dlc"};

// Whether `folder` is that title.
bool is_title(const fs::path& folder, Slot slot) {
    const auto t = title_of(folder);
    std::error_code ec;
    return t && t->id == kSlotIds[slot] && (slot != kUpdate || fs::exists(folder / "code" / "cross_f.rpx", ec));
}

// The titles in `folder`, the folders in it and the folders beside it, for
// the slots not chosen yet.
void find_titles(const fs::path& folder, fs::path (&slots)[kSlots]) {
    std::error_code ec;
    std::vector<fs::path> candidates{folder};
    for (const fs::path& dir : {folder, folder.parent_path()}) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_directory(ec)) candidates.push_back(entry.path());
        }
    }
    for (const fs::path& c : candidates) {
        for (int s = 0; s < kSlots; ++s) {
            if (slots[s].empty() && is_title(c, Slot(s))) slots[s] = c;
        }
    }
}

// DLC folders hold content/<number>/ folders; a dump without the content
// has only placeholder files there.
// The DLC items: content\<number>\ folders with a file in them. Smash's fighters and stages come with the
// update; each DLC item's folder only marks it owned (dummy_content.txt, "content data").
int dlc_items(const fs::path& folder) {
    std::error_code ec;
    int items = 0;
    for (const auto& item : fs::directory_iterator(folder / "content", ec)) {
        if (!item.is_directory(ec)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(item.path(), ec)) {
            if (entry.is_regular_file(ec)) {
                ++items;
                break;
            }
        }
    }
    return items;
}

// A mod: a folder in mods\ with the game's content\ layout somewhere inside, as the common formats
// put it: content\ itself (Sm4shExplorer, big packs), a folder or two down (archives),
// sdcafiine\<title id>\<mod>\content\ (SDCafiine), or beside a Cemu graphic pack's rules.txt.
struct Mod {
    std::string name;
    fs::path root; // the folder with content\ in it
    bool on = true;
};

// The first folder holding content\, searching `depth` levels down.
fs::path find_content_root(const fs::path& folder, int depth) {
    std::error_code ec;
    if (fs::is_directory(folder / "content", ec)) return folder;
    if (depth == 0) return {};
    for (const auto& sub : fs::directory_iterator(folder, ec)) {
        if (!sub.is_directory(ec) || sub.path().filename() == "content") continue;
        if (fs::path root = find_content_root(sub.path(), depth - 1); !root.empty()) return root;
    }
    return {};
}

// A Cemu graphic pack's name from its rules.txt (`name = ...` in [Definition]).
std::string pack_name(const fs::path& root) {
    std::istringstream in(read_text(root / "rules.txt").value_or(""));
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("name", 0) != 0 || line.find('=') == std::string::npos) continue;
        std::string name = line.substr(line.find('=') + 1);
        name.erase(0, name.find_first_not_of(" \t\""));
        name.erase(name.find_last_not_of(" \t\r\"") + 1);
        return name;
    }
    return {};
}

// Unpacks `archive` into `into` with the system's tar (libarchive: .zip, .7z, .rar, .tar.*); false if it couldn't.
bool extract_archive(const fs::path& archive, const fs::path& into) {
#ifdef _WIN32
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring command = L"\"" + (fs::path(system) / "tar.exe").wstring() + L"\" -xf \"" + archive.wstring() + L"\" -C \"" +
                           into.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
#else
    const char* args[] = {"tar", "-xf", archive.c_str(), "-C", into.c_str(), nullptr};
    SDL_Process* process = SDL_CreateProcess(args, false);
    int code = 1;
    if (process) {
        SDL_WaitProcess(process, true, &code);
        SDL_DestroyProcess(process);
    }
    return code == 0;
#endif
}

// What installing a mod (an archive or a folder) did: the mod's name, or why it failed.
struct Installed {
    bool ok = false;
    std::string message;
};

// Installs the mod in `source` (an archive or a folder) as mods\<its name>\. A mod is anything with the game's
// content\ layout somewhere inside (find_content_root), the formats find_mods reads.
Installed install_mod(const fs::path& source, const fs::path& mods) {
    std::error_code ec;
    fs::create_directories(mods, ec);
    std::string name = fs::is_directory(source, ec) ? source.filename().string() : source.stem().string();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".tar") name.resize(name.size() - 4);
    for (char& c : name) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) c = '_';
    }
    if (name.empty() || name[0] == '.') name = "mod" + name;
    // Unpacked (or copied) beside the mods first, then moved into place in one step.
    const fs::path staging = mods / (".installing-" + name);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const auto fail = [&](std::string why) {
        fs::remove_all(staging, ec);
        return Installed{false, std::move(why)};
    };
    if (fs::is_directory(source, ec)) {
        fs::copy(source, staging / source.filename(), fs::copy_options::recursive, ec);
        if (ec) return fail("Couldn't copy that folder: " + ec.message());
    } else if (!extract_archive(source, staging)) {
        return fail("Couldn't unpack " + source.filename().string() + ". Unpack it yourself and drop the folder here.");
    }
    if (find_content_root(staging, 5).empty()) {
        return fail(source.filename().string() + " isn't a Smash mod: there's no content folder in it.");
    }
    // The archive's single top folder, if that's all it holds, becomes the mod's folder.
    fs::path top = staging;
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(staging, ec)) entries.push_back(e.path());
    if (entries.size() == 1 && fs::is_directory(entries[0], ec) && entries[0].filename() != "content") top = entries[0];
    fs::path target = mods / name;
    for (int n = 2; fs::exists(target, ec); ++n) target = mods / (name + " (" + std::to_string(n) + ")");
    fs::rename(top, target, ec);
    if (ec) return fail("Couldn't move the mod into the mods folder: " + ec.message());
    fs::remove_all(staging, ec);
    return Installed{true, target.filename().string()};
}

std::vector<Mod> find_mods(const fs::path& mods) {
    std::vector<Mod> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(mods, ec)) {
        if (!entry.is_directory(ec) || entry.path().filename().string().rfind(".installing-", 0) == 0) continue;
        const fs::path root = find_content_root(entry.path(), 4);
        if (root.empty()) continue;
        // The name: a graphic pack's own, SDCafiine's mod folder, else the folder in mods\.
        std::string name = pack_name(root);
        if (name.empty() && root.parent_path().parent_path().filename() == "sdcafiine") name = root.filename().string();
        found.push_back({name.empty() ? entry.path().filename().string() : name, root});
    }
    std::sort(found.begin(), found.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return found;
}

// ------------------------------------------------------------------ drawing


Image load_image(SDL_Renderer* renderer, const fs::path& path) {
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!pixels) return {};
    SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, nullptr, pixels, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(pixels);
    return {t, float(w), float(h)};
}


// ------------------------------------------------------------------ the launcher

enum class Screen { kMain, kSettings, kMods, kOnline, kNetplay };


struct Item {
    std::string label;
    std::string value; // settings: the current choice, shown with chevrons
    int badge = 0;
};

class Launcher {
public:
    Launcher(SDL_Window* window, SDL_Renderer* renderer) : window_(window), r_(renderer), paint_(renderer) {
        dir_ = exe_dir();
        display::set_file(dir_ / "display.ini", "Smash 4 PC");
        settings_ = display::load(display::file_path()).settings;
        const auto ini = read_ini(dir_ / "launcher.ini");
        for (int s = 0; s < kSlots; ++s) {
            if (auto it = ini.find(kSlotKeys[s]); it != ini.end()) paths_[s] = it->second;
        }
        if (auto it = ini.find("game_folder"); it != ini.end()) find_titles(it->second, paths_); // older file
        if (auto it = ini.find("netplay_host"); it != ini.end()) netplay_host_ = it->second == "yes";
        if (auto it = ini.find("netplay_name"); it != ini.end()) netplay_name_ = it->second;
        online::load(dir_ / "online");
        refresh();
        font_.open({dir_ / "fonts" / "launcher.ttf", "C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/segoeui.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"});
        logo_ = load_image(r_, dir_ / "title-logo.png");
        background_ = load_image(r_, dir_ / "launcher-background.jpg");
        if (!background_.texture) background_ = load_image(r_, dir_ / "launcher-background.png");
    }

    bool running() const { return !quit_; }
    bool game_running() const { return process_ != nullptr; }
    bool installing() const { return installing_; }

    void event(SDL_Event& e) {
        SDL_ConvertEventToRenderCoordinates(r_, &e);
        if (editing_ >= 0 && text_event(e)) return;
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            std::fprintf(stderr, "launcher: quit (event 0x%x)\n", e.type);
            quit_ = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
            case SDLK_UP: case SDLK_W: move(-1); break;
            case SDLK_DOWN: case SDLK_S: move(1); break;
            case SDLK_LEFT: case SDLK_A: change(-1); break;
            case SDLK_RIGHT: case SDLK_D: change(1); break;
            case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: if (!e.key.repeat) activate(); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: if (!e.key.repeat) back(); break;
            case SDLK_F11: SDL_SetWindowFullscreen(window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN)); break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: change(-1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: change(1); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: activate(); break;
            case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: back(); break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) stick_y_ = e.gaxis.value / 32767.0f;
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) stick_x_ = e.gaxis.value / 32767.0f;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            // (Not the motion a new window reports where the pointer already is.)
            if (time_ > 0.3f && (e.motion.xrel != 0 || e.motion.yrel != 0)) mouse_moved_ = true;
            if (int i = item_at(e.motion.x, e.motion.y); i >= 0 && mouse_moved_) select(i);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button != SDL_BUTTON_LEFT) break;
            if (int i = item_at(e.button.x, e.button.y); i >= 0) {
                select(i);
                if (!items()[i].value.empty() && !folder_slot(i) && screen_ != Screen::kOnline) {
                    change(e.button.x < layout_.right_chevron - layout_.h * 0.06f ? -1 : 1);
                } else {
                    activate();
                }
            } else if (screen_ != Screen::kMain && e.button.x < layout_.w * 0.5f) {
                // (a click beside the menu does nothing; Back is an item)
            }
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) {
                go(Screen::kMods);
                start_install(e.drop.data);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == 4 /* X1: back */) back();
            break;
        default: break;
        }
    }

    void update(float dt) {
        time_ += dt;
        // The stick moves the selection, repeating while held.
        const auto repeat = [&](float value, float& held, auto&& action) {
            if (std::fabs(value) < 0.5f) {
                held = 0;
                return;
            }
            if (held == 0 || held > 0.4f) {
                action(value > 0 ? 1 : -1);
                if (held > 0.4f) held = 0.28f;
            }
            held += dt;
        };
        repeat(stick_y_, held_y_, [&](int d) { move(d); });
        repeat(stick_x_, held_x_, [&](int d) { change(d); });
        selection_y_ = ease(selection_y_, float(selected_), dt, 18);
        fade_ = std::min(1.0f, fade_ + dt * 5.5f);
        if (process_) {
            int code = 0;
            if (SDL_WaitProcess(process_, false, &code)) {
                SDL_DestroyProcess(process_);
                process_ = nullptr;
                exit_code_ = code;
                SDL_RestoreWindow(window_);
                SDL_RaiseWindow(window_);
                refresh();
            }
        }
        std::string chosen;
        bool done = false;
        {
            std::lock_guard lock(dialog_mutex_);
            chosen.swap(dialog_result_);
            std::swap(done, dialog_done_);
        }
        finish_sign_in();
        finish_install();
        if (!done) return;
        SDL_SetWindowTitle(window_, "Smash 4 PC");
        if (mod_dialog_) {
            mod_dialog_ = false;
            if (!chosen.empty()) start_install(chosen);
            return;
        }
        if (online_dialog_) {
            online_dialog_ = false;
            if (!chosen.empty()) import_console_files(chosen);
            return;
        }
        if (!chosen.empty()) {
            // The chosen folder fills its slot; titles found beside it fill empty ones.
            paths_[dialog_slot_] = chosen;
            find_titles(chosen, paths_);
            save_launcher_ini();
            refresh();
        }
        // Play's questions go on until nothing is missing; cancelling the game stops them, the
        // update and DLC are skipped.
        if (prompting_) {
            const auto next = missing();
            if (next && (!chosen.empty() || dialog_slot_ != kGame)) {
                choose_folder(*next);
            } else {
                prompting_ = false;
                if (ready()) play();
            }
        }
    }

    // A mod given to the launcher (dropped on its icon, or on the command line).
    void install(const std::string& path) {
        go(Screen::kMods);
        start_install(path);
    }

    void open_screen(const std::string& name) {
        if (name == "settings") go(Screen::kSettings);
        if (name == "online") go(Screen::kOnline);
        if (name == "netplay") go(Screen::kNetplay);
        if (name == "mods") go(Screen::kMods);
        if (name == "play") play();
    }

    void draw(bool save = false) {
        int w = 0, h = 0;
        SDL_GetCurrentRenderOutputSize(r_, &w, &h);
        layout_ = Layout{float(w), float(h), items().size()};
        const float W = layout_.w, H = layout_.h;
        SDL_SetRenderDrawBlendMode(r_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r_, 6, 8, 14, 255);
        SDL_RenderClear(r_);
        if (screen_ == Screen::kOnline) {
            // The Online page looks like the Wii U's own Nintendo Network ID Settings.
            draw_wiiu_online();
            if (save) {
                if (SDL_Surface* shot = SDL_RenderReadPixels(r_, nullptr)) {
                    SDL_SaveBMP(shot, std::getenv("SMASH_LAUNCHER_SHOT"));
                    SDL_DestroySurface(shot);
                }
            }
            SDL_RenderPresent(r_);
            return;
        }
        draw_background();

        // The logo, top left.
        if (logo_.texture) {
            const float lh = H * 0.30f, lw = lh * logo_.width / logo_.height;
            paint_.image(logo_, {W * 0.055f, H * 0.075f, lw, lh});
        } else {
            font_.draw(r_, W * 0.08f, H * 0.25f, "SMASH 4 PC", H * 0.09f, kWhite);
        }

        // The menu: the selected item between chevrons.
        const float alpha = fade_, slide = (1 - fade_) * H * 0.03f;
        const auto& list = items();
        const float px = H * layout_.px;
        const float sel_y = layout_.item_y(selection_y_) - px * 0.36f;
        const float pulse = 0.85f + 0.15f * std::sin(time_ * 4.0f);
        const Color chev{1, 1, 1, alpha * pulse};
        paint_.chevron(layout_.left_chevron - slide, sel_y, H * 0.034f, -1, H * 0.0042f, chev);
        paint_.chevron(layout_.right_chevron + slide, sel_y, H * 0.034f, 1, H * 0.0042f, chev);
        for (size_t i = 0; i < list.size(); ++i) {
            const bool on = static_cast<int>(i) == selected_;
            const Color c{1, 1, 1, alpha * (on ? 1.0f : 0.72f)};
            const float y = layout_.item_y(float(i));
            const float x = layout_.text_x + slide;
            const float tw = font_.draw(r_, x, y, list[i].label, px, c);
            if (list[i].badge > 0) {
                const float br = px * 0.3f, bx = x + tw + px * 0.42f, by = y - px * 0.36f;
                paint_.round_rect(bx - br, by - br, br * 2, br * 2, br, {kBadge.r, kBadge.g, kBadge.b, alpha});
                const std::string n = std::to_string(list[i].badge);
                font_.draw(r_, bx - font_.width(r_, n, px * 0.45f) / 2, by + px * 0.16f, n, px * 0.45f,
                           {0.12f, 0.08f, 0.02f, alpha}, false);
            }
            if (!list[i].value.empty()) {
                const float vpx = px * 0.74f;
                const std::string v = fit(list[i].value, vpx, W * 0.36f - (x - layout_.text_x) - tw);
                const float vx = layout_.right_chevron - H * 0.03f - font_.width(r_, v, vpx);
                const std::string& raw = list[i].value;
                Color vc = raw.rfind("Found", 0) == 0 || raw == "On" || (raw[0] == 'v' && raw.size() < 6) ? kGood
                           : raw == "Choose..."                                                ? kGold
                           : raw == "Wrong folder"                                             ? kBad
                                                                                               : kDim;
                vc.a = c.a;
                font_.draw(r_, vx, y - px * 0.04f, v, vpx, vc);
            }
        }

        draw_status();
        draw_toast();
        const std::string version = std::string("Version ") + kVersion;
        const float vpx = H * 0.032f;
        font_.draw(r_, W * 0.94f - font_.width(r_, version, vpx), H * 0.905f, version, vpx, kWhite);
        if (save) {
            if (SDL_Surface* s = SDL_RenderReadPixels(r_, nullptr)) {
                SDL_SaveBMP(s, std::getenv("SMASH_LAUNCHER_SHOT"));
                SDL_DestroySurface(s);
            }
        }
        SDL_RenderPresent(r_);
    }

private:
    // The main menu: Play, the game / update / DLC folders, Settings, Mods, Quit.
    enum MainItem { kPlay, kGameItem, kUpdateItem, kDlcItem, kOnlineItem, kNetplayItem, kSettingsItem, kModsItem, kQuitItem };
    // The Netplay page (host/netplay.h: the host's game streamed to its guests): hosting, a name, joining.
    // Then the open rooms (or a line saying there are none), then Back.
    enum NetplayItem { kHostSwitch, kNameField, kFirstRoom };
    // The Online page: the console's files, the Pretendo ID and password, signing in and out.
    enum OnlineItem { kConsoleFiles, kUserField, kPasswordField, kSignIn, kSignOut, kOnlineBack };
    static constexpr int kSettingsBack = 3;
    static std::optional<Slot> folder_slot_of(Screen screen, int item) {
        if (screen != Screen::kMain || item < kGameItem || item > kDlcItem) return std::nullopt;
        return Slot(item - kGameItem);
    }
    std::optional<Slot> folder_slot(int item) const { return folder_slot_of(screen_, item); }

    struct Layout {
        float w = 1600, h = 900;
        float text_x = 0, left_chevron = 0, right_chevron = 0;
        float top = 0.505f, step = 0.083f, px = 0.048f; // fractions of the height
        Layout() = default;
        Layout(float w_, float h_, size_t items) : w(w_), h(h_) {
            text_x = std::max(w * 0.085f, h * 0.15f);
            left_chevron = text_x - h * 0.05f;
            right_chevron = text_x + std::min(w * 0.40f, h * 0.78f);
            if (items > 4) { // the main menu: smaller, closer, above the status
                top = 0.445f;
                step = 0.33f / float(items - 1);
                px = 0.040f;
            }
        }
        float item_y(float i) const { return h * (top + i * step); }
    } layout_;

    // A folder's short state for the menu (the path shows below it when selected).
    std::string folder_value(Slot s) const {
        if (paths_[s].empty()) return s == kDlc ? "None" : "Choose...";
        if (!valid_[s]) return "Wrong folder";
        if (s == kUpdate) return "v" + std::to_string(update_version_);
        if (s == kDlc) return dlc_items_ > 0 ? "Found (" + std::to_string(dlc_items_) + " items)" : "No items";
        return "Found";
    }

    const std::vector<Item>& items() {
        list_.clear();
        switch (screen_) {
        case Screen::kMain:
            list_ = {{"PLAY"},
                     {"GAME", folder_value(kGame)},
                     {"UPDATE", folder_value(kUpdate)},
                     {"DLC", folder_value(kDlc)},
                     {"ONLINE", online_value()},
                     {"NETPLAY", netplay_host_ ? "Hosting" : ""},
                     {"SETTINGS"},
                     {"MODS", "", static_cast<int>(std::count_if(mods_.begin(), mods_.end(), [](const Mod& m) { return m.on; }))},
                     {"QUIT"}};
            break;
        case Screen::kSettings: {
            const char* res = "Automatic";
            switch (settings_.resolution) {
            case display::Resolution::k720: res = "1280 x 720"; break;
            case display::Resolution::k1080: res = "1920 x 1080"; break;
            case display::Resolution::k1440: res = "2560 x 1440"; break;
            case display::Resolution::k2160: res = "3840 x 2160 (4K)"; break;
            case display::Resolution::k2880: res = "5120 x 2880 (5K)"; break;
            default: break;
            }
            const std::string fps = !settings_.interpolation                       ? "60"
                                    : settings_.frame_rate == 0                     ? "Automatic"
                                    : settings_.frame_rate == display::kUncapped   ? "Uncapped"
                                                                                   : std::to_string(settings_.frame_rate);
            list_ = {{"RESOLUTION", res},
                     {"DISPLAY", settings_.fullscreen ? "Fullscreen" : "Window"},
                     {"FRAME RATE", fps},
                     {"BACK"}};
            break;
        }
        case Screen::kOnline: {
            const auto field = [&](int item, const std::string& text, bool secret) {
                std::string shown = secret ? std::string(utf8_length(text), '*') : text;
                if (editing_ == item) return shown + (std::fmod(time_, 1.0f) < 0.5f ? "|" : " ");
                return shown.empty() ? std::string("Type...") : shown;
            };
            list_ = {{"CONSOLE FILES", online::device().present ? "Found" : "Choose..."},
                     {"PRETENDO ID", field(kUserField, user_, false)},
                     {"PASSWORD", field(kPasswordField, password_, true)},
                     {"SIGN IN", signing_in_ ? "Signing in..." : ""},
                     {"SIGN OUT", online::account().present ? online::account().account_id : ""},
                     {"BACK"}};
            break;
        }
        case Screen::kNetplay: {
            const auto field = [&](int item, const std::string& text) {
                if (editing_ == item) return text + (std::fmod(time_, 1.0f) < 0.5f ? "|" : " ");
                return text.empty() ? std::string("Type...") : text;
            };
            list_ = {{"HOST MY GAMES", netplay_host_ ? "On" : "Off"},
                     {"MY NAME", field(kNameField, netplay_name_)}};
            std::lock_guard lock(rooms_mutex_);
            for (const auto& room : rooms_) {
                std::string label = room.name + "'S ROOM";
                for (char& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                list_.push_back({label, room.players >= room.max
                                                            ? "Full"
                                                            : std::to_string(room.players) + "/" + std::to_string(room.max)});
            }
            if (rooms_.empty()) list_.push_back({rooms_reached_ ? "NO OPEN ROOMS" : "LOOKING FOR ROOMS..."});
            list_.push_back({"BACK"});
            break;
        }
        case Screen::kMods:
            // Each mod with its switch, then the folder and Back.
            for (const Mod& m : mods_) list_.push_back({m.name, m.on ? "On" : "Off"});
            list_.push_back({"INSTALL MOD...", installing_ ? "Installing..." : ""});
            list_.push_back({"OPEN MODS FOLDER"});
            list_.push_back({"BACK"});
            break;
        }
        return list_;
    }

    void select(int i) {
        if (i == selected_) return;
        selected_ = i;
    }
    void move(int d) {
        const int n = static_cast<int>(items().size());
        selected_ = (selected_ + d + n) % n;
    }
    void go(Screen s) {
        if (s == Screen::kNetplay) watch_rooms();
        screen_ = s;
        selected_ = 0;
        selection_y_ = 0;
        fade_ = 0;
    }
    void back() {
        if (editing_ >= 0) stop_editing();
        if (screen_ != Screen::kMain) go(Screen::kMain);
    }

    // Left / right on a setting.
    void change(int d) {
        // Left on the update or DLC: play without it.
        if (const auto slot = folder_slot(selected_); slot && *slot != kGame && d < 0 && !paths_[*slot].empty()) {
            paths_[*slot].clear();
            asked_[*slot] = true;
            save_launcher_ini();
            refresh();
            return;
        }
        if (screen_ == Screen::kNetplay && selected_ == kHostSwitch) {
            netplay_host_ = !netplay_host_;
            save_launcher_ini();
            return;
        }
        if (screen_ == Screen::kMods && selected_ < static_cast<int>(mods_.size())) { // a mod's switch
            mods_[selected_].on = !mods_[selected_].on;
            save_mods();
            return;
        }
        if (screen_ != Screen::kSettings) return;
        if (selected_ == 0) {
            static constexpr display::Resolution order[] = {display::Resolution::kAuto, display::Resolution::k1080,
                                                            display::Resolution::k1440, display::Resolution::k2160,
                                                            display::Resolution::k2880, display::Resolution::k720};
            int i = 0;
            while (order[i] != settings_.resolution && i < 5) ++i;
            settings_.resolution = order[(i + d + 6) % 6];
        } else if (selected_ == 1) {
            settings_.fullscreen = !settings_.fullscreen;
        } else if (selected_ == 2) {
            // Automatic (120 on 100 Hz displays), 60, 120, uncapped (as many as the GPU renders, no vsync).
            static constexpr uint32_t kRates[] = {0, 60, 120, display::kUncapped};
            int current = 0;
            if (!settings_.interpolation) current = 1;
            else if (settings_.frame_rate == 120) current = 2;
            else if (settings_.frame_rate == display::kUncapped) current = 3;
            const int next = (current + d + 4) % 4;
            settings_.interpolation = next != 1;
            settings_.frame_rate = next == 1 ? 0 : kRates[next];
        } else {
            return;
        }
        save_display();
    }

    void activate() {
        switch (screen_) {
        case Screen::kMain:
            if (selected_ == kPlay) play();
            if (const auto slot = folder_slot(selected_)) choose_folder(*slot);
            if (selected_ == kOnlineItem) go(Screen::kOnline);
            if (selected_ == kNetplayItem) go(Screen::kNetplay);
            if (selected_ == kSettingsItem) go(Screen::kSettings);
            if (selected_ == kModsItem) go(Screen::kMods);
            if (selected_ == kQuitItem) quit_ = true;
            break;
        case Screen::kSettings:
            if (selected_ == kSettingsBack) back();
            else change(1);
            break;
        case Screen::kNetplay:
            switch (selected_) {
            case kHostSwitch: change(1); break;
            case kNameField: start_editing(selected_); break;
            default:
                if (selected_ + 1 == static_cast<int>(items().size())) back();
                else join(selected_ - kFirstRoom);
                break;
            }
            break;
        case Screen::kOnline:
            switch (selected_) {
            case kConsoleFiles: choose_console_files(); break;
            case kUserField:
            case kPasswordField: start_editing(selected_); break;
            case kSignIn: sign_in(); break;
            case kSignOut: sign_out(); break;
            default: back(); break;
            }
            break;
        case Screen::kMods: {
            const int n = static_cast<int>(mods_.size());
            if (selected_ < n) {
                change(1);
            } else if (selected_ == n) {
                choose_mod_archive();
            } else if (selected_ == n + 1) {
                std::error_code ec;
                fs::create_directories(dir_ / "mods", ec);
                SDL_OpenURL(("file:///" + (dir_ / "mods").generic_string()).c_str());
            } else {
                back();
            }
            break;
        }
        }
    }

    // mods.ini: the mods switched off, one `off = name` line each (new mods start on).
    void load_mods() {
        mods_ = find_mods(dir_ / "mods");
        std::istringstream in(read_text(dir_ / "mods.ini").value_or(""));
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("off = ", 0) != 0) continue;
            std::string name = line.substr(6);
            name.erase(name.find_last_not_of(" \r") + 1);
            for (Mod& m : mods_) m.on = m.on && m.name != name;
        }
    }
    void save_mods() {
        std::ofstream out(dir_ / "mods.ini", std::ios::binary);
        out << "# Smash 4 PC's mods (folders in mods\\ holding content\\): the ones switched off.\n";
        for (const Mod& m : mods_) {
            if (!m.on) out << "off = " << m.name << "\n";
        }
    }

    bool ready() const { return valid_[kGame]; } // the update and DLC are optional

    // ------------------------------------------------------------ the mod installer
    void choose_mod_archive() {
        if (installing_) return;
        static const auto callback = [](void* self, const char* const* files, int) {
            auto* l = static_cast<Launcher*>(self);
            std::lock_guard lock(l->dialog_mutex_);
            if (files && files[0]) l->dialog_result_ = files[0];
            l->dialog_done_ = true;
        };
        static const SDL_DialogFileFilter filters[] = {{"Mod archives", "zip;7z;rar"}, {"All files", "*"}};
        mod_dialog_ = true;
        SDL_SetWindowTitle(window_, "Choose a mod to install (.zip, .7z or .rar)");
        SDL_ShowOpenFileDialog(callback, this, window_, filters, 2, nullptr, false);
    }

    // Unpacks off the launcher's thread (a big pack takes a while).
    void start_install(const std::string& source) {
        if (installing_) return;
        installing_ = true;
        mod_ok_ = true;
        mod_status_ = "Installing " + fs::u8path(source).filename().string() + "...";
        std::thread([this, source, mods = dir_ / "mods"] {
            Installed result = install_mod(fs::u8path(source), mods);
            std::lock_guard lock(install_mutex_);
            install_result_ = std::move(result);
            install_done_ = true;
        }).detach();
    }

    void finish_install() {
        Installed r;
        {
            std::lock_guard lock(install_mutex_);
            if (!install_done_) return;
            install_done_ = false;
            r = std::move(install_result_);
        }
        installing_ = false;
        mod_ok_ = r.ok;
        if (!r.ok) {
            mod_status_ = r.message;
            return;
        }
        load_mods();
        // The new mod, switched on and selected; mods.ini lists only the ones off, so it starts on.
        for (size_t i = 0; i < mods_.size(); ++i) {
            if (mods_[i].root.lexically_relative(dir_ / "mods").begin()->string() == r.message) selected_ = static_cast<int>(i);
        }
        mod_status_ = "Installed " + r.message + (valid_[kUpdate] ? ". It's on." : ". Mods need the update to load.");
    }

    // The next folder Play asks for: the game, then the update and the DLC once each (both may be skipped).
    std::optional<Slot> missing() const {
        if (!valid_[kGame]) return kGame;
        if (!valid_[kUpdate] && !asked_[kUpdate]) return kUpdate;
        if (!valid_[kDlc] && !asked_[kDlc]) return kDlc;
        return std::nullopt;
    }

    // The game's program: smash.exe is the update's (v304), smash-base.exe the game's own (v1.0);
    // smash-compat.exe is the update's for processors without AVX2/FMA.
    fs::path game_exe() const {
        if (!valid_[kUpdate]) return dir_ / "smash-base.exe";
        return dir_ / (cafe::host::cpu_has_x86_64_v3() ? "smash.exe" : "smash-compat.exe");
    }

    void play() {
        if (process_) return;
        if (!valid_[kGame] || (!asked_[kUpdate] && !valid_[kUpdate]) || (!asked_[kDlc] && !valid_[kDlc])) {
            // Ask for what is missing, one folder after another (once per run for the optional ones).
            prompting_ = true;
            if (const auto slot = missing()) {
                choose_folder(*slot);
                return;
            }
            prompting_ = false;
        }
        if (!ready()) return;
        std::error_code ec;
        if (!fs::exists(game_exe(), ec)) {
            launch_error_ = "missing " + game_exe().filename().string();
            return;
        }
        if (!valid_[kUpdate] && !cafe::host::cpu_has_x86_64_v3()) {
            launch_error_ = "this processor lacks AVX2: add the update to play (smash-compat.exe)";
            return;
        }
        launch_error_.clear();
        // smash.exe [--host PORT] [--mod FOLDER]... UPDATE GAME; smash-base.exe GAME (mods are made for the update).
        std::vector<std::string> words{game_exe().string()};
        if (netplay_host_) words.insert(words.end(), {"--host", netplay_name_.empty() ? "Player" : netplay_name_});
        if (valid_[kUpdate]) {
            for (const Mod& m : mods_) {
                if (m.on) words.insert(words.end(), {"--mod", m.root.string()});
            }
            // The DLC's items unlock what the update carries, so only with it.
            if (valid_[kDlc]) words.insert(words.end(), {"--dlc", paths_[kDlc].string()});
            words.push_back(paths_[kUpdate].string());
        }
        words.push_back(paths_[kGame].string());
        std::vector<const char*> args;
        for (const std::string& w : words) args.push_back(w.c_str());
        args.push_back(nullptr);
        // The game's messages, for when it closes with an error.
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SMASH_LOG", (dir_ / "smash.log").string().c_str(), true);
        process_ = SDL_CreateProcess(args.data(), false);
        if (process_) {
            exit_code_ = 0;
            SDL_MinimizeWindow(window_);
        } else {
            launch_error_ = SDL_GetError();
        }
    }

    // The room list, asked for every three seconds while the Netplay page shows.
    void watch_rooms() {
        if (rooms_thread_.joinable()) return;
        rooms_thread_ = std::thread([this] {
            while (!quit_) {
                if (screen_ == Screen::kNetplay) {
                    std::vector<cafe::host::netplay::Room> rooms;
                    const bool reached = cafe::host::netplay::list_rooms(rooms);
                    std::lock_guard lock(rooms_mutex_);
                    rooms_reached_ = reached;
                    if (reached) rooms_ = rooms;
                }
                for (int i = 0; i < 30 && !quit_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        rooms_thread_.detach();
    }

    // Plays in the listed room `index`: smash.exe --join room:ID NAME (no game folder needed).
    void join(int index) {
        if (process_) return;
        std::string target;
        {
            std::lock_guard lock(rooms_mutex_);
            if (index < 0 || index >= static_cast<int>(rooms_.size())) return;
            const auto& room = rooms_[index];
            if (room.players >= room.max) {
                launch_error_ = room.name + "'s room is full";
                return;
            }
            char id[16];
            std::snprintf(id, sizeof id, "room:%06x", room.id);
            target = id;
        }
        save_launcher_ini();
        const std::string exe = (dir_ / "smash.exe").string();
        const std::string name = netplay_name_.empty() ? "Player" : netplay_name_;
        const char* args[] = {exe.c_str(), "--join", target.c_str(), name.c_str(), nullptr};
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SMASH_LOG", (dir_ / "smash.log").string().c_str(), true);
        process_ = SDL_CreateProcess(args, false);
        if (process_) {
            exit_code_ = 0;
            launch_error_.clear();
            SDL_MinimizeWindow(window_);
        } else {
            launch_error_ = SDL_GetError();
        }
    }

    void choose_folder(Slot slot) {
        // (The callback can come on another thread.)
        static const auto callback = [](void* self, const char* const* files, int) {
            auto* l = static_cast<Launcher*>(self);
            std::lock_guard lock(l->dialog_mutex_);
            if (files && files[0]) l->dialog_result_ = files[0];
            l->dialog_done_ = true; // chosen, cancelled or failed
        };
        asked_[slot] = true;
        dialog_slot_ = slot;
        // The dialog's title says which folder it wants.
        static const char* titles[kSlots] = {"Choose the game folder (0005000010144F00)",
                                             "Choose the update folder (0005000E10144F00)",
                                             "Choose the DLC folder (0005000C10144F00) - optional"};
        SDL_SetWindowTitle(window_, titles[slot]);
        fs::path start = paths_[slot];
        for (int s = 0; s < kSlots && start.empty(); ++s) start = paths_[s].parent_path();
        const std::string s = start.string();
        SDL_ShowOpenFolderDialog(callback, this, window_, s.empty() ? nullptr : s.c_str(), false);
    }

    void refresh() {
        for (int s = 0; s < kSlots; ++s) valid_[s] = !paths_[s].empty() && is_title(paths_[s], Slot(s));
        update_version_ = valid_[kUpdate] ? title_of(paths_[kUpdate])->version : 0;
        dlc_items_ = valid_[kDlc] ? dlc_items(paths_[kDlc]) : 0;
        load_mods();
        std::error_code ec;
        shaders_ = 0;
        for (const auto& entry : fs::directory_iterator(dir_ / "cache" / "spirv", ec)) shaders_ += entry.is_regular_file(ec);
    }
    void save_display() {
        std::ofstream(display::file_path(), std::ios::binary) << display::format(settings_);
    }
    void save_launcher_ini() {
        std::ofstream out(dir_ / "launcher.ini", std::ios::binary);
        out << "# Smash 4 PC's launcher: the game (0005000010144F00), its update (0005000E10144F00) and DLC\n"
               "# (0005000C10144F00) folders, as chosen in the menu. Only the game is needed.\n";
        for (int s = 0; s < kSlots; ++s) out << kSlotKeys[s] << " = " << paths_[s].string() << "\n";
        out << "netplay_host = " << (netplay_host_ ? "yes" : "no") << "\nnetplay_name = " << netplay_name_
            << "\n";
    }

    // Shortened with "..." in the middle to fit `width`.
    std::string fit(std::string text, float px, float width) {
        if (font_.width(r_, text, px) <= width) return text;
        while (text.size() > 8 && font_.width(r_, text, px) > width) text.erase(text.size() / 2 - 2, 1);
        const size_t mid = text.size() / 2;
        return text.substr(0, mid - 2) + "..." + text.substr(mid + 1);
    }

    int item_at(float x, float y) const {
        if (screen_ == Screen::kOnline) {
            for (size_t i = 0; i < online_rects_.size(); ++i) {
                const SDL_FRect& b = online_rects_[i];
                if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return static_cast<int>(i);
            }
            return -1;
        }
        const float H = layout_.h;
        if (x < layout_.left_chevron - H * 0.03f || x > layout_.right_chevron + H * 0.03f) return -1;
        for (size_t i = 0; i < list_.size(); ++i) {
            const float iy = layout_.item_y(float(i));
            if (y > iy - H * 0.06f && y < iy + H * 0.022f) return static_cast<int>(i);
        }
        return -1;
    }

    void draw_background() {
        const float W = layout_.w, H = layout_.h;
        if (background_.texture) {
            // Covering the window, drifting slowly.
            const float zoom = 1.08f + 0.02f * std::sin(time_ * 0.07f);
            const float scale = std::max(W / background_.width, H / background_.height) * zoom;
            const float bw = background_.width * scale, bh = background_.height * scale;
            const float dx = (W - bw) / 2 + std::sin(time_ * 0.05f) * W * 0.012f, dy = (H - bh) / 2;
            SDL_SetTextureColorModFloat(background_.texture, 0.82f, 0.82f, 0.86f);
            paint_.image(background_, {dx, dy, bw, bh});
        } else {
            paint_.rect(0, 0, W, H, {0.10f, 0.14f, 0.32f}, {0.04f, 0.06f, 0.16f}, {0.01f, 0.01f, 0.04f}, {0.03f, 0.04f, 0.12f});
        }
        // Dark from the left and the bottom, where the menu is.
        const Color black{0, 0, 0, 0.88f}, clear{0, 0, 0, 0};
        paint_.rect(0, 0, W * 0.30f, H, black, {0, 0, 0, 0.62f}, {0, 0, 0, 0.62f}, black);
        paint_.rect(W * 0.30f, 0, W * 0.38f, H, {0, 0, 0, 0.62f}, clear, clear, {0, 0, 0, 0.62f});
        paint_.rect(0, H * 0.62f, W, H * 0.38f, clear, clear, {0, 0, 0, 0.70f}, {0, 0, 0, 0.70f});
        paint_.rect(0, 0, W, H * 0.18f, {0, 0, 0, 0.35f}, {0, 0, 0, 0.35f}, clear, clear);
    }

    void draw_status() {
        if (screen_ == Screen::kOnline) {
            draw_online_status();
            return;
        }
        if (screen_ != Screen::kMain && screen_ != Screen::kMods && screen_ != Screen::kNetplay) return;
        const float H = layout_.h, x = layout_.text_x, y = H * 0.86f, px = H * 0.033f;
        if (screen_ == Screen::kNetplay) {
            std::string line, detail;
            Color c = kDim;
            if (!launch_error_.empty()) {
                line = "Could not start: " + launch_error_;
                c = kBad;
            } else if (selected_ >= kFirstRoom && selected_ + 1 < static_cast<int>(items().size())) {
                bool none;
                {
                    std::lock_guard lock(rooms_mutex_);
                    none = rooms_.empty();
                }
                line = none ? "Rooms appear here while their host plays with HOST MY GAMES on."
                                      : "Join: you play on the host's game as the next free player (up to 8).";
                detail = "You need no game files to join. Your pads and keyboard play as one of their Pro Controllers.";
            } else if (netplay_host_) {
                line = "Hosting: while you play, your room is listed and friends join from their list.";
                detail = "No setup needed: nothing to forward on your router. Up to 7 friends join as players 2-8.";
                c = kGood;
            } else {
                line = "Host: your game is streamed to up to 7 friends, who play as players 2-8.";
                detail = "Everything works online this way: 8-Player Smash, every stage and mode, items and mods.";
            }
            font_.draw(r_, x, y, fit(line, px * 0.85f, layout_.w * 0.62f), px * 0.85f, c);
            if (!detail.empty()) font_.draw(r_, x, y + H * 0.045f, fit(detail, px * 0.8f, layout_.w * 0.62f), px * 0.8f, kDim);
            return;
        }
        const float icon = px * 0.75f, ix = x - icon * 1.55f;
        if (screen_ == Screen::kMods && !mod_status_.empty()) {
            font_.draw(r_, x, y, fit(mod_status_, px * 0.85f, layout_.w * 0.6f), px * 0.85f, mod_ok_ ? kGood : kBad);
            font_.draw(r_, x, y + H * 0.045f, "Drop a mod's .zip, .7z or .rar (or its folder) on this window to install it.", px * 0.8f, kDim);
            return;
        }
        if (screen_ == Screen::kMods) {
            const char* note = !valid_[kUpdate] ? "Mods need the update: they are made for v304."
                               : mods_.empty()  ? "Put each mod's folder (with content\\ inside) in:"
                                                : "Mods load over the game; the first in the list wins.";
            font_.draw(r_, x, y, note, px * 0.85f, valid_[kUpdate] ? kDim : kBad);
            font_.draw(r_, x, y + H * 0.045f, fit((dir_ / "mods").string(), px * 0.8f, layout_.w * 0.5f), px * 0.8f, kDim);
            return;
        }
        std::string line, detail;
        Color c = kGood;
        if (process_) {
            paint_.spinner(ix + icon / 2, y - px * 0.33f, icon * 0.5f, H * 0.0035f, time_, kGood);
            line = "Smash 4 PC is running...";
        } else if (!launch_error_.empty()) {
            c = kBad;
            paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            line = "The game did not start: " + launch_error_;
        } else if (exit_code_ != 0) {
            c = kBad;
            paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            line = "The game closed with an error (" + std::to_string(exit_code_) + "). Details: smash.log";
        } else if (ready()) {
            paint_.check(ix, y - px * 0.78f, icon, H * 0.0035f, c);
            line = "Game ready.";
        } else {
            c = kBad;
            paint_.cross(ix, y - px * 0.7f, icon * 0.8f, H * 0.0035f, c);
            line = paths_[kGame].empty() ? "Choose the game folder." : "That game folder is not Smash for Wii U.";
        }
        font_.draw(r_, x, y, fit(line, px, layout_.w * 0.55f), px, c);
        if (const auto slot = folder_slot(selected_); slot && !paths_[*slot].empty()) {
            detail = paths_[*slot].string(); // the selected folder in full
        } else if (ready()) {
            detail = std::string("Wii U \x7f USA \x7f ") +
                     (valid_[kUpdate] ? "Update v" + std::to_string(update_version_) : std::string("No update (v1.0)")) +
                     " \x7f " + (!valid_[kDlc]      ? std::string("No DLC")
                         : dlc_items_ > 0 ? "DLC found: " + std::to_string(dlc_items_) + " items"
                                          : std::string("DLC folder has no items"));
        } else {
            detail = "Needs the game, 0005000010144F00 (the update and DLC are optional)";
        }
        detail = fit(detail, px * 0.8f, layout_.w * 0.6f);
        // (0x7f: a bullet, drawn below)
        float dx = x;
        const float dpx = px * 0.8f;
        size_t start = 0;
        for (size_t i = 0; i <= detail.size(); ++i) {
            if (i == detail.size() || detail[i] == '\x7f') {
                dx += font_.draw(r_, dx, y + H * 0.05f, detail.substr(start, i - start), dpx, kDim);
                if (i < detail.size()) {
                    const float r = dpx * 0.09f;
                    paint_.round_rect(dx + dpx * 0.1f - r, y + H * 0.05f - dpx * 0.3f - r, r * 2, r * 2, r, kDim);
                    dx += dpx * 0.2f;
                }
                start = i + 1;
            }
        }
    }

    // ------------------------------------------------------------ the Online page
    std::string online_value() const {
        if (online::account().present) return "Signed in: " + online::account().account_id;
        return online::device().present ? "Not signed in" : "Not set up";
    }

    static size_t utf8_length(const std::string& s) {
        return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (c & 0xC0) != 0x80; }));
    }

    void start_editing(int item) {
        editing_ = item;
        SDL_StartTextInput(window_);
    }
    void stop_editing() {
        if (screen_ == Screen::kNetplay && editing_ >= 0) save_launcher_ini();
        editing_ = -1;
        SDL_StopTextInput(window_);
    }

    // Typing in the ID or password field; true if the event was the field's.
    bool text_event(const SDL_Event& e) {
        const bool netplay = screen_ == Screen::kNetplay;
        std::string& text = netplay ? netplay_name_ : editing_ == kUserField ? user_ : password_;
        // A PNID is at most 16 characters (and a netplay name, sent in 31 bytes).
        const size_t limit = editing_ == kUserField || (netplay && editing_ == kNameField) ? 16 : 64;
        switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
            for (const char* c = e.text.text; *c && utf8_length(text) < limit; ++c) text += *c;
            return true;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_BACKSPACE) {
                while (!text.empty() && (text.back() & 0xC0) == 0x80) text.pop_back();
                if (!text.empty()) text.pop_back();
            } else if (e.key.key == SDLK_V && (e.key.mod & SDL_KMOD_CTRL)) {
                if (char* clip = SDL_GetClipboardText()) {
                    for (const char* c = clip; *c && *c != '\n' && *c != '\r' && utf8_length(text) < limit; ++c) text += *c;
                    SDL_free(clip);
                }
            } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_TAB) {
                // On from the ID to the password; from the password, sign in.
                const int was = editing_;
                stop_editing();
                if (netplay) {
                } else if (was == kUserField) {
                    selected_ = kPasswordField;
                    start_editing(kPasswordField);
                } else if (e.key.key != SDLK_TAB) {
                    selected_ = kSignIn;
                    sign_in();
                }
            } else if (e.key.key == SDLK_ESCAPE) {
                stop_editing();
            }
            return true; // (letters arrive as text; the menu keys stay with the field)
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST || e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                stop_editing();
                return true;
            }
            return false;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: stop_editing(); return false;
        default: return false;
        }
    }

    // Dumpling's online files: otp.bin, seeprom.bin and (if the console's account was dumped) its account.dat.
    void choose_console_files() {
        static const auto callback = [](void* self, const char* const* files, int) {
            auto* l = static_cast<Launcher*>(self);
            std::lock_guard lock(l->dialog_mutex_);
            if (files && files[0]) l->dialog_result_ = files[0];
            l->dialog_done_ = true;
        };
        online_dialog_ = true;
        SDL_SetWindowTitle(window_, "Choose the folder with your Wii U's otp.bin and seeprom.bin (Dumpling's Online Files)");
        SDL_ShowOpenFolderDialog(callback, this, window_, nullptr, false);
    }

    void import_console_files(const fs::path& folder) {
        std::error_code ec;
        fs::path otp, seeprom, account;
        // (Searching a few folders down: Dumpling puts account.dat under mlc01\usr\save\system\act\80000001.)
        for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || it.depth() > 7) {
                if (it.depth() > 7) it.disable_recursion_pending();
                continue;
            }
            const std::string name = it->path().filename().string();
            if (name == "otp.bin" && otp.empty()) otp = it->path();
            if (name == "seeprom.bin" && seeprom.empty()) seeprom = it->path();
            if (name == "account.dat" && account.empty() && it->path().parent_path().filename() == "80000001") account = it->path();
        }
        if (otp.empty() || seeprom.empty() || fs::file_size(otp, ec) != 1024 || fs::file_size(seeprom, ec) != 512) {
            online_status_ = "No otp.bin and seeprom.bin from a Wii U in that folder.";
            online_ok_ = false;
            return;
        }
        const fs::path dir = dir_ / "online";
        fs::create_directories(dir, ec);
        fs::copy_file(otp, dir / "otp.bin", fs::copy_options::overwrite_existing, ec);
        fs::copy_file(seeprom, dir / "seeprom.bin", fs::copy_options::overwrite_existing, ec);
        if (!account.empty()) fs::copy_file(account, dir / "account.dat", fs::copy_options::overwrite_existing, ec);
        online::load(dir);
        online_ok_ = online::device().present;
        online_status_ = !online_ok_                   ? "Those console files could not be read."
                         : online::account().present ? "Console files and account found: signed in as " + online::account().account_id + "."
                                                       : "Console files found. Now sign in with your Pretendo ID.";
    }

    void sign_in() {
        if (signing_in_) return;
        if (!online::device().present) {
            online_status_ = "Choose your console files first.";
            online_ok_ = false;
            return;
        }
        if (user_.empty() || password_.empty()) {
            online_status_ = "Type your Pretendo ID and password.";
            online_ok_ = false;
            return;
        }
        signing_in_ = true;
        online_status_ = "Signing in to Pretendo...";
        online_ok_ = true;
        std::thread([this, user = user_, password = password_] {
            act_server::SignIn r = act_server::sign_in(user, password);
            std::lock_guard lock(sign_in_mutex_);
            sign_in_result_ = std::move(r);
            sign_in_done_ = true;
        }).detach();
        password_.assign(password_.size(), '\0');
        password_.clear();
    }

    // The sign-in's result, on the launcher's thread: account.dat, or why not.
    void finish_sign_in() {
        act_server::SignIn r;
        {
            std::lock_guard lock(sign_in_mutex_);
            if (!sign_in_done_) return;
            sign_in_done_ = false;
            r = std::move(sign_in_result_);
        }
        signing_in_ = false;
        if (!r.ok) {
            online_ok_ = false;
            online_status_ = "Sign-in failed: " + (r.message.empty() ? "error " + std::to_string(r.error_code) : r.message);
            return;
        }
        online::NewAccount a;
        a.user_id = r.user_id;
        a.pid = r.pid;
        a.password_hash = r.password_hash;
        a.mii = r.mii;
        a.mii_name = r.mii_name;
        a.us = r.country.empty() || r.country == "US";
        online_ok_ = online::save_account(dir_ / "online", a);
        online_status_ = online_ok_ ? "Signed in as " + r.user_id + ". Smash's online modes use this account."
                                    : "Signed in, but the account could not be saved.";
    }

    void sign_out() {
        std::error_code ec;
        fs::remove(dir_ / "online" / "account.dat", ec);
        online::load(dir_ / "online");
        online_ok_ = true;
        online_status_ = "Signed out. The console files stay; sign in again any time.";
    }

    void draw_online_status() {
        const float H = layout_.h, x = layout_.text_x, y = H * 0.86f, px = H * 0.03f;
        const std::string line = !online_status_.empty() ? online_status_
                                 : online::account().present
                                     ? "Signed in as " + online::account().account_id + " on Pretendo Network."
                                     : "Online play uses Pretendo Network: your Wii U's files and your Pretendo ID.";
        font_.draw(r_, x, y, fit(line, px, layout_.w * 0.6f), px, online_status_.empty() || online_ok_ ? kGood : kBad);
        const std::string hint = selected_ == kConsoleFiles
                                     ? "Dumpling (on your Wii U): Dump Online Files. Choose the folder it made."
                                     : "Your password is not saved: only the console's hash of it, as on a Wii U.";
        font_.draw(r_, x, y + H * 0.045f, fit(hint, px * 0.8f, layout_.w * 0.6f), px * 0.8f, kDim);
    }

    // ------------------------------------------------------------ the Online page, Wii U style
    // Light gray with faint diagonal stripes, a white title bar, white rounded rows with a cyan
    // outline on the selected one, inset text boxes, the blue pill button and a footer of round
    // button hints: the look of the Wii U's System Settings.
    void draw_wiiu_online() {
        const float W = layout_.w, H = layout_.h;
        const Color ink{0.24f, 0.24f, 0.26f}, soft{0.52f, 0.53f, 0.56f}, cyan{0.0f, 0.68f, 0.90f};
        const Color white{1, 1, 1}, shade{0, 0, 0, 0.10f};
        const float a = fade_;
        const auto fa = [&](Color c, float k = 1) { return Color{c.r, c.g, c.b, c.a * a * k}; };
        // Background: the Wii U's pale gray with Smash 4's light rays and its emblem, faint.
        paint_.rect(0, 0, W, H, {0.93f, 0.96f, 0.99f}, {0.93f, 0.96f, 0.99f}, {0.84f, 0.87f, 0.92f},
                    {0.84f, 0.87f, 0.92f});
        {
            const float ox = W * 0.5f, oy = -H * 0.35f, reach = W + H;
            const float turn = time_ * 0.02f;
            for (int i = 0; i < 24; i += 2) {
                const float a0 = float(M_PI) * (0.08f + i / 24.0f * 0.84f) + turn, a1 = a0 + float(M_PI) * 0.84f / 24;
                SDL_FPoint p[4] = {{ox, oy}, {ox + std::cos(a0) * reach, oy + std::sin(a0) * reach},
                                   {ox + std::cos(a1) * reach, oy + std::sin(a1) * reach}, {ox, oy}};
                const Color c[4] = {{1, 1, 1, 0.0f}, {1, 1, 1, 0.22f}, {1, 1, 1, 0.22f}, {1, 1, 1, 0.0f}};
                paint_.quad(p, c);
            }
            // The Smash emblem: a ring split by a vertical and a horizontal bar.
            const float er = H * 0.42f, ecx = W - H * 0.28f, ecy = H * 0.62f, t = H * 0.055f;
            const Color mark{0.0f, 0.45f, 0.75f, 0.06f};
            const float r0 = er - t / 2, r1 = er + t / 2;
            for (int k = 0; k < 120; ++k) { // the ring, segment by segment, edge to edge
                const float b0 = k / 120.0f * 2 * float(M_PI), b1 = (k + 1) / 120.0f * 2 * float(M_PI);
                SDL_FPoint q[4] = {{ecx + std::cos(b0) * r1, ecy + std::sin(b0) * r1},
                                   {ecx + std::cos(b1) * r1, ecy + std::sin(b1) * r1},
                                   {ecx + std::cos(b1) * r0, ecy + std::sin(b1) * r0},
                                   {ecx + std::cos(b0) * r0, ecy + std::sin(b0) * r0}};
                const Color mc[4] = {mark, mark, mark, mark};
                paint_.quad(q, mc);
            }
            paint_.rect(ecx + er * 0.12f, ecy - er, t * 1.2f, er * 2, mark);
            paint_.rect(ecx - er, ecy + er * 0.3f, er * 2, t * 1.1f, mark);
        }
        // Title bar: Smash 4's slanted ONLINE banner, then the Wii U settings title; the sign-in state on the right.
        const float bar = H * 0.115f;
        paint_.rect(0, 0, W, bar, white);
        paint_.rect(0, bar, W, H * 0.008f, {0, 0, 0, 0.10f}, {0, 0, 0, 0.10f}, {0, 0, 0, 0}, {0, 0, 0, 0});
        const float bw0 = H * 0.42f, slant = bar * 0.45f;
        {
            SDL_FPoint p[4] = {{0, 0}, {bw0 + slant, 0}, {bw0, bar}, {0, bar}};
            const Color c[4] = {{0.0f, 0.70f, 0.95f}, {0.10f, 0.45f, 0.90f}, {0.10f, 0.40f, 0.85f}, {0.0f, 0.62f, 0.90f}};
            paint_.quad(p, c);
            SDL_FPoint s2[4] = {{bw0 + slant + H * 0.012f, 0}, {bw0 + slant + H * 0.03f, 0}, {bw0 + H * 0.03f, bar},
                                {bw0 + H * 0.012f, bar}};
            const Color c2[4] = {cyan, cyan, cyan, cyan};
            paint_.quad(s2, c2);
        }
        // (Bold: drawn twice, a pixel apart.)
        const float tpx = H * 0.058f;
        font_.draw(r_, H * 0.05f, bar * 0.72f, "ONLINE", tpx, {0, 0, 0, 0.25f}, false);
        font_.draw(r_, H * 0.047f, bar * 0.70f, "ONLINE", tpx, white, false);
        font_.draw(r_, H * 0.047f + 1.5f, bar * 0.70f, "ONLINE", tpx, white, false);
        font_.draw(r_, bw0 + slant + H * 0.06f, bar * 0.64f, "Pretendo Network ID", H * 0.04f, ink, false);
        const bool signed_in = online::account().present;
        const std::string state = signed_in ? online::account().account_id : "Not signed in";
        const float sw = font_.width(r_, state, H * 0.028f) + H * 0.07f;
        const float sx = W - H * 0.05f - sw, sy = bar * 0.27f, sh = bar * 0.46f;
        paint_.round_rect(sx, sy, sw, sh, sh / 2, signed_in ? Color{0.90f, 0.97f, 1.0f} : Color{0.94f, 0.94f, 0.95f});
        paint_.round_rect(sx + sh * 0.32f, sy + sh * 0.36f, sh * 0.28f, sh * 0.28f, sh * 0.14f,
                          signed_in ? Color{0.25f, 0.75f, 0.35f} : soft);
        font_.draw(r_, sx + sh * 0.8f, sy + sh * 0.68f, state, H * 0.028f, ink, false);

        // The rows: Console files, ID, password, then the two buttons.
        const auto& list = items();
        online_rects_.assign(list.size(), SDL_FRect{});
        const float cw = std::min(W * 0.62f, H * 1.15f), cx = (W - cw) / 2;
        const float rh = H * 0.088f, gap = H * 0.02f;
        float y = bar + H * 0.06f;
        const float px = H * 0.032f, slide = (1 - a) * H * 0.02f;
        for (int i = kConsoleFiles; i <= kPasswordField; ++i) {
            const bool sel = selected_ == i;
            const SDL_FRect b{cx, y + slide, cw, rh};
            online_rects_[i] = b;
            if (sel) {
                // A pulsing glow around the Wii U's cyan outline.
                const float pulse = 0.5f + 0.5f * std::sin(time_ * 5.0f), g = H * (0.014f + 0.006f * pulse);
                paint_.round_rect(b.x - g, b.y - g, b.w + 2 * g, b.h + 2 * g, H * 0.018f + g, fa(cyan, 0.18f + 0.12f * pulse));
                paint_.round_rect(b.x - H * 0.007f, b.y - H * 0.007f, b.w + H * 0.014f, b.h + H * 0.014f, H * 0.025f,
                                  fa(cyan, 0.95f));
            }
            paint_.round_rect(b.x + 2, b.y + 4, b.w, b.h, H * 0.018f, fa(shade));
            paint_.round_rect(b.x, b.y, b.w, b.h, H * 0.018f, fa(white));
            // Smash 4's colour strip down the left of each item.
            paint_.round_rect(b.x + H * 0.008f, b.y + rh * 0.18f, H * 0.008f, rh * 0.64f, H * 0.004f,
                              fa(sel ? cyan : Color{0.70f, 0.78f, 0.86f}));
            static const char* kNames[] = {"Console files", "Pretendo ID", "Password"};
            font_.draw(r_, b.x + H * 0.035f, b.y + rh * 0.62f, kNames[i], px, fa(ink), false);
            const float vx = b.x + b.w * 0.42f, vw = b.w * 0.58f - H * 0.03f;
            if (i == kConsoleFiles) {
                const bool found = online::device().present;
                const std::string v = found ? "Found" : "Choose...";
                const float tw = font_.width(r_, v, px * 0.9f);
                if (found) paint_.check(b.x + b.w - H * 0.035f - tw - H * 0.045f, b.y + rh * 0.34f, px * 0.75f, H * 0.004f,
                                        fa({0.25f, 0.70f, 0.32f}));
                font_.draw(r_, b.x + b.w - H * 0.035f - tw, b.y + rh * 0.62f, v, px * 0.9f, fa(found ? ink : cyan), false);
            } else {
                // An inset text box.
                const float bh = rh * 0.62f, by = b.y + (rh - bh) / 2;
                paint_.round_rect(vx, by, vw, bh, H * 0.012f, fa({0.86f, 0.87f, 0.89f}));
                paint_.round_rect(vx + 1.5f, by + 2.5f, vw - 3, bh - 4, H * 0.011f, fa({0.975f, 0.978f, 0.985f}));
                const std::string& text = i == kUserField ? user_ : password_;
                std::string shown = i == kPasswordField ? std::string(utf8_length(text), '*') : text;
                const bool typing = editing_ == i;
                if (shown.empty() && !typing) {
                    font_.draw(r_, vx + H * 0.02f, by + bh * 0.7f, i == kUserField ? "Tap to type" : "Tap to type",
                               px * 0.85f, fa(soft, 0.8f), false);
                } else {
                    const float tw = font_.draw(r_, vx + H * 0.02f, by + bh * 0.7f, shown, px * 0.9f, fa(ink), false);
                    if (typing && std::fmod(time_, 1.0f) < 0.55f) {
                        paint_.rect(vx + H * 0.022f + tw, by + bh * 0.2f, H * 0.003f, bh * 0.6f, fa(cyan));
                    }
                }
                if (typing) paint_.round_rect(vx - 1, by - 1, vw + 2, H * 0.004f, H * 0.002f, fa(cyan, 0.0f));
            }
            y += rh + gap;
        }

        // Sign In: the blue pill; Sign Out: a white one (when signed in).
        y += gap;
        const float bw = cw * 0.42f, bh2 = rh * 0.86f;
        const auto pill = [&](int item, float x, const std::string& label, bool primary, bool enabled) {
            const bool sel = selected_ == item;
            const SDL_FRect b{x, y + slide, bw, bh2};
            online_rects_[item] = b;
            if (sel) paint_.round_rect(b.x - H * 0.008f, b.y - H * 0.008f, b.w + H * 0.016f, b.h + H * 0.016f,
                                       (bh2 + H * 0.016f) / 2, fa(cyan, 0.9f));
            paint_.round_rect(b.x + 2, b.y + 4, b.w, b.h, bh2 / 2, fa(shade));
            if (primary && enabled) {
                paint_.round_rect(b.x, b.y, b.w, b.h, bh2 / 2, fa({0.0f, 0.58f, 0.85f}));
                paint_.round_rect(b.x + 3, b.y + 2, b.w - 6, b.h * 0.5f, bh2 / 2, fa({1, 1, 1}, 0.18f));
            } else {
                paint_.round_rect(b.x, b.y, b.w, b.h, bh2 / 2, fa(white));
            }
            const float tw = font_.width(r_, label, px);
            const Color tc = primary && enabled ? white : enabled ? ink : soft;
            font_.draw(r_, b.x + (b.w - tw) / 2, b.y + bh2 * 0.64f, label, px, fa(tc), false);
        };
        const bool can_sign_in = online::device().present && !signing_in_;
        pill(kSignIn, cx + cw / 2 - bw - H * 0.02f, signing_in_ ? "Signing in..." : "Sign In", true, can_sign_in);
        pill(kSignOut, cx + cw / 2 + H * 0.02f, "Sign Out", false, signed_in);
        if (signing_in_) {
            const SDL_FRect& b = online_rects_[kSignIn];
            paint_.spinner(b.x + bh2 * 0.5f, b.y + bh2 / 2, bh2 * 0.22f, H * 0.004f, time_, white);
        }
        y += bh2 + gap * 1.8f;

        // What happened, or what to do.
        const std::string line = !online_status_.empty() ? online_status_
                                 : signed_in ? "Signed in. Smash's online modes use this account on Pretendo Network."
                                             : "Sign in with your Pretendo Network ID to play online.";
        const Color lc = online_status_.empty() || online_ok_ ? ink : Color{0.85f, 0.25f, 0.22f};
        const float lw = font_.width(r_, line, px * 0.8f);
        font_.draw(r_, (W - std::min(lw, W * 0.9f)) / 2, y + slide, fit(line, px * 0.8f, W * 0.9f), px * 0.8f, fa(lc), false);
        const std::string hint = selected_ == kConsoleFiles
                                     ? "On your Wii U, Dumpling > Dump Online Files. Then choose the folder it made."
                                     : "Your password is not saved, only the console's hash of it, as on a Wii U.";
        const float hw = font_.width(r_, hint, px * 0.7f);
        font_.draw(r_, (W - std::min(hw, W * 0.9f)) / 2, y + H * 0.045f + slide, fit(hint, px * 0.7f, W * 0.9f), px * 0.7f,
                   fa(soft), false);

        // Footer: the round button hints, Back on the left (clickable) and OK on the right.
        const float fh = H * 0.1f, fy = H - fh;
        paint_.rect(0, fy, W, fh, {0.84f, 0.85f, 0.87f});
        paint_.rect(0, fy, W, H * 0.004f, {0.78f, 0.79f, 0.81f});
        const auto hint_button = [&](float x, const char* letter, const char* text, bool selected) {
            const float d = fh * 0.46f, by = fy + (fh - d) / 2;
            const float tw = font_.width(r_, text, H * 0.03f);
            const SDL_FRect b{x - fh * 0.12f, fy + fh * 0.14f, d + tw + fh * 0.5f, fh * 0.72f};
            if (selected) paint_.round_rect(b.x - 3, b.y - 3, b.w + 6, b.h + 6, (b.h + 6) / 2, fa(cyan, 0.9f));
            paint_.round_rect(b.x, b.y, b.w, b.h, b.h / 2, {1, 1, 1, 0.9f});
            paint_.round_rect(x, by, d, d, d / 2, {0.33f, 0.34f, 0.37f});
            const float lw2 = font_.width(r_, letter, H * 0.026f);
            font_.draw(r_, x + (d - lw2) / 2, by + d * 0.7f, letter, H * 0.026f, white, false);
            font_.draw(r_, x + d + fh * 0.18f, by + d * 0.72f, text, H * 0.03f, ink, false);
            return b;
        };
        online_rects_[kOnlineBack] = hint_button(H * 0.05f, "B", "Back", selected_ == kOnlineBack);
        hint_button(W - H * 0.05f - fh * 0.46f - font_.width(r_, "OK", H * 0.03f) - fh * 0.3f, "A", "OK", false);
    }

    void draw_toast() {
        const float H = layout_.h;
        const float px = H * 0.027f;
        const std::string text = shaders_ > 0 ? std::to_string(shaders_) + " shaders ready"
                                              : "Shaders compile the first time you play";
        const float tw = font_.width(r_, text, px);
        const float bx = H * 0.018f, bh = H * 0.068f, by = H - bh - H * 0.02f, bw = tw + H * 0.11f;
        paint_.round_rect(bx, by, bw, bh, H * 0.008f, {kGold.r, kGold.g, kGold.b, 0.75f});
        paint_.round_rect(bx + 1.5f, by + 1.5f, bw - 3, bh - 3, H * 0.007f, {0.04f, 0.035f, 0.03f, 0.88f});
        const float icon = px * 0.8f, ix = bx + H * 0.03f, cy = by + bh / 2;
        if (shaders_ > 0) {
            paint_.check(ix, cy - icon / 2, icon, H * 0.0032f, kGood);
        } else {
            paint_.spinner(ix + icon / 2, cy, icon / 2, H * 0.003f, time_, kWhite);
        }
        font_.draw(r_, ix + H * 0.045f, cy + px * 0.36f, text, px, kWhite, false);
    }

    SDL_Window* window_;
    SDL_Renderer* r_;
    Painter paint_;
    Font font_;
    Image logo_, background_;
    fs::path dir_;
    fs::path paths_[kSlots];
    bool valid_[kSlots] = {};
    int dlc_items_ = 0; // DLC items present (owned)
    int update_version_ = 0;
    Slot dialog_slot_ = kGame;
    bool dialog_done_ = false, prompting_ = false;
    bool asked_[kSlots] = {};
    bool mouse_moved_ = false;
    std::vector<Mod> mods_;
    display::Settings settings_;
    int shaders_ = 0;
    Screen screen_ = Screen::kMain;
    std::vector<Item> list_;
    int selected_ = 0;
    float selection_y_ = 0, fade_ = 0, time_ = 0;
    float stick_x_ = 0, stick_y_ = 0, held_x_ = 0, held_y_ = 0;
    SDL_Process* process_ = nullptr;
    int exit_code_ = 0;
    std::string launch_error_;
    std::mutex dialog_mutex_;
    std::string dialog_result_;
    bool quit_ = false;
    // The Online page.
    bool online_dialog_ = false, signing_in_ = false, online_ok_ = true;
    // The mod installer.
    bool mod_dialog_ = false, installing_ = false, mod_ok_ = true, install_done_ = false;
    std::string mod_status_;
    std::mutex install_mutex_;
    Installed install_result_;
    int editing_ = -1; // the field being typed in
    // Netplay.
    bool netplay_host_ = false;
    std::string netplay_name_;
    std::thread rooms_thread_;
    std::mutex rooms_mutex_;
    std::vector<cafe::host::netplay::Room> rooms_;
    bool rooms_reached_ = false;
    std::vector<SDL_FRect> online_rects_; // each Online row's box, for the mouse
    std::string user_, password_, online_status_;
    std::mutex sign_in_mutex_;
    bool sign_in_done_ = false;
    act_server::SignIn sign_in_result_;
};

} // namespace

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Smash 4 PC", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Smash 4 PC", 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Smash 4 PC", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, 960, 540);
    {
        Launcher launcher(window, renderer);
        // SMASH_LAUNCHER_SHOT=<file.bmp>: saves the frame shown after 1.5 s and quits
        // (SMASH_LAUNCHER_SCREEN=settings, mods or online: that screen).
        const char* shot = std::getenv("SMASH_LAUNCHER_SHOT");
        if (const char* s = std::getenv("SMASH_LAUNCHER_SCREEN")) launcher.open_screen(s);
        if (argc > 1) launcher.install(argv[1]); // a mod's archive or folder dropped on the launcher's icon
        const uint64_t started = SDL_GetTicksNS();
        uint64_t last = started;
        while (launcher.running()) {
            SDL_Event e;
            // While the game runs the launcher only waits for it.
            if (launcher.game_running() ? SDL_WaitEventTimeout(&e, 250) : SDL_PollEvent(&e)) {
                do launcher.event(e);
                while (SDL_PollEvent(&e));
            }
            const uint64_t now = SDL_GetTicksNS();
            launcher.update(std::min(0.1f, (now - last) / 1e9f));
            last = now;
            const bool save = shot && now - started > 1500000000ull && !launcher.installing();
            if (!launcher.game_running()) launcher.draw(save);
            if (save) break;
        }
        if (shot) std::fprintf(stderr, "launcher: stopped after %.2f s\n", (SDL_GetTicksNS() - started) / 1e9);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
