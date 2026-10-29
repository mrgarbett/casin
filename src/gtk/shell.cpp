// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell.h"

#include "window.h"

#include <algorithm>
#include <map>

namespace oc::gtk {

namespace {

// Extensions Restricto opens; the folder browser lists only these.
const std::set<std::string> kSequenceExtensions = {"gb",  "gbk", "genbank", "gbff", "ape", "dna", "fasta",
                                                   "fa",  "fna", "fas",     "seq",  "embl", "ab1"};

// Folders open in the browser's tree, shared by every window.
std::set<std::string> g_expanded;

std::string settingsPath()
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "restricto", "settings.ini", nullptr);
    std::string out = path;
    g_free(path);
    return out;
}

GKeyFile *loadSettings()
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, settingsPath().c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    return kf;
}

void saveSettings(GKeyFile *kf)
{
    const std::string path = settingsPath();
    gchar *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
}

std::string lowerExtension(const std::string &name)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = name.substr(dot + 1);
    for (char &c : ext)
        c = char(g_ascii_tolower(c));
    return ext;
}

// Folders first, then files, each in natural order; hidden entries left out.
std::vector<std::pair<std::string, bool>> listFolder(const std::string &folder)
{
    std::vector<std::pair<std::string, bool>> out;
    GDir *dir = g_dir_open(folder.c_str(), 0, nullptr);
    if (!dir)
        return out;
    while (const char *name = g_dir_read_name(dir)) {
        if (name[0] == '.')
            continue;
        gchar *path = g_build_filename(folder.c_str(), name, nullptr);
        const bool isDir = g_file_test(path, G_FILE_TEST_IS_DIR);
        // A link to a folder can lead out of the chosen folder (flatpak-builder
        // leaves var/run -> /run) or round in a loop: leave those out.
        if (isDir && g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
            g_free(path);
            continue;
        }
        if (isDir || kSequenceExtensions.count(lowerExtension(name)))
            out.emplace_back(path, isDir);
        g_free(path);
    }
    g_dir_close(dir);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second)
            return a.second;
        gchar *ka = g_utf8_collate_key_for_filename(a.first.c_str(), -1);
        gchar *kb = g_utf8_collate_key_for_filename(b.first.c_str(), -1);
        const bool less = std::string(ka) < std::string(kb);
        g_free(ka);
        g_free(kb);
        return less;
    });
    return out;
}

// The sequence files under `folder` whose name contains `query`, at most 500,
// looking in at most 5,000 folders. Runs off the main thread.
void findFiles(const std::string &folder, const std::string &query, std::vector<std::string> *found,
               int *folders, int depth = 0)
{
    if (depth > 12 || found->size() >= 500 || ++*folders > 5000)
        return;
    for (const auto &[path, isDir] : listFolder(folder)) {
        if (isDir) {
            findFiles(path, query, found, folders, depth + 1);
            continue;
        }
        gchar *base = g_path_get_basename(path.c_str());
        gchar *name = g_utf8_casefold(base, -1);
        if (std::string(name).find(query) != std::string::npos)
            found->push_back(path);
        g_free(name);
        g_free(base);
        if (found->size() >= 500)
            return;
    }
}

std::string baseNameOf(const std::string &path)
{
    gchar *b = g_path_get_basename(path.c_str());
    std::string out = b;
    g_free(b);
    return out;
}

GtkWidget *redDot()
{
    GtkWidget *dot = gtk_label_new("●");
    gtk_widget_add_css_class(dot, "restricto-dot");
    gtk_widget_set_tooltip_text(dot, "Unsaved changes");
    return dot;
}

} // namespace

// ---- settings ----------------------------------------------------------------------

bool settingBool(const char *group, const char *key, bool fallback)
{
    GKeyFile *kf = loadSettings();
    GError *err = nullptr;
    const gboolean v = g_key_file_get_boolean(kf, group, key, &err);
    g_key_file_free(kf);
    if (err) {
        g_error_free(err);
        return fallback;
    }
    return v;
}

void setSettingBool(const char *group, const char *key, bool value)
{
    GKeyFile *kf = loadSettings();
    g_key_file_set_boolean(kf, group, key, value);
    saveSettings(kf);
    g_key_file_free(kf);
}

std::string settingString(const char *group, const char *key)
{
    GKeyFile *kf = loadSettings();
    gchar *v = g_key_file_get_string(kf, group, key, nullptr);
    std::string out = v ? v : "";
    g_free(v);
    g_key_file_free(kf);
    return out;
}

void setSettingString(const char *group, const char *key, const std::string &value)
{
    GKeyFile *kf = loadSettings();
    g_key_file_set_string(kf, group, key, value.c_str());
    saveSettings(kf);
    g_key_file_free(kf);
}

