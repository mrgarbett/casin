// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mapview.h"
#include "seqview.h"
#include "primereditor.h"
#include "gelwizard.h"
#include "pcrwizard.h"
#include "core/abif.h"
#include "core/seqio.h"
#include "view/document.h"

#include <adwaita.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace oc::gtk {

class Shell;

// One document, shown as a tab of a Shell (the top-level window): the map and
// sequence in the middle, a Sequence/Enzymes/Features/Primers panel on the
// right picked from a floating pill, and floating status capsules. Owned by
// its tab's widget (deleted when the tab closes).
class Window {
public:
    // A new document, as a tab of the app's active window (or of a new window).
    static Window *create(GtkApplication *app);
    // The document `w` belongs to: its tab, or its window's current tab.
    static Window *fromWidget(GtkWidget *w);
    // Every open document, in every window.
    static std::vector<Window *> all(GtkApplication *app);

    GtkWindow *gtkWindow() const { return GTK_WINDOW(m_window); }
    GtkWidget *widget() const { return m_page; }
    Shell *shell() const { return m_shell; }
    Document &document() { return m_doc; }
    const Document &document() const { return m_doc; }
    bool isEmpty() const { return m_doc.isEmpty(); }
    // The file it was opened from or saved to, or empty.
    const std::string &path() const { return m_doc.path(); }
    // A wizard (gel, PCR) is open over this tab.
    bool hasTools() const { return m_pcr || m_gel; }
    bool isCurrent() const;
    bool load(const std::string &path, std::string *error);
    // Shows this tab and brings its window to the front.
    void present();
    void showError(const std::string &heading, const std::string &body);
    void toast(const std::string &text);
    // Shows just these enzymes on the map, sequence and enzyme list.
    void showOnlyEnzymes(const std::set<std::string> &names, int maxCuts);
    // Saves a PNG of the window once it has been laid out, then calls done.
    void screenshot(const std::string &path, void (*done)(bool ok, gpointer), gpointer data);
    // Tools from the first alpha, hidden until each gets its own tested step.
    // Saved in the user's settings and applied to every open window.
    static bool experimentalTools();
    static void setExperimentalTools(GtkApplication *app, bool on);
    // The file name shown in the title and tab ("Untitled" for a new sequence).
    std::string displayName() const;
    // Under the title: the sequence name when it differs from the file's, the
    // length and the topology.
    std::string subtitle() const;

private:
    friend class Shell;
    explicit Window(Shell *shell);
    ~Window();
    void buildUi();
    GtkWidget *buildPanel();
    // The window's actions, each run on the window's current tab.
    static void addActions(GActionMap *map, Shell *shell);
    // Asks about unsaved changes before the tab (or, with closeWindow, the
    // window) closes. True when it can close right away.
    bool confirmClose(bool closeWindow);
    // The tab is closing: closes the windows it opened, such as the PCR wizard.
    void closeTools();
    void finishClose();
    // Map beside the sequence (the sequence then sits in the panel), stacked
    // above it, or hidden.
    void applyLayout();
    void setMapStacked(bool stacked);
    void setMapHidden(bool hidden);
    void setPanelShown(bool shown);
    // Sizes the panel (and the map against the sequence) once laid out.
    void placePanel();
    void placeSplit();
    void settle();
    void toggleView(int which);   // the status capsule's toggles, 0-3
    // One of "sequence", "enzymes", "features", "primers".
    void showPane(const std::string &pane);
    void pillClicked(const std::string &pane);
    void refreshPill();
    void refreshToggles();
    void showFindBar(bool show);
    void showBadge(GtkWidget *from, double x, double y);
    void applyEnzymeSearch();
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
    static void actToggleSidebar(GSimpleAction *, GVariant *, gpointer self);
    static void actShowFolder(GSimpleAction *, GVariant *, gpointer self);
    static void actShowPanel(GSimpleAction *, GVariant *, gpointer self);
    static void actShowMap(GSimpleAction *, GVariant *, gpointer self);
    static void actStackMap(GSimpleAction *, GVariant *, gpointer self);
    static void actFullscreen(GSimpleAction *, GVariant *, gpointer self);
    static void actExportGenBank(GSimpleAction *, GVariant *, gpointer self);
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
    static void actInsertion(GSimpleAction *, GVariant *, gpointer self);
    static void actExportFasta(GSimpleAction *, GVariant *, gpointer self);
    static void actExportMap(GSimpleAction *, GVariant *, gpointer self);
    static void actCopy(GSimpleAction *, GVariant *, gpointer self);
    static void actCopyRc(GSimpleAction *, GVariant *, gpointer self);
    static void actFind(GSimpleAction *, GVariant *, gpointer self);
    static void actClose(GSimpleAction *, GVariant *, gpointer self);
    static void actComplement(GSimpleAction *, GVariant *, gpointer self);
    static void actFrames(GSimpleAction *, GVariant *, gpointer self);
    static void actClearReads(GSimpleAction *, GVariant *, gpointer self);
    static void actGroupTens(GSimpleAction *, GVariant *, gpointer self);
    static void actZoomIn(GSimpleAction *, GVariant *, gpointer self);
    static void actZoomOut(GSimpleAction *, GVariant *, gpointer self);
    static void actZoomReset(GSimpleAction *, GVariant *, gpointer self);
    // Steps the map and sequence zoom of every window (0 = back to 100 %).
    void stepZoom(int direction);
    static void actRevert(GSimpleAction *, GVariant *, gpointer self);
    static void actCopyMap(GSimpleAction *, GVariant *, gpointer self);
    static void actGoTo(GSimpleAction *, GVariant *, gpointer self);
    static void actRename(GSimpleAction *, GVariant *, gpointer self);
    static void actAnnotate(GSimpleAction *, GVariant *, gpointer self);
    static void actDeletePrimer(GSimpleAction *, GVariant *, gpointer self);
    static void actCheckPrimer(GSimpleAction *, GVariant *, gpointer self);
    void copyText(const std::string &text, const char *what);
    void saveDialog(const std::string &title, const std::string &suggested, int kind, bool closeAfter = false);
    // Saves to the document's own path when its format keeps everything;
    // otherwise asks where. Returns true when it saved right away.
    bool save(bool closeAfter = false);
    bool saveTo(const std::string &path, FileFormat format);
    void updateActions();
    void rebuildPrimerList();
public:
    // Opens the Primers pane with a new primer's inputs, filled in with
    // `bases` if given. An editor already open keeps what was typed unless
    // new bases come.
    void beginAddingPrimer(const std::string &bases);
private:
    void endAddingPrimer();
    // Feature dialog: index < 0 adds a feature from the selection.
    void featureDialog(int index);
    void openGelWizard();
    void openPcrWizard();
    void cloningDialog();
    void goldenGateDialog();
    void gibsonDialog();
    void designPrimersDialog();
    void chooseEnzymesDialog();
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
    void insertionDialog();
    void renameDialog();
    void goToDialog();
    void deletePrimerDialog();
    void checkPrimerDialog();
    void copyMapImage();
    void revert();
    void pasteText(const std::string &text);

