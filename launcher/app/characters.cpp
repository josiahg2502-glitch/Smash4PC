// The characters' section (see launcher.h): Tekken 7 and Steam as the
// importer finds them, what is missing to import, Tekken 7's own characters
// and its DLC characters (with whether the player owns them), and adding and
// removing them through import_character.py.

#include "launcher.h"

#include "system.h"
#include "widgets.h"

namespace launcher {
namespace {

GtkWidget* fact_name(const char* text) {
    GtkWidget* l = ui::label(text, "dim", false);
    gtk_widget_set_valign(l, GTK_ALIGN_START);
    return l;
}

std::string display_name(const std::optional<ImporterStatus>& status, const std::string& name) {
    if (status) {
        for (const Fighter& f : status->fighters) {
            if (f.name == name) return f.display;
        }
    }
    return name;
}

std::string names_text(const std::optional<ImporterStatus>& status, const std::vector<std::string>& names) {
    if (names.size() == 1) return display_name(status, names[0]);
    return std::to_string(names.size()) + " characters";
}

// What to do to get these pieces: the command that installs them, or on a
// system whose package manager cannot add to it, where they come from.
GtkWidget* install_hint(const Distro& distro, const std::vector<Package>& packages, const std::string& what,
                        const char* fallback) {
    if (image_based(distro)) return ui::label(image_based_hint(what), "small dim");
    const std::string command = install_command(distro, packages);
    return command.empty() ? ui::label(fallback, "small dim") : ui::command(command);
}

} // namespace

std::optional<bool> Launcher::owns(const Fighter& fighter) const {
    if (fighter.dlc == 0) return true;
    if (!owned_answer_) return std::nullopt;
    const auto it = owned_.find(fighter.name);
    if (it == owned_.end()) return std::nullopt;
    return it->second;
}

bool Launcher::can_import(std::string* why) const {
    const auto no = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (importer_.empty() || python_.empty()) return no("The character importer or Python 3 is missing.");
    if (install_.state != InstallState::kInstalled) return no("Install the game first: characters are made from it.");
    if (!status_) return no("The importer cannot tell what this PC has: " + status_error_);
    if (!status_->tekken7_usable()) {
        return no(status_->tekken7_error.empty() ? "Tekken 7 is not installed." : status_->tekken7_error);
    }
    if (!status_->numpy || !status_->pillow) return no("Python needs numpy and Pillow to add characters.");
    if (!status_->game_error.empty()) return no("The importer cannot read the installed game: " + status_->game_error);
    return true;
}

bool Launcher::addable(const Fighter& f) const {
    // (A moveset not saved yet is saved from Tekken 7, which needs Steam; a
    // DLC fighter's import asks Steam whether the DLC is owned, every time.
    // An older importer's pack is replaced, a newer one's never.)
    if (f.added() || f.newer || owns(f) != true || !status_) return false;
    if (f.dlc != 0) return status_->steam_running;
    return status_->steam_running || (f.moveset_saved && status_->heihachi_saved);
}

std::vector<std::string> Launcher::base_to_add() const {
    std::vector<std::string> names;
    if (status_) {
        for (const Fighter& f : status_->fighters) {
            if (f.dlc == 0 && addable(f)) names.push_back(f.name);
        }
    }
    return names;
}

std::vector<std::string> Launcher::owned_to_add() const {
    std::vector<std::string> names;
    if (status_) {
        for (const Fighter& f : status_->fighters) {
            if (f.dlc != 0 && addable(f)) names.push_back(f.name);
        }
    }
    return names;
}

// ---------------------------------------------------------------- the section

GtkWidget* Launcher::fighter_row(const Fighter& f, bool usable) {
    // A line each: the name, the state (added, made by an older version, or
    // for the DLC's whether it is owned) and the buttons, in columns.
    GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* name = ui::label(f.display, nullptr, false);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(row), name);
    GtkWidget* state = ui::box(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(state, 96, -1);
    gtk_widget_set_halign(state, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), state);
    const std::optional<bool> owned = owns(f);
    GtkWidget* state_label = nullptr;
    if (f.outdated) {
        state_label = ui::label("Older version", "pill warn", false);
        ui::set_tooltip(state_label, "Made by an older version: the game leaves them out until they are added again.");
    } else if (f.newer) {
        state_label = ui::label("Newer version", "pill warn", false);
        ui::set_tooltip(state_label, "Made by a newer version of Ultimate Launcher: the game leaves them out until it "
                                     "is updated.");
    } else if (f.installed) {
        state_label = ui::label("Added", "pill ok", false);
    } else if (f.dlc != 0 && owned) {
        state_label = ui::label(*owned ? "Owned" : "Not owned", "small dim", false);
    }
    if (state_label) {
        gtk_widget_set_hexpand(state_label, TRUE);
        gtk_widget_set_halign(state_label, GTK_ALIGN_END);
        gtk_widget_set_valign(state_label, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(state), state_label);
    }

