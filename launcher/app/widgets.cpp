// The window's pieces (see widgets.h).

#include "widgets.h"

#include <algorithm>

namespace launcher::ui {
namespace {

// Colours come from the theme (its foreground, mixed in) wherever they can,
// so that a light theme still reads; only the banner's red and the states'
// colours are the launcher's own.
constexpr const char* kStyle = R"css(
.launcher-content { padding: 24px 24px 32px 24px; }

.banner {
    border-radius: 14px;
    padding: 22px 24px;
    background-image: linear-gradient(120deg, rgba(196, 22, 28, 0.55), rgba(196, 22, 28, 0.12) 55%,
                                      rgba(196, 22, 28, 0) 85%);
    border: 1px solid alpha(currentColor, 0.08);
}
.banner .game-title { font-size: 190%; font-weight: 800; }
.banner .game-edition { font-size: 92%; font-weight: 700; letter-spacing: 3px; opacity: 0.85; }
.banner .game-note { opacity: 0.7; }

.section-title { font-weight: 700; font-size: 108%; margin: 0 4px; }

.card {
    border-radius: 12px;
    padding: 18px 20px;
    background-color: alpha(currentColor, 0.045);
    border: 1px solid alpha(currentColor, 0.09);
}
.card.disabled-card { opacity: 0.6; }
.card-title { font-weight: 700; font-size: 115%; }
.dim { opacity: 0.68; }
.small { font-size: 92%; }
.path { font-family: monospace; font-size: 92%; opacity: 0.75; }
.error-text { color: #f66151; }
.warning-text { color: #f5c211; }

.pill { border-radius: 999px; padding: 3px 11px; font-size: 86%; font-weight: 700; }
.pill.ok { background-color: rgba(46, 194, 126, 0.18); color: #3ad08c; }
.pill.warn { background-color: rgba(229, 165, 10, 0.18); color: #f0b72f; }
.pill.bad { background-color: rgba(224, 27, 36, 0.18); color: #f66151; }
.pill.neutral { background-color: alpha(currentColor, 0.1); }

.check-row image.ok { color: #3ad08c; }
.check-row image.bad { color: #f66151; }
.check-row image.warn { color: #f0b72f; }

.facts > label.fact-name { opacity: 0.68; }

.fighter-list {
    border-radius: 10px;
    border: 1px solid alpha(currentColor, 0.09);
    background-color: alpha(currentColor, 0.03);
}
.fighter-list > row {
    padding: 6px 8px 6px 14px;
    background-color: transparent;
    border-bottom: 1px solid alpha(currentColor, 0.07);
}
.fighter-list > row > box { min-height: 36px; }
.fighter-list > row:last-child { border-bottom: none; }
.fighter-list > row:hover { background-color: alpha(currentColor, 0.03); }

.command {
    border-radius: 8px;
    padding: 2px 2px 2px 10px;
    background-color: alpha(black, 0.22);
}
.command label { font-family: monospace; font-size: 92%; }

.task-panel {
    padding: 14px 0 16px 0;
    background-color: alpha(currentColor, 0.05);
    border-top: 1px solid alpha(currentColor, 0.12);
}
.task-title { font-weight: 700; }
.task-panel progressbar trough, .task-panel progressbar progress { min-height: 8px; border-radius: 4px; }
textview.log { font-family: monospace; font-size: 88%; }
textview.log text { background-color: alpha(black, 0.25); }
.log-frame { border-radius: 8px; border: 1px solid alpha(currentColor, 0.1); }

button.suggested-action.play { padding-left: 22px; padding-right: 22px; font-weight: 700; }

.update-notice {
    border-radius: 12px;
    padding: 14px 16px 14px 18px;
    background-color: alpha(@theme_selected_bg_color, 0.14);
    border: 1px solid alpha(@theme_selected_bg_color, 0.5);
}
.update-notice > image { color: mix(@theme_selected_bg_color, currentColor, 0.3); }
.update-notice .notice-title { font-weight: 700; }
headerbar button.update-available { color: mix(@theme_selected_bg_color, currentColor, 0.25); }

.setting-list {
    border-radius: 10px;
    border: 1px solid alpha(currentColor, 0.09);
    background-color: alpha(currentColor, 0.03);
}
.setting-row { padding: 10px 8px 10px 12px; border-bottom: 1px solid alpha(currentColor, 0.07); }
.setting-row:last-child { border-bottom: none; }

.key-group-title { font-size: 86%; font-weight: 700; opacity: 0.62; margin: 0 4px 6px 4px; }
.key-list {
    border-radius: 10px;
    border: 1px solid alpha(currentColor, 0.09);
    background-color: alpha(currentColor, 0.03);
}
.key-row {
    padding: 5px 8px 5px 10px;
    border-bottom: 1px solid alpha(currentColor, 0.07);
    transition: background-color 400ms ease-out;
}
.key-row:first-child { border-radius: 9px 9px 0 0; }
.key-row:last-child { border-bottom: none; border-radius: 0 0 9px 9px; }
.key-row > box { min-height: 40px; }
.key-row.flash { background-color: alpha(@theme_selected_bg_color, 0.2); }
.key-note { font-size: 84%; }
.key-note.swapped-note { color: mix(@theme_selected_bg_color, currentColor, 0.25); }
button.keycap {
    min-width: 58px;
    padding: 3px 10px 1px 10px;
    border-radius: 7px;
    font-weight: 700;
    background-image: none;
    box-shadow: none;
    background-color: alpha(currentColor, 0.08);
    border: 1px solid alpha(currentColor, 0.17);
    border-bottom: 3px solid alpha(currentColor, 0.24);
}
button.keycap:hover { background-color: alpha(currentColor, 0.13); }
button.keycap:active { background-color: alpha(currentColor, 0.18); border-bottom-width: 1px; padding-top: 5px; }
button.keycap .key-waiting { font-size: 88%; font-weight: 400; }
button.keycap.picking {
    border-color: @theme_selected_bg_color;
    background-color: alpha(@theme_selected_bg_color, 0.16);
}
)css";

// ---------------------------------------------------------------- clamp

struct Clamp {
    GtkWidget parent_instance;
    GtkWidget* child;
    int width;
};
struct ClampClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(Clamp, launcher_clamp, GTK_TYPE_WIDGET)

int column_width(Clamp* self, int available) {
    int minimum = 0;
    gtk_widget_measure(self->child, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, nullptr, nullptr, nullptr);
    return std::max(minimum, std::min(available, self->width));
}

void clamp_measure(GtkWidget* widget, GtkOrientation orientation, int for_size, int* minimum, int* natural,
                   int* minimum_baseline, int* natural_baseline) {
    auto* self = reinterpret_cast<Clamp*>(widget);
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        gtk_widget_measure(self->child, orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
        *natural = std::max(*minimum, self->width);
    } else {
        const int width = for_size < 0 ? -1 : column_width(self, for_size);
        gtk_widget_measure(self->child, orientation, width, minimum, natural, minimum_baseline, natural_baseline);
    }
}

void clamp_allocate(GtkWidget* widget, int width, int height, int baseline) {
    auto* self = reinterpret_cast<Clamp*>(widget);
    const int column = std::min(width, column_width(self, width));
    graphene_point_t at;
    graphene_point_init(&at, static_cast<float>((width - column) / 2), 0.f);
    gtk_widget_allocate(self->child, column, height, baseline, gsk_transform_translate(nullptr, &at));
}

GtkSizeRequestMode clamp_request_mode(GtkWidget*) { return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH; }

void clamp_dispose(GObject* object) {
    auto* self = reinterpret_cast<Clamp*>(object);
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(launcher_clamp_parent_class)->dispose(object);
}

void launcher_clamp_class_init(ClampClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = clamp_dispose;
    GTK_WIDGET_CLASS(klass)->measure = clamp_measure;
    GTK_WIDGET_CLASS(klass)->size_allocate = clamp_allocate;
    GTK_WIDGET_CLASS(klass)->get_request_mode = clamp_request_mode;
}

void launcher_clamp_init(Clamp*) {}

// ---------------------------------------------------------------- columns

struct Columns {
    GtkWidget parent_instance;
    int spacing;
};
struct ColumnsClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(Columns, launcher_columns, GTK_TYPE_WIDGET)

// The widest child's width, at least or as it would like.
int widest(GtkWidget* widget, bool natural) {
    int most = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
        int minimum = 0, wanted = 0;
        gtk_widget_measure(c, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &wanted, nullptr, nullptr);
        most = std::max(most, natural ? wanted : minimum);
    }
    return most;
}

bool paired(Columns* self, int width) {
    return width >= 2 * widest(GTK_WIDGET(self), true) + self->spacing;
}

// Lays the children out in `width` (or only measures, `place` false): the
// height they take.
int columns_layout(Columns* self, int width, bool place, bool natural = true) {
    const bool pair = paired(self, width);
    const int column = pair ? (width - self->spacing) / 2 : width;
    int y = 0;
    GtkWidget* c = gtk_widget_get_first_child(GTK_WIDGET(self));
    while (c) {
        GtkWidget* row[2] = {c, pair ? gtk_widget_get_next_sibling(c) : nullptr};
        int height = 0;
        for (GtkWidget* w : row) {
            if (!w) continue;
            int minimum = 0, wanted = 0;
            gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, column, &minimum, &wanted, nullptr, nullptr);
            height = std::max(height, natural ? wanted : minimum);
        }
        for (int i = 0; i < 2 && place; ++i) {
            if (!row[i]) continue;
            graphene_point_t at;
            graphene_point_init(&at, static_cast<float>(i * (column + self->spacing)), static_cast<float>(y));
            gtk_widget_allocate(row[i], column, height, -1, gsk_transform_translate(nullptr, &at));
        }
        y += height;
        c = row[1] ? gtk_widget_get_next_sibling(row[1]) : gtk_widget_get_next_sibling(row[0]);
        if (c) y += self->spacing;
    }
    return y;
}

void columns_measure(GtkWidget* widget, GtkOrientation orientation, int for_size, int* minimum, int* natural,
                     int*, int*) {
    auto* self = reinterpret_cast<Columns*>(widget);
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = widest(widget, false);
        *natural = 2 * widest(widget, true) + self->spacing;
    } else {
        const int width = for_size < 0 ? 2 * widest(widget, true) + self->spacing : for_size;
        *minimum = columns_layout(self, width, false, false);
        *natural = columns_layout(self, width, false);
    }
}

void columns_allocate(GtkWidget* widget, int width, int, int) {
    columns_layout(reinterpret_cast<Columns*>(widget), width, true);
}

GtkSizeRequestMode columns_request_mode(GtkWidget*) { return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH; }

void columns_dispose(GObject* object) {
    while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(object))) gtk_widget_unparent(child);
    G_OBJECT_CLASS(launcher_columns_parent_class)->dispose(object);
}

void launcher_columns_class_init(ColumnsClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = columns_dispose;
    GTK_WIDGET_CLASS(klass)->measure = columns_measure;
    GTK_WIDGET_CLASS(klass)->size_allocate = columns_allocate;
    GTK_WIDGET_CLASS(klass)->get_request_mode = columns_request_mode;
}

void launcher_columns_init(Columns*) {}

void copied(GtkButton* button, gpointer) {
    const char* text = static_cast<const char*>(g_object_get_data(G_OBJECT(button), "text"));
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), "Copied");
}

constexpr const char* kId = "launcher-id";

} // namespace

std::string shown(const std::filesystem::path& path) {
    char* name = g_filename_display_name(path.c_str());
    std::string out = name;
    g_free(name);
    return out;
}

std::string valid(std::string_view text) {
    if (g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) return std::string(text);
    char* fixed = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
    std::string out = fixed;
    g_free(fixed);
    return out;
}

void set_text(GtkWidget* label, const std::string& text) { gtk_label_set_text(GTK_LABEL(label), valid(text).c_str()); }

void set_tooltip(GtkWidget* widget, const std::string& text) {
    gtk_widget_set_tooltip_text(widget, valid(text).c_str());
}

void set_id(GtkWidget* widget, const std::string& id) {
    g_object_set_data_full(G_OBJECT(widget), kId, g_strdup(id.c_str()), g_free);
}

GtkWidget* find_id(GtkWidget* container, const std::string& id) {
    const char* own = static_cast<const char*>(g_object_get_data(G_OBJECT(container), kId));
    if (own && id == own) return container;
    for (GtkWidget* c = gtk_widget_get_first_child(container); c; c = gtk_widget_get_next_sibling(c)) {
        if (GtkWidget* found = find_id(c, id)) return found;
    }
    return nullptr;
}

std::string focused_id(GtkWidget* container) {
    GtkRoot* root = gtk_widget_get_root(container);
    std::string id;
    for (GtkWidget* w = root ? gtk_root_get_focus(root) : nullptr; w; w = gtk_widget_get_parent(w)) {
        const char* own = static_cast<const char*>(g_object_get_data(G_OBJECT(w), kId));
        if (own && id.empty()) id = own;
        if (w == container) return id;
    }
    return {};
}

Rebuild::Rebuild(GtkWidget* box) : box_(box), focus_(focused_id(box)) { clear(box); }

Rebuild::~Rebuild() {
    // (Not when it cannot take the focus: one made insensitive by a task. A
    // row's second button gone, the row's own takes it: "remove:kazumi",
    // beside Add Again, then "fighter:kazumi", Add.)
    GtkWidget* widget = focus_.empty() ? nullptr : find_id(box_, focus_);
    if (!widget && focus_.rfind("remove:", 0) == 0) widget = find_id(box_, "fighter:" + focus_.substr(7));
    if (widget) gtk_widget_grab_focus(widget);
}

GtkWidget* clamp_new(GtkWidget* child, int width) {
    auto* self = static_cast<Clamp*>(g_object_new(launcher_clamp_get_type(), nullptr));
    self->child = child;
    self->width = width;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

GtkWidget* columns_new(int spacing) {
    auto* self = static_cast<Columns*>(g_object_new(launcher_columns_get_type(), nullptr));
    self->spacing = spacing;
    return GTK_WIDGET(self);
}

void columns_append(GtkWidget* columns, GtkWidget* child) { gtk_widget_set_parent(child, columns); }

void add_classes(GtkWidget* widget, const char* classes) {
    if (!classes) return;
    gchar** names = g_strsplit(classes, " ", -1);
    for (gchar** n = names; *n; ++n) {
        if (**n) gtk_widget_add_css_class(widget, *n);
    }
    g_strfreev(names);
}

GtkWidget* label(const std::string& text, const char* classes, bool wrap) {
    GtkWidget* l = gtk_label_new(valid(text).c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (wrap) {
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    } else {
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    }
    add_classes(l, classes);
    return l;
}

GtkWidget* button(const char* text, const char* classes, const char* icon) {
    GtkWidget* b = gtk_button_new();
    if (icon) {
        GtkWidget* content = box(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name(icon));
        if (text) gtk_box_append(GTK_BOX(content), gtk_label_new(text));
        gtk_button_set_child(GTK_BUTTON(b), content);
    } else {
        gtk_button_set_label(GTK_BUTTON(b), text);
    }
    add_classes(b, classes);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

GtkWidget* box(GtkOrientation orientation, int spacing, const char* classes) {
    GtkWidget* b = gtk_box_new(orientation, spacing);
    add_classes(b, classes);
    return b;
}

void clear(GtkWidget* container) {
    // (The window's focus first, when it is in there: GTK complains when the
    // focused widget is removed.)
    if (GtkRoot* root = gtk_widget_get_root(container)) {
        GtkWidget* focus = gtk_root_get_focus(root);
        if (focus && gtk_widget_is_ancestor(focus, container)) gtk_root_set_focus(root, nullptr);
    }
    while (GtkWidget* child = gtk_widget_get_first_child(container)) gtk_box_remove(GTK_BOX(container), child);
}

GtkWidget* command(const std::string& text) {
    GtkWidget* row = box(GTK_ORIENTATION_HORIZONTAL, 6, "command");
    GtkWidget* l = label(text);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD);
    // (Selectable with the mouse; not focused by the keyboard, which would
    // select it all: the copy button is the way to copy it.)
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_focusable(l, FALSE);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    GtkWidget* copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_add_css_class(copy, "flat");
    gtk_widget_set_tooltip_text(copy, "Copy");
    g_object_set_data_full(G_OBJECT(copy), "text", g_strdup(text.c_str()), g_free);
    g_signal_connect(copy, "clicked", G_CALLBACK(copied), nullptr);
    set_id(copy, "copy:" + text);
    gtk_box_append(GTK_BOX(row), copy);
    return row;
}

void load_style() {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, kStyle, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    // Dark, as a game's launcher, whatever the desktop's choice: the theme's
    // dark variant (GTK 4.20 deprecates the setting but still follows it;
    // its interface colour scheme does not choose the theme's variant).
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
}

bool save_png(GtkWidget* widget, const std::string& file, int scale, const GdkRGBA* background) {
    const int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    GtkNative* native = gtk_widget_get_native(widget);
    if (width <= 0 || height <= 0 || !native) return false;
    if (scale <= 0) scale = gdk_surface_get_scale_factor(gtk_native_get_surface(native));
    GdkPaintable* paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gtk_snapshot_scale(snapshot, static_cast<float>(scale), static_cast<float>(scale));
    if (background) {
        graphene_rect_t all;
        graphene_rect_init(&all, 0, 0, static_cast<float>(width), static_cast<float>(height));
        gtk_snapshot_append_color(snapshot, background, &all);
    }
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    if (!node) return false;
    graphene_rect_t viewport;
    graphene_rect_init(&viewport, 0, 0, static_cast<float>(width * scale), static_cast<float>(height * scale));
    GdkTexture* texture = gsk_renderer_render_texture(gtk_native_get_renderer(native), node, &viewport);
    if (!texture) {
        // (Not every backend's renderer renders into a texture: Broadway's.)
        GskRenderer* cairo = gsk_cairo_renderer_new();
        if (gsk_renderer_realize(cairo, gtk_native_get_surface(native), nullptr)) {
            texture = gsk_renderer_render_texture(cairo, node, &viewport);
            gsk_renderer_unrealize(cairo);
        }
        g_object_unref(cairo);
    }
    gsk_render_node_unref(node);
    const bool ok = texture && gdk_texture_save_to_png(texture, file.c_str());
    if (texture) g_object_unref(texture);
    return ok;
}

} // namespace launcher::ui
