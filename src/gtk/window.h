// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mapview.h"
#include "seqview.h"
#include "view/document.h"

#include <adwaita.h>
#include <memory>
#include <string>
#include <vector>

namespace oc::gtk {

// One window per document. Owned by its GtkWindow (deleted on destroy).
class Window {
public:
    static Window *create(GtkApplication *app);
    static Window *fromWidget(GtkWidget *w);

    GtkWindow *gtkWindow() const { return GTK_WINDOW(m_window); }
    Document &document() { return m_doc; }
    bool isEmpty() const { return m_doc.isEmpty(); }
    bool load(const std::string &path, std::string *error);
    void present() { gtk_window_present(gtkWindow()); }
    void showError(const std::string &heading, const std::string &body);
    void toast(const std::string &text);
    // Saves a PNG of the window once it has been laid out, then calls done.
    void screenshot(const std::string &path, void (*done)(bool ok, gpointer), gpointer data);

private:
    explicit Window(GtkApplication *app);
    ~Window();
    void buildUi(GtkApplication *app);
    GtkWidget *buildSidebar();
    void rebuildFeatureList();
    void rebuildEnzymeList();
    void refreshEnzymeRows();
    void updateTitle();
    void updateStatus();
    void find(const std::string &query);
    void exportMap(const std::string &path);

    static void onDarkChanged(GObject *, GParamSpec *, gpointer self);
    static void onFilterChanged(GObject *, GParamSpec *, gpointer self);
    static void onFeatureRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer self);
    static void onEnzymeRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer self);
    static void onEnzymeToggled(GtkCheckButton *, gpointer self);
    static void onSearchActivate(GtkSearchEntry *, gpointer self);
    static gboolean onDrop(GtkDropTarget *, const GValue *value, double x, double y, gpointer self);
    static void actSaveGenBank(GSimpleAction *, GVariant *, gpointer self);
    static void actExportFasta(GSimpleAction *, GVariant *, gpointer self);
    static void actExportMap(GSimpleAction *, GVariant *, gpointer self);
    static void actCopy(GSimpleAction *, GVariant *, gpointer self);
    static void actCopyRc(GSimpleAction *, GVariant *, gpointer self);
    static void actFind(GSimpleAction *, GVariant *, gpointer self);
    static void actClose(GSimpleAction *, GVariant *, gpointer self);
    static void actComplement(GSimpleAction *, GVariant *, gpointer self);
    void copyText(const std::string &text, const char *what);
    void saveDialog(const std::string &title, const std::string &suggested, int kind);

    Document m_doc;
    GtkWidget *m_window = nullptr;
    GtkWidget *m_toasts = nullptr;
    GtkWidget *m_stack = nullptr;
    GtkWidget *m_title = nullptr;
    GtkWidget *m_status = nullptr;
    GtkWidget *m_search = nullptr;
    GtkWidget *m_split = nullptr;
    GtkWidget *m_featureList = nullptr;
    GtkWidget *m_enzymeList = nullptr;
    GtkWidget *m_filter = nullptr;
    std::unique_ptr<MapView> m_map;
    std::unique_ptr<SequenceView> m_seq;
    struct EnzymeRow {
        std::string name;
        GtkWidget *row;
        GtkWidget *check;
        GtkWidget *count;
    };
    std::vector<EnzymeRow> m_enzymeRows;
    bool m_updatingRows = false;
};

// Opens files: the first goes into `preferred` if it is empty, the rest get
// windows of their own.
void openFiles(GtkApplication *app, const std::vector<std::string> &paths, Window *preferred = nullptr);
void showOpenDialog(GtkApplication *app, GtkWindow *parent);

} // namespace oc::gtk