// ---- styles ------------------------------------------------------------------------

void Shell::loadStyles()
{
    // Floating capsules over the map and sequence (the Mac app's glass pills),
    // the red unsaved dot, and toggles tinted with the accent colour when on.
    static const char *css = R"(
.restricto-capsule {
  background-color: alpha(@headerbar_bg_color, 0.94);
  border-radius: 999px;
  box-shadow: 0 1px 4px alpha(black, 0.18), 0 0 0 1px alpha(black, 0.07);
  padding: 3px 8px;
}
.restricto-capsule.vertical { padding: 8px 3px; }
.restricto-capsule button { min-width: 26px; min-height: 24px; padding: 0 2px; }
.restricto-capsule button:checked, .restricto-capsule button.on,
.restricto-toggle:checked { color: @accent_color; background: none; }
.restricto-capsule button:checked:hover, .restricto-toggle:checked:hover { background: alpha(currentColor, 0.07); }
.restricto-capsule button.dim { color: alpha(currentColor, 0.45); }
.restricto-badge {
  background-color: alpha(@popover_bg_color, 0.96);
  border-radius: 6px;
  box-shadow: 0 1px 4px alpha(black, 0.25);
  padding: 3px 8px;
  font-feature-settings: "tnum";
  font-size: smaller;
  font-weight: 500;
}
.restricto-dot { color: @error_color; font-size: 7pt; }
.restricto-title { font-weight: bold; }
.restricto-subtitle { font-size: smaller; opacity: 0.55; }
.restricto-browser-header { font-size: smaller; font-weight: bold; opacity: 0.55; margin-top: 6px; }
)";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// ---- shell -------------------------------------------------------------------------

Shell *Shell::create(GtkApplication *app)
{
    return new Shell(app);   // owned by its GtkWindow
}

Shell *Shell::fromWindow(GtkWindow *w)
{
    return w ? static_cast<Shell *>(g_object_get_data(G_OBJECT(w), "oc-shell")) : nullptr;
}

Shell *Shell::active(GtkApplication *app)
{
    Shell *s = fromWindow(gtk_application_get_active_window(app));
    if (s && !s->m_closing)
        return s;
    for (Shell *other : all(app))
        if (!other->m_closing)
            return other;
    return nullptr;
}

std::vector<Shell *> Shell::all(GtkApplication *app)
{
    std::vector<Shell *> out;
    for (GList *l = gtk_application_get_windows(app); l; l = l->next)
        if (Shell *s = fromWindow(GTK_WINDOW(l->data)))
            out.push_back(s);
    return out;
}

Shell::Shell(GtkApplication *app)
{
    m_window = adw_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1320, 820);
    gtk_window_set_title(GTK_WINDOW(m_window), "Restricto");
    g_object_set_data_full(G_OBJECT(m_window), "oc-shell", this, [](gpointer p) { delete static_cast<Shell *>(p); });
    Window::addActions(G_ACTION_MAP(m_window), this);
    buildUi();
    g_signal_connect(m_window, "close-request", G_CALLBACK(&Shell::onCloseRequest), this);
}