    Document m_doc;
    Shell *m_shell = nullptr;
    GtkWidget *m_window = nullptr;   // the shell's window, parent of dialogs
    GtkWidget *m_page = nullptr;     // the tab's content
    GtkWidget *m_stack = nullptr;    // "empty" or "document"
    GtkWidget *m_outer = nullptr;    // middle | panel
    GtkWidget *m_middle = nullptr;
    GtkWidget *m_split = nullptr;    // map | sequence, or map over sequence
    GtkWidget *m_seqPane = nullptr;  // the sequence with its find bar; moves between middle and panel
    GtkWidget *m_panel = nullptr;
    GtkWidget *m_panelStack = nullptr;
    GtkWidget *m_sequenceHolder = nullptr;
    GtkWidget *m_pill = nullptr;
    std::vector<std::pair<std::string, GtkWidget *>> m_pillButtons;
    GtkWidget *m_statusRow = nullptr;
    std::vector<GtkWidget *> m_toggles;   // complement, translation, enzymes, features
    GtkWidget *m_info = nullptr;          // the second status capsule
    GtkWidget *m_status = nullptr;
    GtkWidget *m_findBar = nullptr;
    GtkWidget *m_search = nullptr;
    GtkWidget *m_badge = nullptr;
    GtkWidget *m_badgeLabel = nullptr;
    guint m_badgeTimer = 0;
    GtkWidget *m_enzymeSearch = nullptr;
    GtkWidget *m_onMapOnly = nullptr;
    int m_hoverBase = -1;
    std::string m_statusMessage;   // from Find, until the selection changes
    bool m_mapStacked = false;
    bool m_mapHidden = false;
    bool m_panelShown = true;
    // The panel slides open and shut; where its divider rests while it is shut.
    AdwAnimation *m_panelAnim = nullptr;
    int m_panelRest = 0;
    std::string m_pane = "sequence";
    std::string m_tablePane = "enzymes";   // the pane last picked other than the sequence
    bool m_settingToggles = false;
    bool m_closeWindow = false;
    int m_panelWant = 0;        // width to give the panel once laid out; -1 = half
    bool m_placeSplit = false;
    guint m_settleTimer = 0;
    std::string m_browserKey;   // what the folder browsers last showed of this document
    GtkWidget *m_featureList = nullptr;
    GtkWidget *m_enzymeList = nullptr;
    GtkWidget *m_primerList = nullptr;
    // The Primers pane: the list, or a new primer's inputs while one is added.
    GtkWidget *m_primerStack = nullptr;
    std::unique_ptr<PrimerEditor> m_primerEditor;
    guint m_endPrimerIdle = 0;
    PcrWizard *m_pcr = nullptr;   // owns itself; closes with this tab
    GelWizard *m_gel = nullptr;   // the same
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

// Inside a Flatpak the host's files are only visible through the document
// portal; when `path` can't be read there, returns `error` with a hint on how
// to pass files in. Otherwise returns `error` unchanged.
std::string withSandboxHint(const std::string &path, const std::string &error);

// Lets a combo row's menu show its items' whole names (the row itself still
// shortens the chosen one to fit). For rows whose model is a GtkStringList.
void showWholeNames(AdwComboRow *row);

} // namespace oc::gtk