    const auto append = [&](GtkWidget* widget) {
        g_object_set_data_full(G_OBJECT(widget), "fighter", g_strdup(f.name.c_str()), g_free);
        gtk_widget_set_size_request(widget, 88, -1);
        gtk_box_append(GTK_BOX(row), widget);
    };
    const auto remove_button = [&](const std::string& id) {
        GtkWidget* button = ui::button("Remove", "flat");
        ui::set_id(button, id);
        gtk_widget_set_sensitive(button, !task_ && install_.state == InstallState::kInstalled);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkWidget* b, gpointer data) {
                             static_cast<Launcher*>(data)->start_remove(
                                 {static_cast<const char*>(g_object_get_data(G_OBJECT(b), "fighter"))});
                         }),
                         this);
        append(button);
    };
    const auto add_button = [&](const char* text) {
        GtkWidget* button = ui::button(text);
        ui::set_id(button, "fighter:" + f.name); // (the row's button: Add, then Remove)
        gtk_widget_set_sensitive(button, usable && !task_ && addable(f));
        // (When adding is impossible for another reason, the card says why above.)
        const char* why = !usable              ? nullptr
                          : !owned             ? "Start Steam: it tells which DLC you own"
                          : addable(f)         ? nullptr
                          : f.dlc != 0         ? "Start Steam: it checks that you own their DLC"
                                               : "Start Steam: Tekken 7 needs it to save their moves";
        if (why) gtk_widget_set_tooltip_text(button, why);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkWidget* b, gpointer data) {
                             static_cast<Launcher*>(data)->start_import(
                                 {static_cast<const char*>(g_object_get_data(G_OBJECT(b), "fighter"))});
                         }),
                         this);
        append(button);
    };
    if (f.outdated) {
        // (Adding them again replaces the old pack: the main button. A DLC
        // no longer owned cannot be added again, and says so there.)
        remove_button("remove:" + f.name);
        if (owned != false) {
            add_button("Add Again");
        } else {
            GtkWidget* note = ui::label("Not owned", "small dim", false);
            gtk_label_set_xalign(GTK_LABEL(note), 0.5f);
            append(note);
        }
    } else if (f.installed) {
        remove_button("fighter:" + f.name);
    } else if (owned == false) {
        append(ui::box(GTK_ORIENTATION_HORIZONTAL, 0)); // (keeps the columns)
    } else {
        add_button("Add");
    }

    GtkWidget* list_row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(list_row), FALSE);
    gtk_widget_set_focusable(list_row, FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
    return list_row;
}