void Shell::buildUi()
{
    // Header: sidebar and actions at the start, the document's title in the
    // middle, view toggles and the menu at the end (as the Mac toolbar).
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *sidebar = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebar), "sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(sidebar, "Show or hide the folder sidebar (F9)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(sidebar), "win.sidebar");
    gtk_widget_add_css_class(sidebar, "restricto-toggle");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebar);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(actions, "linked");
    struct Btn { const char *icon, *tip, *action; };
    for (const Btn &b : {Btn{"edit-undo-symbolic", "Undo the last edit (Ctrl+Z)", "win.undo"},
                         Btn{"edit-redo-symbolic", "Redo (Shift+Ctrl+Z)", "win.redo"},
                         Btn{"document-save-symbolic", "Save (Ctrl+S)", "win.save"}}) {
        GtkWidget *button = gtk_button_new_from_icon_name(b.icon);
        gtk_widget_set_tooltip_text(button, b.tip);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(button), b.action);
        gtk_box_append(GTK_BOX(actions), button);
    }
    GMenu *exportMenu = g_menu_new();
    g_menu_append(exportMenu, "Map Image (PNG, SVG or PDF)…", "win.export-map");
    g_menu_append(exportMenu, "GenBank…", "win.export-genbank");
    g_menu_append(exportMenu, "FASTA…", "win.export-fasta");
    GtkWidget *exportButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(exportButton), "document-send-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(exportButton), G_MENU_MODEL(exportMenu));
    gtk_widget_set_tooltip_text(exportButton, "Export the map, or the sequence as GenBank or FASTA");
    g_object_unref(exportMenu);
    gtk_box_append(GTK_BOX(actions), exportButton);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), actions);

    GtkWidget *titleBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(titleBox, GTK_ALIGN_CENTER);
    GtkWidget *titleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(titleRow, GTK_ALIGN_CENTER);
    m_titleDot = redDot();
    m_title = gtk_label_new("Restricto");
    gtk_label_set_ellipsize(GTK_LABEL(m_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(m_title, "restricto-title");
    gtk_box_append(GTK_BOX(titleRow), m_titleDot);
    gtk_box_append(GTK_BOX(titleRow), m_title);
    m_subtitle = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(m_subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(m_subtitle, "restricto-subtitle");
    gtk_box_append(GTK_BOX(titleBox), titleRow);
    gtk_box_append(GTK_BOX(titleBox), m_subtitle);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), titleBox);

    m_menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_menuButton), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(m_menuButton), TRUE);
    gtk_widget_set_tooltip_text(m_menuButton, "Main menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_menuButton);
    for (const Btn &b : {Btn{"sidebar-show-right-symbolic", "Show or hide the right panel (Ctrl+Alt+I)", "win.show-panel"},
                         Btn{"view-dual-symbolic", "Stack the map above the sequence, or put it beside", "win.stack-map"},
                         Btn{"media-record-symbolic", "Show or hide the map", "win.show-map"}}) {
        GtkWidget *t = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(t), b.icon);
        gtk_widget_set_tooltip_text(t, b.tip);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(t), b.action);
        gtk_widget_add_css_class(t, "restricto-toggle");
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), t);
    }
    rebuildMenu();

    // Tabs, with the bar shown from two documents on.
    m_tabs = GTK_WIDGET(adw_tab_view_new());
    GtkWidget *tabBar = GTK_WIDGET(adw_tab_bar_new());
    adw_tab_bar_set_view(ADW_TAB_BAR(tabBar), ADW_TAB_VIEW(m_tabs));
    adw_tab_bar_set_autohide(ADW_TAB_BAR(tabBar), TRUE);
    g_signal_connect(m_tabs, "notify::selected-page", G_CALLBACK(&Shell::onSelectedPage), this);
    g_signal_connect(m_tabs, "notify::n-pages", G_CALLBACK(&Shell::onPagesChanged), this);
    g_signal_connect(m_tabs, "close-page", G_CALLBACK(&Shell::onClosePage), this);
    g_signal_connect(m_tabs, "page-attached", G_CALLBACK(&Shell::onPageAttached), this);

    m_split = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(m_split), buildBrowser());
    // The tab bar sits over the documents only, so the sidebar doesn't move
    // when it appears.
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(m_tabs, TRUE);
    gtk_box_append(GTK_BOX(content), tabBar);
    gtk_box_append(GTK_BOX(content), m_tabs);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(m_split), content);
    adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(m_split), 220);
    adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(m_split), 320);
    setSidebarShown(settingBool("Layout", "sidebar", true));

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), m_split);
    m_toasts = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(m_toasts), toolbar);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(m_window), m_toasts);

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(&Shell::onDrop), this);
    gtk_widget_add_controller(m_window, GTK_EVENT_CONTROLLER(drop));
    // Pick up files saved or added elsewhere when the window comes back.
    g_signal_connect_swapped(m_window, "notify::is-active", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<Shell *>(p);
                                 if (gtk_window_is_active(self->gtkWindow()))
                                     self->reloadBrowser();
                             }),
                             this);
}

Window *Shell::current() const
{
    AdwTabPage *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(m_tabs));
    if (!page)
        return nullptr;
    return static_cast<Window *>(g_object_get_data(G_OBJECT(adw_tab_page_get_child(page)), "oc-page"));
}

std::vector<Window *> Shell::pages() const
{
    std::vector<Window *> out;
    const int n = adw_tab_view_get_n_pages(ADW_TAB_VIEW(m_tabs));
    for (int i = 0; i < n; ++i) {
        AdwTabPage *page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(m_tabs), i);
        if (auto *w = static_cast<Window *>(g_object_get_data(G_OBJECT(adw_tab_page_get_child(page)), "oc-page")))
            out.push_back(w);
    }
    return out;
}

void Shell::addPage(Window *w)
{
    AdwTabPage *page = adw_tab_view_append(ADW_TAB_VIEW(m_tabs), w->widget());
    adw_tab_view_set_selected_page(ADW_TAB_VIEW(m_tabs), page);
    setTabTitle(w);
}

