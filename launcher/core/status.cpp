// The importer's status (see status.h).

#include "status.h"

#include "json.h"

#include <fstream>
#include <sstream>

namespace launcher {

std::optional<ImporterStatus> parse_status(std::string_view text, std::string* error) {
    const auto doc = json::parse(text, error);
    if (!doc) return std::nullopt;
    if (!doc->is_object()) {
        if (error) *error = "the status is not a JSON object";
        return std::nullopt;
    }
    const json::Value& d = *doc;
    ImporterStatus s;
    s.python_version = d["python"]["version"].string();
    s.numpy = d["python"]["numpy"].boolean();
    s.pillow = d["python"]["pillow"].boolean();
    s.gamescope = d["tools"]["gamescope"].string();
    s.pactl = d["tools"]["pactl"].string();
    s.helper = d["tools"]["helper"].string();
    s.steam_installed = d["steam"]["installed"].boolean();
    s.steam_running = d["steam"]["running"].boolean();
    s.tekken7_found = d["tekken7"]["found"].boolean();
    s.tekken7_path = d["tekken7"]["path"].string();
    s.tekken7_error = d["tekken7"]["error"].string();
    s.packs_folder = d["packs_folder"].string();
    s.heihachi_saved = d["movesets"]["heihachi"].boolean(true);
    s.game_error = d["game"]["error"].string();
    for (const json::Value& f : d["fighters"].items()) {
        Fighter fighter;
        fighter.name = f["name"].string();
        if (fighter.name.empty()) continue;
        fighter.display = f["display"].string(fighter.name);
        fighter.dlc = static_cast<int64_t>(f["dlc"].number(0));
        fighter.installed = f["installed"].boolean();
        fighter.outdated = fighter.installed && f["outdated"].boolean();
        fighter.newer = fighter.installed && !fighter.outdated && f["newer"].boolean();
        fighter.moveset_saved = f["moveset_saved"].boolean();
        s.fighters.push_back(std::move(fighter));
    }
    return s;
}

Ownership parse_owned(std::string_view text) {
    Ownership out;
    std::string error;
    const auto doc = json::parse(text, &error);
    if (!doc || !doc->is_object()) {
        out.error = doc ? "the answer is not a JSON object" : "the answer is not JSON (" + error + ")";
        return out;
    }
    if ((*doc)["error"].is_string()) {
        out.error = (*doc)["error"].string();
        return out;
    }
    const json::Value& owned = (*doc)["owned"];
    if (!owned.is_object()) {
        out.error = "the answer has no \"owned\"";
        return out;
    }
    for (size_t i = 0; i < owned.keys().size(); ++i) out.owned[owned.keys()[i]] = owned.values()[i].boolean();
    return out;
}

bool save_ownership(const std::filesystem::path& file, std::string_view json) {
    if (!parse_owned(json).error.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::trunc);
        out << json;
        if (!out) return false;
    }
    std::filesystem::rename(part, file, ec);
    return !ec;
}

std::optional<Ownership> saved_ownership(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    Ownership owned = parse_owned(text.str());
    if (!owned.error.empty()) return std::nullopt;
    return owned;
}

} // namespace launcher
