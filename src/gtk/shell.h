// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <adwaita.h>
#include <set>
#include <string>
#include <vector>

namespace oc::gtk {

class Window;

// A top-level window: a folder browser on the left, the open documents as
// tabs (the tab bar shows from two on) and one header bar whose buttons,
// title and menu act on the current tab. Owned by its GtkWindow.
class Shell {
public:
    static Shell *create(GtkApplication *app);
    static Shell *fromWindow(GtkWindow *w);
    // The app's active window, or nullptr.
    static Shell *active(GtkApplication *app);
    static std::vector<Shell *> all(GtkApplication *app);

    GtkWindow *gtkWindow() const { return GTK_WINDOW(m_window); }
    GtkWidget *widget() const { return m_window; }
    GtkApplication *app() const { return gtk_window_get_application(gtkWindow()); }
    Window *current() const;
    std::vector<Window *> pages() const;
    // A tab's Save/Discard/Cancel is up: it now asks about closing the
    // window, and its answer carries on with that. False when none is.
    bool askingClose();
    void addPage(Window *w);
    void selectPage(Window *w);
    // Closes the tab, asking first about unsaved changes.
    void closePage(Window *w);
    void toast(const std::string &text);
    void rebuildMenu();
    // The title, the tab titles and the header toggles, from the current tab.
    void refreshChrome();
    void setTabTitle(Window *w);
    bool sidebarShown() const;
    void setSidebarShown(bool shown);
    void chooseFolder();

    // Every window's folder browser: documents opened, closed, saved or changed.
    static void documentsChanged(GtkApplication *app);
    static void loadStyles();

private:
    explicit Shell(GtkApplication *app);
    void buildUi();
    GtkWidget *buildBrowser();
    void reloadBrowser();
    void startSearch(const std::string &folder, const std::string &query);
    void addBrowserRow(const std::string &path, bool folder, int depth);
    void browserRowActivated(GtkListBoxRow *row);
    std::string browserFolder() const;
    static void onSelectedPage(GObject *, GParamSpec *, gpointer self);
    static gboolean onClosePage(AdwTabView *, AdwTabPage *page, gpointer self);
    static void onPageAttached(AdwTabView *, AdwTabPage *page, int, gpointer self);
    static void onPagesChanged(GObject *, GParamSpec *, gpointer self);
    static gboolean onCloseRequest(GtkWindow *, gpointer self);
    static gboolean onDrop(GtkDropTarget *, const GValue *value, double, double, gpointer self);

    GtkWidget *m_window = nullptr;
    GtkWidget *m_toasts = nullptr;
    GtkWidget *m_split = nullptr;     // folder browser | tabs
    GtkWidget *m_tabs = nullptr;      // AdwTabView
    GtkWidget *m_menuButton = nullptr;
    GtkWidget *m_title = nullptr;
    GtkWidget *m_titleDot = nullptr;
    GtkWidget *m_subtitle = nullptr;
    // Folder browser.
    GtkWidget *m_folderLabel = nullptr;
    GtkWidget *m_fileSearch = nullptr;
    GtkWidget *m_fileList = nullptr;
    GtkWidget *m_noFolder = nullptr;
    int m_rows = 0;
    // The file search: what was asked, and what it found.
    std::string m_searchKey;
    std::vector<std::string> m_found;
    bool m_searching = false;
    bool m_closing = false;
};

// Settings kept between launches (~/.config/restricto/settings.ini).
bool settingBool(const char *group, const char *key, bool fallback);
void setSettingBool(const char *group, const char *key, bool value);
std::string settingString(const char *group, const char *key);
void setSettingString(const char *group, const char *key, const std::string &value);

} // namespace oc::gtk