void Shell::selectPage(Window *w)
{
    AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(m_tabs), w->widget());
    if (page)
        adw_tab_view_set_selected_page(ADW_TAB_VIEW(m_tabs), page);
}

void Shell::closePage(Window *w)
{
    AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(m_tabs), w->widget());
    if (page)
        adw_tab_view_close_page(ADW_TAB_VIEW(m_tabs), page);
}

void Shell::toast(const std::string &text)
{
    AdwToast *t = adw_toast_new(text.c_str());
    adw_toast_set_timeout(t, 3);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(m_toasts), t);
}

void Shell::setTabTitle(Window *w)
{
    if (!gtk_widget_get_parent(w->widget()))
        return;   // not added yet
    AdwTabPage *page = adw_tab_view_get_page(ADW_TAB_VIEW(m_tabs), w->widget());
    if (!page)
        return;
    // A tab can't colour part of its title, so the unsaved mark is a bullet.
    const std::string name = w->displayName();
    const std::string title = w->document().isModified() ? "• " + name : name;
    adw_tab_page_set_title(page, title.c_str());
    adw_tab_page_set_tooltip(page, w->document().path().empty() ? name.c_str() : w->document().path().c_str());
}

void Shell::refreshChrome()
{
    Window *w = current();
    if (!w)
        return;
    const std::string name = w->displayName();
    gtk_label_set_text(GTK_LABEL(m_title), w->isEmpty() ? "Restricto" : name.c_str());
    gtk_label_set_text(GTK_LABEL(m_subtitle), w->subtitle().c_str());
    gtk_widget_set_visible(m_titleDot, !w->isEmpty() && w->document().isModified());
    gtk_window_set_title(gtkWindow(), w->isEmpty() ? "Restricto" : (name + " — Restricto").c_str());
    setTabTitle(w);
}

bool Shell::sidebarShown() const
{
    return adw_overlay_split_view_get_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(m_split));
}

void Shell::setSidebarShown(bool shown)
{
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(m_split), shown);
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(m_window), "sidebar");
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(shown));
    if (shown)
        reloadBrowser();
}

void Shell::onSelectedPage(GObject *, GParamSpec *, gpointer p)
{
    auto *self = static_cast<Shell *>(p);
    if (Window *w = self->current()) {
        w->updateActions();
        self->refreshChrome();
        self->reloadBrowser();
    }
}

gboolean Shell::onClosePage(AdwTabView *view, AdwTabPage *page, gpointer)
{
    auto *w = static_cast<Window *>(g_object_get_data(G_OBJECT(adw_tab_page_get_child(page)), "oc-page"));
    // Asks first when there are unsaved changes, and closes the tab later.
    adw_tab_view_close_page_finish(view, page, !w || w->confirmClose(false));
    return GDK_EVENT_STOP;
}

void Shell::onPageAttached(AdwTabView *, AdwTabPage *page, int, gpointer p)
{
    // A tab dragged here from another window now belongs to this one.
    auto *self = static_cast<Shell *>(p);
    if (auto *w = static_cast<Window *>(g_object_get_data(G_OBJECT(adw_tab_page_get_child(page)), "oc-page"))) {
        w->m_shell = self;
        w->m_window = self->m_window;
    }
}

void Shell::onPagesChanged(GObject *, GParamSpec *, gpointer p)
{
    auto *self = static_cast<Shell *>(p);
    if (self->m_closing)
        return;
    // Closing the last tab closes the window.
    if (adw_tab_view_get_n_pages(ADW_TAB_VIEW(self->m_tabs)) == 0) {
        self->m_closing = true;
        gtk_window_destroy(self->gtkWindow());
        return;
    }
    documentsChanged(self->app());
}

gboolean Shell::onCloseRequest(GtkWindow *, gpointer p)
{
    auto *self = static_cast<Shell *>(p);
    for (Window *w : self->pages())
        if (!w->confirmClose(true))
            return TRUE;   // asking; the answer closes the window again
    self->m_closing = true;
    return FALSE;
}

gboolean Shell::onDrop(GtkDropTarget *, const GValue *value, double, double, gpointer p)
{
    auto *self = static_cast<Shell *>(p);
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    std::vector<std::string> paths;
    for (GSList *l = static_cast<GSList *>(g_value_get_boxed(value)); l; l = l->next) {
        char *path = g_file_get_path(G_FILE(l->data));
        if (path) {
            paths.push_back(path);
            g_free(path);
        }
    }
    if (paths.empty())
        return FALSE;
    openFiles(self->app(), paths, self->current());
    return TRUE;
}

// ---- folder browser ------------------------------------------------------------------

