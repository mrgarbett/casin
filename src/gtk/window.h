// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mapview.h"
#include "seqview.h"
#include "core/abif.h"
#include "core/seqio.h"
#include "view/document.h"

#include <adwaita.h>
#include <memory>
#include <set>
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
    // Shows just these enzymes on the map, sequence and enzyme list.
    void showOnlyEnzymes(const std::set<std::string> &names, int maxCuts);
    // Saves a PNG of the window once it has been laid out, then calls done.
    void screenshot(const std::string &path, void (*done)(bool ok, gpointer), gpointer data);

private:
    explicit Window(GtkApplication *app);
    ~Window();
    void buildUi(GtkApplication *app);
    GtkWidget *buildSidebar();
    void rebuildFeatureList();
    void rebuildEnzymeList();
    void rebuildEnzymeSets(const std::string &select = {});
    void saveEnzymeSetDialog();
    void refreshEnzymeRows();
    void updateTitle();
    void updateStatus();
    void find(const std::string &query);
    void exportMap(const std::string &path);

    static void onDarkChanged(GObject *, GParamSpec *, gpointer self);
    static void onFilterChanged(GObject *, GParamSpec *, gpointer self);
    static void onEnzymeSetChanged(GObject *, GParamSpec *, gpointer self);
    static void onFeatureRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer self);
    static void onEnzymeRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer self);
    static void onEnzymeToggled(GtkCheckButton *, gpointer self);
    static void onSearchActivate(GtkSearchEntry *, gpointer self);
    static gboolean onDrop(GtkDropTarget *, const GValue *value, double x, double y, gpointer self);
    static void actSave(GSimpleAction *, GVariant *, gpointer self);
    static void actSaveAs(GSimpleAction *, GVariant *, gpointer self);
    static void actUndo(GSimpleAction *, GVariant *, gpointer self);
    static void actRedo(GSimpleAction *, GVariant *, gpointer self);
    static void actCut(GSimpleAction *, GVariant *, gpointer self);
    static void actPaste(GSimpleAction *, GVariant *, gpointer self);
    static void actSelectAll(GSimpleAction *, GVariant *, gpointer self);
    static void actReverseComplement(GSimpleAction *, GVariant *, gpointer self);
    static void actSetOrigin(GSimpleAction *, GVariant *, gpointer self);
    static void actCircular(GSimpleAction *, GVariant *, gpointer self);
    static void actAddFeature(GSimpleAction *, GVariant *, gpointer self);
    static void actEditFeature(GSimpleAction *, GVariant *, gpointer self);
    static void actDeleteFeature(GSimpleAction *, GVariant *, gpointer self);
    static void actAddPrimer(GSimpleAction *, GVariant *, gpointer self);
    static void actDigest(GSimpleAction *, GVariant *, gpointer self);
    static void actPcr(GSimpleAction *, GVariant *, gpointer self);
    static void actClone(GSimpleAction *, GVariant *, gpointer self);
    static void actGoldenGate(GSimpleAction *, GVariant *, gpointer self);
    static void actGibson(GSimpleAction *, GVariant *, gpointer self);
    static void actDesignPrimers(GSimpleAction *, GVariant *, gpointer self);
    static void actChooseEnzymes(GSimpleAction *, GVariant *, gpointer self);
    static void actPrimerBySequence(GSimpleAction *, GVariant *, gpointer self);
    static void actMutagenesis(GSimpleAction *, GVariant *, gpointer self);
    static void actHistory(GSimpleAction *, GVariant *, gpointer self);
    static void actAlign(GSimpleAction *, GVariant *, gpointer self);
    static void actCompare(GSimpleAction *, GVariant *, gpointer self);
    static void actOrfs(GSimpleAction *, GVariant *, gpointer self);
    static void actMapOptions(GSimpleAction *, GVariant *, gpointer self);
    static void actPrint(GSimpleAction *, GVariant *, gpointer self);
    static void actSearchFolder(GSimpleAction *, GVariant *, gpointer self);
    static void actTaCloning(GSimpleAction *, GVariant *, gpointer self);
    static void actOverlapExtension(GSimpleAction *, GVariant *, gpointer self);
    static void actProtein(GSimpleAction *, GVariant *, gpointer self);
    static gboolean onCloseRequest(GtkWindow *, gpointer self);
    static void actExportFasta(GSimpleAction *, GVariant *, gpointer self);
    static void actExportMap(GSimpleAction *, GVariant *, gpointer self);
    static void actCopy(GSimpleAction *, GVariant *, gpointer self);
    static void actCopyRc(GSimpleAction *, GVariant *, gpointer self);
    static void actFind(GSimpleAction *, GVariant *, gpointer self);
    static void actClose(GSimpleAction *, GVariant *, gpointer self);
    static void actComplement(GSimpleAction *, GVariant *, gpointer self);
    static void actFrames(GSimpleAction *, GVariant *, gpointer self);
    static void actClearReads(GSimpleAction *, GVariant *, gpointer self);
    void copyText(const std::string &text, const char *what);
    void saveDialog(const std::string &title, const std::string &suggested, int kind, bool closeAfter = false);
    // Saves to the document's own path when its format keeps everything;
    // otherwise asks where. Returns true when it saved right away.
    bool save(bool closeAfter = false);
    bool saveTo(const std::string &path, FileFormat format);
    void updateActions();
    void rebuildPrimerList();
    // Feature dialog: index < 0 adds a feature from the selection.
    void featureDialog(int index);
    void primerDialog();
    void digestDialog();
    void pcrDialog();
    void cloningDialog();
    void goldenGateDialog();
    void gibsonDialog();
    void designPrimersDialog();
    void chooseEnzymesDialog();
    void primerBySequenceDialog();
    void mutagenesisDialog();
    void historyDialog();
    // compare = pick any open sequence (a parent construct), not just linear reads.
    void alignDialog(bool compare = false);
    void orfDialog();
    void mapOptionsDialog();
    void print();
    void searchFolderDialog();
    void taCloningDialog();
    void overlapExtensionDialog();
    void proteinDialog();
    void pasteText(const std::string &text);

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
    GtkWidget *m_primerList = nullptr;
    bool m_closeConfirmed = false;
    GtkWidget *m_filter = nullptr;
    GtkWidget *m_setDrop = nullptr;
    std::vector<EnzymeSet> m_sets;   // [0] = all enzymes
    bool m_settingSet = false;
    std::unique_ptr<MapView> m_map;
    std::unique_ptr<SequenceView> m_seq;
    // Chromatogram strip, shown for a document opened from a trace file.
    std::unique_ptr<Trace> m_trace;
    GtkWidget *m_traceBox = nullptr;
    GtkWidget *m_traceArea = nullptr;
    GtkAdjustment *m_traceAdj = nullptr;
    void showTrace(std::unique_ptr<Trace> trace);
    void scrollTraceToSelection();
    static void drawTrace(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self);
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
// A new window with a sequence typed or pasted by the user.
void showNewSequenceDialog(GtkApplication *app, GtkWindow *parent);

} // namespace oc::gtk