void Launcher::show_characters() {
    GtkWidget* card = characters_card_;
    const ui::Rebuild rebuild(card);
    const Distro distro = this_distro();
    if (importer_.empty()) {
        gtk_box_append(GTK_BOX(card), ui::label("The character importer is missing from this launcher.", "error-text"));
        return;
    }
    if (python_.empty()) {
        gtk_box_append(GTK_BOX(card), ui::label("Python 3 is needed to add characters.", "error-text"));
        gtk_box_append(GTK_BOX(card), install_hint(distro, {Package::kPython, Package::kNumpy, Package::kPillow},
                                                   "Python 3 with numpy and Pillow",
                                                   "Install it with your distribution's packages."));
        return;
    }
    if (!status_) {
        GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 12);
        if (status_loading_) {
            GtkWidget* spinner = gtk_spinner_new();
            gtk_spinner_start(GTK_SPINNER(spinner));
            gtk_box_append(GTK_BOX(row), spinner);
            gtk_box_append(GTK_BOX(row), ui::label("Looking for Tekken 7…", "dim"));
        } else {
            gtk_box_append(GTK_BOX(row), ui::label("The importer cannot tell what this PC has: " + status_error_,
                                                   "error-text"));
        }
        gtk_box_append(GTK_BOX(card), row);
        return;
    }
    const ImporterStatus& s = *status_;
    if (install_.state != InstallState::kInstalled) {
        GtkWidget* row = ui::box(GTK_ORIENTATION_HORIZONTAL, 10, "check-row");
        GtkWidget* image = gtk_image_new_from_icon_name("dialog-information-symbolic");
        gtk_widget_set_valign(image, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), image);
        GtkWidget* note = ui::label("Install the game first: the characters are made from it and from your Tekken 7.");
        gtk_widget_set_hexpand(note, TRUE);
        gtk_box_append(GTK_BOX(row), note);
        gtk_box_append(GTK_BOX(card), row);
    }

    // Tekken 7 and Steam.
    GtkWidget* facts = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(facts), 10);
    gtk_grid_set_column_spacing(GTK_GRID(facts), 18);
    gtk_grid_attach(GTK_GRID(facts), fact_name("Tekken 7"), 0, 0, 1, 1);
    GtkWidget* tekken7 = nullptr;
    if (s.tekken7_usable()) {
        tekken7 = ui::label(s.tekken7_path, "path", false);
        ui::set_tooltip(tekken7, s.tekken7_path);
    } else {
        std::string text = s.tekken7_found ? "Found, but it cannot be used" : "Not installed";
        if (!s.tekken7_error.empty()) text += ": " + s.tekken7_error;
        tekken7 = ui::label(text, "error-text");
    }
    gtk_widget_set_hexpand(tekken7, TRUE);
    gtk_grid_attach(GTK_GRID(facts), tekken7, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(facts), fact_name("Steam"), 0, 1, 1, 1);
    GtkWidget* steam = ui::box(GTK_ORIENTATION_VERTICAL, 3);
    if (!s.steam_installed) {
        gtk_box_append(GTK_BOX(steam), ui::label("Not installed: Tekken 7 for PC comes from Steam", "error-text"));
    } else if (s.steam_running) {
        gtk_box_append(GTK_BOX(steam), ui::label("Running", nullptr, false));
    } else {
        gtk_box_append(GTK_BOX(steam), ui::label("Not running", "warning-text", false));
        gtk_box_append(GTK_BOX(steam), ui::label("Start Steam to add characters: Tekken 7 needs it the first time, "
                                                 "and it tells which DLC you own.",
                                                 "small dim"));
    }
    gtk_grid_attach(GTK_GRID(facts), steam, 1, 1, 1, 1);

    // What is missing to import, with the command that installs it.
    std::vector<std::string> missing, names; // (as listed, and as named in a sentence)
    std::vector<Package> packages;
    if (!s.numpy || !s.pillow) {
        missing.push_back(!s.numpy && !s.pillow ? "numpy and Pillow for Python"
                          : !s.numpy            ? "numpy for Python"
                                                : "Pillow for Python");
        names.push_back(missing.back());
        if (!s.numpy) packages.push_back(Package::kNumpy);
        if (!s.pillow) packages.push_back(Package::kPillow);
    }
    if (s.gamescope.empty()) {
        missing.push_back("gamescope (runs Tekken 7 hidden)");
        names.emplace_back("gamescope");
        packages.push_back(Package::kGamescope);
    }
    if (s.pactl.empty()) {
        missing.push_back("pactl (a silent sound output for it)");
        names.emplace_back("pactl");
        packages.push_back(Package::kPactl);
    }
    if (s.helper.empty()) {
        missing.push_back("MinGW (builds a small helper for Tekken 7)");
        names.emplace_back("MinGW");
        packages.push_back(Package::kMingw);
    }
    int row = 2;
    if (!s.game_error.empty() && install_.state == InstallState::kInstalled) {
        gtk_grid_attach(GTK_GRID(facts), fact_name("Game"), 0, row, 1, 1);
        GtkWidget* game = ui::label("The importer cannot read it: " + s.game_error, "error-text");
        gtk_grid_attach(GTK_GRID(facts), game, 1, row++, 1, 1);
    }
    if (!missing.empty()) {
        gtk_grid_attach(GTK_GRID(facts), fact_name("Missing"), 0, row, 1, 1);
        GtkWidget* needed = ui::box(GTK_ORIENTATION_VERTICAL, 6);
        std::string text;
        for (size_t i = 0; i < missing.size(); ++i) text += (i ? "\n" : "") + missing[i];
        gtk_box_append(GTK_BOX(needed), ui::label(text, "warning-text"));
        std::string what;
        for (size_t i = 0; i < names.size(); ++i) {
            what += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
        }
        gtk_box_append(GTK_BOX(needed),
                       install_hint(distro, packages, what, "Install them with your distribution's packages."));
        gtk_grid_attach(GTK_GRID(facts), needed, 1, row, 1, 1);
    }
    gtk_box_append(GTK_BOX(card), facts);
    const bool usable = can_import();

    // Tekken 7's own characters.
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 4);
    gtk_box_append(GTK_BOX(card), separator);
    GtkWidget* head = ui::box(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* head_text = ui::box(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(head_text, TRUE);
    gtk_box_append(GTK_BOX(head_text), ui::label("Tekken 7's characters", "heading"));
    gtk_box_append(GTK_BOX(head_text),
                   ui::label("They join the roster beside the game's own fighters. Each takes about a minute; the "
                             "first time, Tekken 7 runs hidden in the background to save their moves.",
                             "small dim"));
    gtk_box_append(GTK_BOX(head), head_text);
    const auto base = base_to_add();
    GtkWidget* add_all = ui::button("Add All");
    ui::set_id(add_all, "add-all");
    gtk_widget_set_sensitive(add_all, usable && !task_ && !base.empty());
    clicked(add_all, [](Launcher* self) { self->start_import(self->base_to_add()); });
    gtk_box_append(GTK_BOX(head), add_all);
    gtk_box_append(GTK_BOX(card), head);

    // Packs an older version made, which the game skips, of characters that
    // can be added again (base and DLC alike: the DLC's list may be closed;
    // a DLC not owned only says so in its row); and a newer version's, which
    // it skips too until it is updated.
    std::vector<const Fighter*> outdated, newer;
    for (const Fighter& f : s.fighters) {
        if (f.outdated && owns(f) != false) outdated.push_back(&f);
        if (f.newer) newer.push_back(&f);
    }
    const auto warn_line = [&](const std::vector<const Fighter*>& who, const char* made, const char* todo) {
        if (who.empty()) return;
        GtkWidget* line = ui::box(GTK_ORIENTATION_HORIZONTAL, 10, "check-row");
        GtkWidget* image = gtk_image_new_from_icon_name("dialog-warning-symbolic");
        gtk_widget_add_css_class(image, "warn");
        gtk_widget_set_valign(image, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(line), image);
        const std::string text = who.size() == 1
                                     ? who[0]->display + " was made by " + made + " and is not in the game: "
                                     : std::to_string(who.size()) + " characters were made by " + made +
                                           " and are not in the game: ";
        GtkWidget* note = ui::label(text + todo);
        gtk_widget_set_hexpand(note, TRUE);
        gtk_box_append(GTK_BOX(line), note);
        gtk_box_append(GTK_BOX(card), line);
    };
    warn_line(outdated, "an older version", "add them again.");
    warn_line(newer, "a newer version of Ultimate Launcher", "update it, then the game, to play them.");

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "fighter-list");
    int dlc_count = 0, owned_count = 0, dlc_outdated = 0;
    for (const Fighter& f : s.fighters) {
        if (f.dlc != 0) {
            ++dlc_count;
            owned_count += owns(f) == true;
            dlc_outdated += f.outdated && owns(f) != false;
            continue;
        }
        gtk_list_box_append(GTK_LIST_BOX(list), fighter_row(f, usable));
    }
    gtk_box_append(GTK_BOX(card), list);
    if (dlc_count == 0) return;

    // The DLC's, in a list that opens.
    GtkWidget* title = ui::box(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(title), ui::label("DLC characters", "heading", false));
    const std::string summary =
        owned_answer_ ? std::to_string(owned_count) + " of " + std::to_string(dlc_count) + " owned"
                      : std::to_string(dlc_count);
    gtk_box_append(GTK_BOX(title), ui::label(summary, "pill neutral", false));
    if (dlc_outdated > 0) {
        gtk_box_append(GTK_BOX(title), ui::label(std::to_string(dlc_outdated) + " to add again", "pill warn", false));
    }
    GtkWidget* expander = gtk_expander_new(nullptr);
    ui::set_id(expander, "dlc");
    gtk_expander_set_label_widget(GTK_EXPANDER(expander), title);
    gtk_expander_set_expanded(GTK_EXPANDER(expander), dlc_expanded_);
    gtk_widget_set_margin_top(expander, 6);
    g_signal_connect(expander, "notify::expanded", G_CALLBACK(+[](GObject* e, GParamSpec*, gpointer data) {
                         static_cast<Launcher*>(data)->dlc_expanded_ = gtk_expander_get_expanded(GTK_EXPANDER(e));
                     }),
                     this);
    GtkWidget* inside = ui::box(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(inside, 12);
    GtkWidget* owned_row = ui::box(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* note = nullptr;
    // (Steam's answer kept from before is shown at once: asking again takes
    // most of a minute after a boot.)
    const std::string owns_text = std::to_string(owned_count) + " of them";
    if (owned_state_ == Owned::kChecking) {
        note = ui::box(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_box_append(GTK_BOX(note), spinner);
        gtk_box_append(GTK_BOX(note), ui::label(owned_answer_ ? "Asking Steam again which DLC you own (last time: " +
                                                                    owns_text + ")…"
                                                              : "Asking Steam which DLC you own…",
                                                "small dim"));
    } else if (owned_state_ == Owned::kKnown) {
        note = ui::label("Your Steam account owns " + owns_text + ": only those can be added.", "small dim");
    } else if (owned_state_ == Owned::kFailed) {
        note = ui::label(owned_answer_ ? "Steam could not be asked again (" + owned_error_ +
                                             "). It said last time that your account owns " + owns_text + "."
                                       : "Which DLC you own is not known: " + owned_error_,
                         "small error-text");
    } else if (owned_answer_) {
        note = ui::label(s.steam_running ? "Your Steam account owned " + owns_text + " last time Steam was asked."
                                         : "Your Steam account owned " + owns_text +
                                               " last time Steam was asked. Start Steam to add them: it checks again.",
                         "small dim");
    } else if (!s.steam_installed) {
        note = ui::label("Which DLC you own is not known: Steam, which tells it, is not installed.", "small dim");
    } else if (!s.tekken7_usable()) {
        // (Steam is asked as the game asks it, in Tekken 7's Proton prefix:
        // never while Tekken 7 cannot be used, so "not known yet" would wait
        // for nothing.)
        note = ui::label(std::string("Which DLC you own is not known: Steam is asked through Tekken 7, which ") +
                             (s.tekken7_found ? "cannot be used (see above)." : "is not installed."),
                         "small dim");
    } else {
        note = ui::label(s.steam_running ? "Which DLC you own is not known yet."
                                         : "Start Steam to see which DLC you own: only those can be added.",
                         "small dim");
    }
    gtk_widget_set_hexpand(note, TRUE);
    gtk_widget_set_valign(note, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(owned_row), note);
    const auto owned = owned_to_add();
    GtkWidget* add_owned = ui::button("Add All Owned");
    ui::set_id(add_owned, "add-all-owned");
    gtk_widget_set_sensitive(add_owned, usable && !task_ && !owned.empty());
    clicked(add_owned, [](Launcher* self) { self->start_import(self->owned_to_add()); });
    gtk_box_append(GTK_BOX(owned_row), add_owned);
    gtk_box_append(GTK_BOX(inside), owned_row);
    GtkWidget* dlc_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dlc_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(dlc_list, "fighter-list");
    for (const Fighter& f : s.fighters) {
        if (f.dlc != 0) gtk_list_box_append(GTK_LIST_BOX(dlc_list), fighter_row(f, usable));
    }
    gtk_box_append(GTK_BOX(inside), dlc_list);
    gtk_expander_set_child(GTK_EXPANDER(expander), inside);
    gtk_box_append(GTK_BOX(card), expander);
}

// ---------------------------------------------------------------- tasks

void Launcher::start_import(const std::vector<std::string>& names) {
    std::string why;
    if (task_) return refuse("Another task is running", "Wait for it to end, then add characters.");
    if (!can_import(&why)) return refuse("Cannot add characters", why);
    std::vector<std::string> argv = importer_command("import_character.py");
    argv.insert(argv.end(), names.begin(), names.end());
    argv.insert(argv.end(), {"--game", install_.folder.string(), "--progress"});
    if (start_task(Kind::kImport, "Adding " + names_text(status_, names), argv, importer_environment(), nullptr)) {
        task_->subject = names_text(status_, names);
    }
}

void Launcher::start_remove(const std::vector<std::string>& names) {
    if (task_) return refuse("Another task is running", "Wait for it to end, then remove characters.");
    if (importer_.empty() || python_.empty() || install_.state != InstallState::kInstalled) {
        return refuse("Cannot remove characters", "The importer, Python 3 or the installed game is missing.");
    }
    std::vector<std::string> argv = importer_command("import_character.py");
    argv.push_back("--remove");
    argv.insert(argv.end(), names.begin(), names.end());
    argv.insert(argv.end(), {"--game", install_.folder.string(), "--progress"});
    if (start_task(Kind::kRemove, "Removing " + names_text(status_, names), argv, importer_environment(), nullptr)) {
        task_->subject = names_text(status_, names);
    }
}

} // namespace launcher