GtkWidget *Shell::buildBrowser()
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box, "background");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(top, 12);
    gtk_widget_set_margin_end(top, 6);
    gtk_widget_set_margin_top(top, 8);
    m_folderLabel = gtk_label_new("No Folder");
    gtk_label_set_xalign(GTK_LABEL(m_folderLabel), 0);
    gtk_label_set_ellipsize(GTK_LABEL(m_folderLabel), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(m_folderLabel, "heading");
    gtk_widget_set_hexpand(m_folderLabel, TRUE);
    GtkWidget *choose = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_add_css_class(choose, "flat");
    gtk_widget_set_tooltip_text(choose, "Choose Folder…");
    g_signal_connect_swapped(choose, "clicked", G_CALLBACK(+[](gpointer p) { static_cast<Shell *>(p)->chooseFolder(); }),
                             this);
    gtk_box_append(GTK_BOX(top), m_folderLabel);
    gtk_box_append(GTK_BOX(top), choose);

    m_fileSearch = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_fileSearch), "Search files");
    gtk_widget_set_margin_start(m_fileSearch, 8);
    gtk_widget_set_margin_end(m_fileSearch, 8);
    g_signal_connect_swapped(m_fileSearch, "search-changed",
                             G_CALLBACK(+[](gpointer p) { static_cast<Shell *>(p)->reloadBrowser(); }), this);

    m_fileList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_fileList), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(m_fileList, "navigation-sidebar");
    g_signal_connect(m_fileList, "row-activated", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         static_cast<Shell *>(p)->browserRowActivated(row);
                     })),
                     this);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_fileList);
    gtk_widget_set_vexpand(scroll, TRUE);

    m_noFolder = gtk_label_new("Choose a folder to list its sequence files here.");
    gtk_label_set_wrap(GTK_LABEL(m_noFolder), TRUE);
    gtk_label_set_justify(GTK_LABEL(m_noFolder), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(m_noFolder, "dim-label");
    gtk_widget_set_margin_start(m_noFolder, 16);
    gtk_widget_set_margin_end(m_noFolder, 16);
    gtk_widget_set_margin_top(m_noFolder, 24);

    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), m_fileSearch);
    gtk_box_append(GTK_BOX(box), m_noFolder);
    gtk_box_append(GTK_BOX(box), scroll);
    return box;
}

std::string Shell::browserFolder() const
{
    const std::string chosen = settingString("Browser", "folder");
    if (!chosen.empty() && g_file_test(chosen.c_str(), G_FILE_TEST_IS_DIR))
        return chosen;
    // Until one is chosen: the folder of the document shown.
    if (Window *w = current())
        if (!w->document().path().empty()) {
            gchar *dir = g_path_get_dirname(w->document().path().c_str());
            std::string out = dir;
            g_free(dir);
            return out;
        }
    return {};
}

void Shell::chooseFolder()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Show Folder in Sidebar");
    gtk_file_dialog_set_accept_label(dialog, "Show");
    const std::string folder = browserFolder();
    if (!folder.empty()) {
        GFile *f = g_file_new_for_path(folder.c_str());
        gtk_file_dialog_set_initial_folder(dialog, f);
        g_object_unref(f);
    }
    gtk_file_dialog_select_folder(dialog, gtkWindow(), nullptr,
                                  [](GObject *src, GAsyncResult *res, gpointer p) {
                                      auto *self = static_cast<Shell *>(p);
                                      GError *err = nullptr;
                                      GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
                                      if (!file) {
                                          if (err)
                                              g_error_free(err);
                                          return;
                                      }
                                      char *path = g_file_get_path(file);
                                      g_object_unref(file);
                                      if (path) {
                                          setSettingString("Browser", "folder", path);
                                          g_free(path);
                                      }
                                      for (Shell *s : Shell::all(self->app()))
                                          s->reloadBrowser();
                                      self->setSidebarShown(true);
                                  },
                                  this);
    g_object_unref(dialog);
}

void Shell::documentsChanged(GtkApplication *app)
{
    for (Shell *s : all(app))
        s->reloadBrowser();
}

