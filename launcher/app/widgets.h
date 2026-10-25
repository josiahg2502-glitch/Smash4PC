#pragma once

// Small pieces the launcher's window is made of, over plain GTK 4: a column
// of limited width, labels and buttons set up as the window uses them, and
// the launcher's style.

#include <gtk/gtk.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace launcher::ui {

// Text for GTK, which takes UTF-8 only: a path as GLib shows file names
// (its bytes may be in another encoding), any other text with what is not
// UTF-8 replaced. Labels, tooltips and the log make their text valid
// themselves; a path is shown with shown().
std::string shown(const std::filesystem::path& path);
std::string valid(std::string_view text);
void set_text(GtkWidget* label, const std::string& text);
void set_tooltip(GtkWidget* widget, const std::string& text);

// A column at most `width` pixels wide, centred in the space it gets.
GtkWidget* clamp_new(GtkWidget* child, int width);

// Its children in two columns, side by side in pairs, when there is room
// for both at their natural width; else in one, one under the other.
GtkWidget* columns_new(int spacing);
void columns_append(GtkWidget* columns, GtkWidget* child);

// A label that wraps (or ellipsizes, `wrap` false), left-aligned, with
// these style classes (space-separated).
GtkWidget* label(const std::string& text, const char* classes = nullptr, bool wrap = true);
GtkWidget* button(const char* text, const char* classes = nullptr, const char* icon = nullptr);
GtkWidget* box(GtkOrientation orientation, int spacing, const char* classes = nullptr);
void add_classes(GtkWidget* widget, const char* classes);
void clear(GtkWidget* box);

// The keyboard focus kept over a rebuild of a box (a card): widgets are
// named by an id ("play", "fighter:kazumi"), and the one of the same id in
// the new content takes the focus when the old one had it, so that the
// keyboard's place is not lost each time the state is shown again.
void set_id(GtkWidget* widget, const std::string& id);
GtkWidget* find_id(GtkWidget* container, const std::string& id);
// The id of the focused widget (or of its nearest parent with one) when the
// focus is in `container`; empty otherwise.
std::string focused_id(GtkWidget* container);
class Rebuild {
public:
    explicit Rebuild(GtkWidget* box); // clears it, remembering what had the focus
    ~Rebuild();                       // gives the focus to its new widget of that id
    Rebuild(const Rebuild&) = delete;
    Rebuild& operator=(const Rebuild&) = delete;

private:
    GtkWidget* box_;
    std::string focus_;
};

// A text the user can copy: shown in a monospace field with a copy button
// (a command that installs what is missing).
GtkWidget* command(const std::string& text);

// The launcher's style sheet, and the dark style the launcher prefers.
void load_style();

// A widget's picture as a PNG, at its window's scale or `scale` (on
// `background`, when given, for a widget that draws none).
bool save_png(GtkWidget* widget, const std::string& file, int scale = 0, const GdkRGBA* background = nullptr);

} // namespace launcher::ui