void Shell::reloadBrowser()
{
    if (!m_fileList || !sidebarShown() || m_closing)
        return;
    gtk_list_box_remove_all(GTK_LIST_BOX(m_fileList));
    m_rows = 0;
    const std::string folder = browserFolder();
    gtk_label_set_text(GTK_LABEL(m_folderLabel), folder.empty() ? "No Folder" : baseNameOf(folder).c_str());
    gtk_widget_set_tooltip_text(m_folderLabel, folder.c_str());
    gtk_widget_set_visible(m_noFolder, folder.empty());

    auto header = [this](const char *title) {
        GtkWidget *label = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_add_css_class(label, "restricto-browser-header");
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_append(GTK_LIST_BOX(m_fileList), row);
    };

    // Open documents with something to lose: unsaved changes, or never saved.
    std::vector<Window *> unsaved;
    for (Window *w : Window::all(app()))
        if (!w->isEmpty() && (w->document().isModified() || w->document().path().empty()))
            unsaved.push_back(w);
    if (!unsaved.empty()) {
        header("Not Saved");
        for (Window *w : unsaved) {
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("text-x-generic-symbolic"));
            GtkWidget *label = gtk_label_new(w->displayName().c_str());
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_box_append(GTK_BOX(box), label);
            if (w->document().isModified())
                gtk_box_append(GTK_BOX(box), redDot());
            GtkWidget *row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
            g_object_set_data(G_OBJECT(row), "page", w);
            gtk_list_box_append(GTK_LIST_BOX(m_fileList), row);
        }
    }
    if (folder.empty())
        return;
    gchar *folded = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(m_fileSearch)), -1);
    const std::string query = folded;
    g_free(folded);
    if (!query.empty()) {
        // The search walks the folder tree in a thread; the list fills in when
        // it is done (a newer search makes an older one's results stale).
        const std::string key = folder + '\n' + query;
        if (key != m_searchKey) {
            m_searchKey = key;
            m_found.clear();
            m_searching = true;
            startSearch(folder, query);
        }
        header(m_searching ? "Searching…" : m_found.empty() ? "No Matching Files" : "Search Results");
        for (const std::string &path : m_found)
            addBrowserRow(path, false, 0);
        return;
    }
    m_searchKey.clear();
    header("Files");
    // Depth-first through the open folders.
    std::vector<std::tuple<std::string, bool, int>> stack;
    auto push = [&](const std::string &dir, int depth) {
        const auto entries = listFolder(dir);
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            stack.emplace_back(it->first, it->second, depth);
    };
    push(folder, 0);
    while (!stack.empty() && m_rows < 3000) {
        auto [path, isDir, depth] = stack.back();
        stack.pop_back();
        addBrowserRow(path, isDir, depth);
        if (isDir && g_expanded.count(path))
            push(path, depth + 1);
    }
}

namespace {
struct SearchJob {
    std::string folder, query, key;
    std::vector<std::string> found;
};
} // namespace

void Shell::startSearch(const std::string &folder, const std::string &query)
{
    auto *job = new SearchJob{folder, query, m_searchKey, {}};
    // The task holds the window, so the shell outlives the search.
    GTask *task = g_task_new(m_window, nullptr, [](GObject *window, GAsyncResult *res, gpointer) {
        auto *job = static_cast<SearchJob *>(g_task_get_task_data(G_TASK(res)));
        Shell *self = fromWindow(GTK_WINDOW(window));
        if (self && !self->m_closing && job->key == self->m_searchKey) {
            self->m_found = std::move(job->found);
            self->m_searching = false;
            self->reloadBrowser();
        }
    }, nullptr);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<SearchJob *>(p); });
    g_task_run_in_thread(task, [](GTask *task, gpointer, gpointer data, GCancellable *) {
        auto *job = static_cast<SearchJob *>(data);
        int folders = 0;
        findFiles(job->folder, job->query, &job->found, &folders);
        std::sort(job->found.begin(), job->found.end(),
                  [](const std::string &a, const std::string &b) { return baseNameOf(a) < baseNameOf(b); });
        g_task_return_boolean(task, TRUE);
    });
    g_object_unref(task);
}

void Shell::addBrowserRow(const std::string &path, bool folder, int depth)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(box, depth * 16);
    if (folder) {
        GtkWidget *arrow = gtk_image_new_from_icon_name(g_expanded.count(path) ? "pan-down-symbolic" : "pan-end-symbolic");
        gtk_widget_add_css_class(arrow, "dim-label");
        gtk_box_append(GTK_BOX(box), arrow);
    }
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(folder ? "folder-symbolic" : "text-x-generic-symbolic"));
    GtkWidget *label = gtk_label_new(baseNameOf(path).c_str());
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    if (!folder)
        for (Window *w : Window::all(app()))
            if (w->document().path() == path && w->document().isModified()) {
                gtk_box_append(GTK_BOX(box), redDot());
                break;
            }
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(row, path.c_str());
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path.c_str()), g_free);
    g_object_set_data(G_OBJECT(row), "folder", GINT_TO_POINTER(folder));
    gtk_list_box_append(GTK_LIST_BOX(m_fileList), row);
    ++m_rows;
}

void Shell::browserRowActivated(GtkListBoxRow *row)
{
    if (auto *page = static_cast<Window *>(g_object_get_data(G_OBJECT(row), "page"))) {
        // Only if it is still open.
        for (Window *w : Window::all(app()))
            if (w == page) {
                w->present();
                return;
            }
        return;
    }
    const char *p = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "path"));
    if (!p)
        return;
    const std::string path = p;
    if (g_object_get_data(G_OBJECT(row), "folder")) {
        if (!g_expanded.erase(path))
            g_expanded.insert(path);
        reloadBrowser();
        return;
    }
    for (Window *w : Window::all(app()))
        if (w->document().path() == path) {
            w->present();
            return;
        }
    openFiles(app(), {path}, current());
}

void Shell::rebuildMenu()
{
    const bool exp = Window::experimentalTools();
    auto addExp = [exp](GMenu *m, const char *label, const char *action) {
        if (exp)
            g_menu_append(m, label, action);
    };
    // The same items, names and grouping as the Mac menu bar (Tools.swift):
    // File, Edit, Sequence (Features, Primers), Cloning, Align, View.
    GMenu *menu = g_menu_new();
    GMenu *fileSection = g_menu_new();
    g_menu_append(fileSection, "New Sequence…", "app.new");
    g_menu_append(fileSection, "Open…", "app.open");
    g_menu_append(fileSection, "Show Folder in Sidebar…", "win.show-folder");
    addExp(fileSection, "Search a Folder…", "win.search-folder");
    g_menu_append(fileSection, "Save", "win.save");
    g_menu_append(fileSection, "Save As…", "win.save-as");
    g_menu_append(fileSection, "Revert to Saved", "win.revert");
    g_menu_append(fileSection, "Export FASTA…", "win.export-fasta");
    g_menu_append(fileSection, "Export Map…", "win.export-map");
    addExp(fileSection, "Print…", "win.print");
    g_menu_append(fileSection, "Close", "win.close");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(fileSection));
    GMenu *editSection = g_menu_new();
    g_menu_append(editSection, "Cut", "win.cut");
    g_menu_append(editSection, "Copy", "win.copy");
    g_menu_append(editSection, "Copy Reverse Complement", "win.copy-rc");
    g_menu_append(editSection, "Copy Map Image", "win.copy-map");
    g_menu_append(editSection, "Paste", "win.paste");
    g_menu_append(editSection, "Select All", "win.select-all");
    g_menu_append(editSection, "Find…", "win.find");
    g_menu_append(editSection, "Go to Position…", "win.go-to");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(editSection));
    GMenu *toolSection = g_menu_new();
    GMenu *seqMenu = g_menu_new();
    g_menu_append(seqMenu, "Reverse Complement", "win.reverse-complement");
    g_menu_append(seqMenu, "Set Origin at Selection", "win.set-origin");
    g_menu_append(seqMenu, "Circular", "win.circular");
    g_menu_append(seqMenu, "Rename…", "win.rename");
    GMenu *featureMenu = g_menu_new();
    GMenu *featureEdit = g_menu_new();
    g_menu_append(featureEdit, "Add Feature…", "win.add-feature");
    g_menu_append(featureEdit, "Edit Feature…", "win.edit-feature");
    g_menu_append(featureEdit, "Delete Feature", "win.delete-feature");
    g_menu_append_section(featureMenu, nullptr, G_MENU_MODEL(featureEdit));
    GMenu *featureFind = g_menu_new();
    g_menu_append(featureFind, "Annotate Common Features", "win.annotate");
    addExp(featureFind, "Find ORFs…", "win.orfs");
    addExp(featureFind, "Protein Properties…", "win.protein");
    g_menu_append_section(featureMenu, nullptr, G_MENU_MODEL(featureFind));
    GMenu *primerMenu = g_menu_new();
    GMenu *primerEdit = g_menu_new();
    g_menu_append(primerEdit, "Add Primer…", "win.add-primer");
    g_menu_append(primerEdit, "Add Primer by Sequence…", "win.primer-by-sequence");
    g_menu_append(primerEdit, "Delete Primer…", "win.delete-primer");
    g_menu_append_section(primerMenu, nullptr, G_MENU_MODEL(primerEdit));
    GMenu *primerTools = g_menu_new();
    g_menu_append(primerTools, "Design Primers…", "win.design-primers");
    g_menu_append(primerTools, "Check a Primer…", "win.check-primer");
    g_menu_append_section(primerMenu, nullptr, G_MENU_MODEL(primerTools));
    GMenu *seqSub = g_menu_new();
    g_menu_append_submenu(seqSub, "Features", G_MENU_MODEL(featureMenu));
    g_menu_append_submenu(seqSub, "Primers", G_MENU_MODEL(primerMenu));
    g_menu_append_section(seqMenu, nullptr, G_MENU_MODEL(seqSub));
    g_menu_append_submenu(toolSection, "Sequence", G_MENU_MODEL(seqMenu));
    GMenu *cloneMenu = g_menu_new();
    GMenu *cloneDigest = g_menu_new();
    g_menu_append(cloneDigest, "Digest and Gel…", "win.digest");
    addExp(cloneDigest, "Choose Enzymes…", "win.choose-enzymes");
    g_menu_append_section(cloneMenu, nullptr, G_MENU_MODEL(cloneDigest));
    GMenu *cloneCore = g_menu_new();
    g_menu_append(cloneCore, "PCR Product…", "win.pcr");
    g_menu_append(cloneCore, "Insertion by PCR…", "win.insertion");
    g_menu_append(cloneCore, "Gibson Assembly…", "win.gibson");
    g_menu_append_section(cloneMenu, nullptr, G_MENU_MODEL(cloneCore));
    GMenu *cloneExp = g_menu_new();
    addExp(cloneExp, "Restriction Cloning…", "win.clone");
    addExp(cloneExp, "Golden Gate Assembly…", "win.golden-gate");
    addExp(cloneExp, "TA, GC and TOPO Cloning…", "win.ta-cloning");
    addExp(cloneExp, "Overlap Extension PCR…", "win.overlap-extension");
    addExp(cloneExp, "Mutagenesis…", "win.mutagenesis");
    addExp(cloneExp, "Cloning History", "win.history");
    g_menu_append_section(cloneMenu, nullptr, G_MENU_MODEL(cloneExp));
    g_menu_append_submenu(toolSection, "Cloning", G_MENU_MODEL(cloneMenu));
    GMenu *alignMenu = g_menu_new();
    g_menu_append(alignMenu, "Align Read…", "win.align");
    g_menu_append(alignMenu, "Compare with Another Sequence…", "win.compare");
    g_menu_append(alignMenu, "Hide Aligned Reads", "win.clear-reads");
    if (exp)
        g_menu_append_submenu(toolSection, "Align", G_MENU_MODEL(alignMenu));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(toolSection));
    GMenu *viewMenu = g_menu_new();
    GMenu *viewLayout = g_menu_new();
    g_menu_append(viewLayout, "Show Sidebar", "win.sidebar");
    g_menu_append(viewLayout, "Show Panel", "win.show-panel");
    g_menu_append(viewLayout, "Show Map", "win.show-map");
    g_menu_append(viewLayout, "Stack Map Above Sequence", "win.stack-map");
    g_menu_append_section(viewMenu, nullptr, G_MENU_MODEL(viewLayout));
    GMenu *viewSeq = g_menu_new();
    GMenu *framesMenu = g_menu_new();
    g_menu_append(framesMenu, "No Translation", "win.frames::0");
    g_menu_append(framesMenu, "Forward ORFs", "win.frames::3");
    g_menu_append(framesMenu, "ORFs on Both Strands", "win.frames::6");
    if (exp)
        g_menu_append_submenu(viewSeq, "Translate", G_MENU_MODEL(framesMenu));
    addExp(viewSeq, "Map Display…", "win.map-options");
    g_menu_append(viewSeq, "Show Complement Strand", "win.show-complement");
    g_menu_append(viewSeq, "Group Bases in Tens", "win.group-tens");
    g_menu_append_section(viewMenu, nullptr, G_MENU_MODEL(viewSeq));
    GMenu *viewWindow = g_menu_new();
    g_menu_append(viewWindow, "Full Screen", "win.fullscreen");
    g_menu_append_section(viewMenu, nullptr, G_MENU_MODEL(viewWindow));
    GMenu *appSection = g_menu_new();
    g_menu_append_submenu(appSection, "View", G_MENU_MODEL(viewMenu));
    g_menu_append(appSection, "Experimental Tools", "app.experimental");
    g_menu_append(appSection, "About Restricto", "app.about");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(appSection));
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(m_menuButton), G_MENU_MODEL(menu));
    for (GMenu *m : {fileSection, editSection, toolSection, seqMenu, featureMenu, featureEdit, featureFind,
                     primerMenu, primerEdit, primerTools, seqSub, cloneMenu, cloneDigest, cloneCore, cloneExp,
                     alignMenu, viewMenu, viewLayout, viewSeq, framesMenu, viewWindow, appSection, menu})
        g_object_unref(m);
}

} // namespace oc::gtk
