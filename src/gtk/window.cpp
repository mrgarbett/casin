// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"

#include "shell.h"

#include "core/genbank.h"
#include "core/version.h"
#include "core/primercheck.h"
#include "core/seqio.h"
#include "core/tm.h"
#include "view/cairo_render.h"
#include "view/style.h"
#include "view/printlayout.h"
#include "view/tracelayout.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <unistd.h>

namespace oc::gtk {

namespace {

enum SaveKind { SaveSequence, SaveFasta, SaveMap, SaveGenBank };

// What this app last copied, with its features. Pasting the same bases back
// (in any window) brings the features along.
Sequence g_copied;

std::string baseName(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void drawSwatch(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data)
{
    const auto *c = static_cast<const view::Color *>(data);
    const double r = 3;
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r - 1, r + 1, r, -M_PI / 2, 0);
    cairo_arc(cr, w - r - 1, h - r - 1, r, 0, M_PI / 2);
    cairo_arc(cr, r + 1, h - r - 1, r, M_PI / 2, M_PI);
    cairo_arc(cr, r + 1, r + 1, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

GtkWidget *swatch(const view::Color &c)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 14);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 14);
    gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
    auto *copy = new view::Color(c);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), drawSwatch, copy,
                                   [](gpointer p) { delete static_cast<view::Color *>(p); });
    return area;
}

std::string locationText(const Sequence &seq, const Feature &f)
{
    const Span sp = f.span(seq.length(), seq.circular);
    int end = sp.end() > seq.length() ? sp.end() - seq.length() : sp.end();
    std::string s = view::withThousands(sp.start + 1) + "–" + view::withThousands(end);
    if (f.ranges.size() > 1 && !(f.ranges.size() == 2 && sp.wraps(seq.length())))
        s += " (" + std::to_string(f.ranges.size()) + " parts)";
    return s;
}

} // namespace

// ---------------------------------------------------------------------------

namespace {

// Runs a window action on the window's current tab.
template <void (*F)(GSimpleAction *, GVariant *, gpointer)>
void onCurrent(GSimpleAction *a, GVariant *v, gpointer shell)
{
    if (Window *w = static_cast<Shell *>(shell)->current())
        F(a, v, w);
}

// The zoom steps of the map and sequence, and the one saved.
const double kZoomSteps[] = {0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0};

double savedZoom()
{
    const double z = std::strtod(settingString("View", "zoom").c_str(), nullptr);
    return z >= 0.8 && z <= 2.0 ? z : 1.0;
}

// Room the vertical pill takes at the top right of the middle.
const int kPillRoom = 48;

GtkWidget *capsuleButton(const char *icon, const char *tip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focus_on_click(b, FALSE);
    return b;
}

// Fades something floating in or out (hidden once faded out), from wherever
// a fade already under way has got to.
void fade(GtkWidget *w, bool in)
{
    auto *anim = static_cast<AdwAnimation *>(g_object_get_data(G_OBJECT(w), "oc-fade"));
    const bool moving = anim && adw_animation_get_state(anim) == ADW_ANIMATION_PLAYING;
    const bool shown = gtk_widget_get_visible(w);
    if (!moving && (in ? shown && gtk_widget_get_opacity(w) >= 1 : !shown))
        return;
    if (moving && adw_timed_animation_get_value_to(ADW_TIMED_ANIMATION(anim)) == (in ? 1 : 0))
        return;
    if (!anim) {
        AdwAnimationTarget *target = adw_callback_animation_target_new(
            +[](double v, gpointer w) { gtk_widget_set_opacity(GTK_WIDGET(w), v); }, w, nullptr);
        anim = adw_timed_animation_new(w, 0, 1, 160, target);
        adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(anim), ADW_EASE_OUT_CUBIC);
        g_signal_connect(anim, "done", G_CALLBACK(+[](AdwAnimation *a, gpointer w) {
                             if (adw_animation_get_value(a) <= 0)
                                 gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
                         }),
                         w);
        g_object_set_data_full(G_OBJECT(w), "oc-fade", anim, g_object_unref);
    }
    const double from = shown ? gtk_widget_get_opacity(w) : 0;
    adw_animation_reset(anim);
    gtk_widget_set_opacity(w, from);
    gtk_widget_set_visible(w, TRUE);
    adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(anim), from);
    adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(anim), in ? 1 : 0);
    adw_animation_play(anim);
}

GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

void setOn(GtkWidget *w, bool on)
{
    if (on) {
        gtk_widget_add_css_class(w, "on");
        gtk_widget_remove_css_class(w, "dim");
    } else {
        gtk_widget_remove_css_class(w, "on");
        gtk_widget_add_css_class(w, "dim");
    }
}

std::string withoutExtension(const std::string &name)
{
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

} // namespace

Window *Window::create(GtkApplication *app)
{
    Shell *shell = Shell::active(app);
    if (!shell)
        shell = Shell::create(app);
    auto *w = new Window(shell);   // owned by its tab's widget
    shell->addPage(w);
    return w;
}

Window *Window::fromWidget(GtkWidget *w)
{
    for (GtkWidget *p = w; p; p = gtk_widget_get_parent(p))
        if (auto *page = static_cast<Window *>(g_object_get_data(G_OBJECT(p), "oc-page")))
            return page;
    GtkRoot *root = w ? gtk_widget_get_root(w) : nullptr;
    Shell *shell = root && GTK_IS_WINDOW(root) ? Shell::fromWindow(GTK_WINDOW(root)) : nullptr;
    return shell ? shell->current() : nullptr;
}

std::vector<Window *> Window::all(GtkApplication *app)
{
    std::vector<Window *> out;
    if (!app)
        return out;
    for (Shell *s : Shell::all(app))
        for (Window *w : s->pages())
            out.push_back(w);
    return out;
}

bool Window::isCurrent() const
{
    return m_shell && m_shell->current() == this;
}

void Window::present()
{
    m_shell->selectPage(this);
    gtk_window_present(gtkWindow());
}

std::string Window::displayName() const
{
    if (!m_doc.path().empty())
        return baseName(m_doc.path());
    return m_doc.sequence().name.empty() ? "Untitled" : m_doc.sequence().name;
}

std::string Window::subtitle() const
{
    if (m_doc.isEmpty())
        return {};
    const Sequence &s = m_doc.sequence();
    std::string sub;
    if (!m_doc.path().empty() && !s.name.empty() && s.name != withoutExtension(baseName(m_doc.path())))
        sub = s.name + " · ";
    return sub + view::formatBp(s.length()) + " · " + (s.circular ? "circular" : "linear");
}

Window::Window(Shell *shell) : m_shell(shell), m_window(shell->widget())
{
    m_mapStacked = settingBool("Layout", "mapStacked", false);
    m_mapHidden = settingBool("Layout", "mapHidden", false);
    const std::string pane = settingString("Layout", "pane");
    if (pane == "features" || pane == "primers")
        m_tablePane = pane;
    m_map = std::make_unique<MapView>(&m_doc);
    // Ours until the view goes (see ~Window): the page's widgets are gone
    // before this is torn down, and the view still draws into it then.
    g_object_ref_sink(m_map->widget());
    m_seq = std::make_unique<SequenceView>(&m_doc);
    m_map->setZoom(savedZoom());
    m_seq->setZoom(savedZoom());
    buildUi();
    rebuildEnzymeSets();
    m_doc.onSequenceChanged([this] {
        if (m_primerEditor)
            m_primerEditor->sequenceChanged();
        if (m_pcr)
            m_pcr->templateChanged();
        if (m_gel)
            m_gel->templateChanged();
        rebuildFeatureList();
        rebuildPrimerList();
        rebuildEnzymeList();
        updateTitle();
        gtk_stack_set_visible_child_name(GTK_STACK(m_stack), m_doc.isEmpty() ? "empty" : "document");
        // The chromatogram only fits the bases it called.
        if (m_traceBox)
            gtk_widget_set_visible(m_traceBox, m_trace && m_trace->bases == m_doc.sequence().bases);
    });
    m_doc.onEnzymesChanged([this] { refreshEnzymeRows(); });
    m_doc.onReadsChanged([this] { updateActions(); });
    m_doc.onSelectionChanged([this] {
        m_statusMessage.clear();
        updateStatus();
        updateActions();
        scrollTraceToSelection();
    });
    m_doc.onHistoryChanged([this] {
        updateTitle();
        updateActions();
    });
    applyLayout();
    updateTitle();
    updateStatus();
    updateActions();
}

Window::~Window()
{
    // Before the views: dropping the editor clears its preview, which they draw.
    if (m_endPrimerIdle)
        g_source_remove(m_endPrimerIdle);
    closeTools();
    m_primerEditor.reset();
    if (m_panelAnim) {
        // Resetting moves it to its start, which no longer reaches the paned:
        // that's gone by now.
        AdwAnimation *anim = std::exchange(m_panelAnim, nullptr);
        g_signal_handlers_disconnect_by_data(anim, this);
        adw_animation_reset(anim);
        g_object_unref(anim);
    }
    if (m_badgeTimer)
        g_source_remove(m_badgeTimer);
    if (m_settleTimer)
        g_source_remove(m_settleTimer);
    if (m_closeAlert) {
        g_signal_handlers_disconnect_by_data(m_closeAlert, this);
        adw_dialog_force_close(std::exchange(m_closeAlert, nullptr));
    }
    // The views first: the sequence pane they draw in is ours to drop.
    GtkWidget *mapWidget = m_map ? m_map->widget() : nullptr;
    m_seq.reset();
    m_map.reset();
    if (mapWidget)
        g_object_unref(mapWidget);
    if (m_seqPane)
        g_object_unref(m_seqPane);
}

void Window::addActions(GActionMap *map, Shell *shell)
{
#define OC_ACT(name, fn) {name, &onCurrent<&Window::fn>, nullptr, nullptr, nullptr, {0, 0, 0}}
#define OC_STATE(name, type, state, fn) {name, nullptr, type, state, &onCurrent<&Window::fn>, {0, 0, 0}}
    static const GActionEntry entries[] = {
        OC_ACT("save", actSave),
        OC_ACT("save-as", actSaveAs),
        OC_ACT("undo", actUndo),
        OC_ACT("redo", actRedo),
        OC_ACT("cut", actCut),
        OC_ACT("paste", actPaste),
        OC_ACT("select-all", actSelectAll),
        OC_ACT("reverse-complement", actReverseComplement),
        OC_ACT("set-origin", actSetOrigin),
        OC_STATE("circular", nullptr, "false", actCircular),
        OC_ACT("add-feature", actAddFeature),
        OC_ACT("edit-feature", actEditFeature),
        OC_ACT("delete-feature", actDeleteFeature),
        OC_ACT("add-primer", actAddPrimer),
        OC_ACT("digest", actDigest),
        OC_ACT("pcr", actPcr),
        OC_ACT("clone", actClone),
        OC_ACT("golden-gate", actGoldenGate),
        OC_ACT("gibson", actGibson),
        OC_ACT("design-primers", actDesignPrimers),
        OC_ACT("choose-enzymes", actChooseEnzymes),
        OC_ACT("mutagenesis", actMutagenesis),
        OC_ACT("history", actHistory),
        OC_ACT("align", actAlign),
        OC_ACT("compare", actCompare),
        OC_ACT("orfs", actOrfs),
        OC_ACT("protein", actProtein),
        OC_ACT("map-options", actMapOptions),
        OC_ACT("export-fasta", actExportFasta),
        OC_ACT("export-genbank", actExportGenBank),
        OC_ACT("export-map", actExportMap),
        OC_ACT("print", actPrint),
        OC_ACT("search-folder", actSearchFolder),
        OC_ACT("ta-cloning", actTaCloning),
        OC_ACT("overlap-extension", actOverlapExtension),
        OC_ACT("insertion", actInsertion),
        OC_ACT("copy", actCopy),
        OC_ACT("copy-rc", actCopyRc),
        OC_ACT("find", actFind),
        OC_ACT("close", actClose),
        OC_STATE("show-complement", nullptr, "true", actComplement),
        OC_STATE("frames", "s", "'0'", actFrames),
        OC_ACT("clear-reads", actClearReads),
        OC_STATE("group-tens", nullptr, "false", actGroupTens),
        OC_ACT("zoom-in", actZoomIn),
        OC_ACT("zoom-out", actZoomOut),
        OC_ACT("zoom-reset", actZoomReset),
        OC_ACT("revert", actRevert),
        OC_ACT("copy-map", actCopyMap),
        OC_ACT("go-to", actGoTo),
        OC_ACT("rename", actRename),
        OC_ACT("annotate", actAnnotate),
        OC_ACT("delete-primer", actDeletePrimer),
        OC_ACT("check-primer", actCheckPrimer),
        OC_STATE("show-panel", nullptr, "true", actShowPanel),
        OC_STATE("show-map", nullptr, "true", actShowMap),
        OC_STATE("stack-map", nullptr, "false", actStackMap),
        // The window's own: they need no document.
        {"sidebar", nullptr, nullptr, "true", &Window::actToggleSidebar, {0, 0, 0}},
        {"show-folder", &Window::actShowFolder, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"fullscreen", nullptr, nullptr, "false", &Window::actFullscreen, {0, 0, 0}},
    };
#undef OC_ACT
#undef OC_STATE
    g_action_map_add_action_entries(map, entries, G_N_ELEMENTS(entries), shell);
}

void Window::buildUi()
{
    m_page = gtk_overlay_new();
    g_object_set_data_full(G_OBJECT(m_page), "oc-page", this, [](gpointer p) { delete static_cast<Window *>(p); });

    // The sequence, with its find bar floating at the bottom. It moves between
    // the middle and the panel, so it is kept alive while it has no parent.
    m_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_search), "Find bases (both strands)");
    gtk_widget_set_size_request(m_search, 260, -1);
    g_signal_connect(m_search, "activate", G_CALLBACK(&Window::onSearchActivate), this);
    g_signal_connect_swapped(m_search, "stop-search", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->showFindBar(false);
                             }),
                             this);
    GtkWidget *closeFind = capsuleButton("window-close-symbolic", "Close the find bar (Esc)");
    g_signal_connect_swapped(closeFind, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->showFindBar(false);
                             }),
                             this);
    GtkWidget *findBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(findBox, "restricto-capsule");
    gtk_box_append(GTK_BOX(findBox), m_search);
    gtk_box_append(GTK_BOX(findBox), closeFind);
    m_findBar = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(m_findBar), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_child(GTK_REVEALER(m_findBar), findBox);
    gtk_widget_set_halign(m_findBar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(m_findBar, GTK_ALIGN_END);
    gtk_widget_set_visible(m_findBar, FALSE);
    m_seqPane = gtk_overlay_new();
    g_object_ref_sink(m_seqPane);
    gtk_overlay_set_child(GTK_OVERLAY(m_seqPane), m_seq->widget());
    gtk_overlay_add_overlay(GTK_OVERLAY(m_seqPane), m_findBar);

    // Under the map and sequence: the chromatogram of a trace file.
    m_traceAdj = gtk_adjustment_new(0, 0, 1, 20, 200, 1);
    m_traceArea = gtk_drawing_area_new();
    gtk_widget_set_size_request(m_traceArea, -1, 190);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_traceArea), &Window::drawTrace, this, nullptr);
    g_signal_connect_swapped(m_traceAdj, "value-changed", G_CALLBACK(gtk_widget_queue_draw), m_traceArea);
    GtkGesture *traceClick = gtk_gesture_click_new();
    g_signal_connect(traceClick, "pressed", G_CALLBACK((+[](GtkGestureClick *, int, double x, double, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         if (!self->m_trace)
                             return;
                         const view::TraceLayout layout(*self->m_trace, view::Theme::light(), 190);
                         const int b = layout.baseAt(x + gtk_adjustment_get_value(self->m_traceAdj));
                         if (b >= 0 && b < self->m_doc.sequence().length())
                             self->m_doc.setSelection({b, 1});
                     })),
                     this);
    gtk_widget_add_controller(m_traceArea, GTK_EVENT_CONTROLLER(traceClick));
    GtkEventController *traceScroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(traceScroll, "scroll", G_CALLBACK((+[](GtkEventControllerScroll *, double dx, double dy, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         gtk_adjustment_set_value(self->m_traceAdj,
                                                  gtk_adjustment_get_value(self->m_traceAdj) + (dx + dy) * 40);
                         return gboolean(TRUE);
                     })),
                     this);
    gtk_widget_add_controller(m_traceArea, traceScroll);
    m_traceBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(m_traceBox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(m_traceBox), m_traceArea);
    gtk_box_append(GTK_BOX(m_traceBox), gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, m_traceAdj));
    gtk_widget_set_visible(m_traceBox, FALSE);

    // The middle: the map, beside or over the sequence (or the sequence alone).
    m_split = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(m_split), m_map->widget());
    gtk_paned_set_shrink_start_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(m_split), FALSE);
    gtk_widget_set_vexpand(m_split, TRUE);
    g_signal_connect_swapped(m_split, "notify::max-position", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->placeSplit();
                             }),
                             this);
    GtkWidget *middleBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(middleBox), m_split);
    gtk_box_append(GTK_BOX(middleBox), m_traceBox);

    // The pill at the top right picks the panel's pane.
    m_pill = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(m_pill, "restricto-capsule");
    gtk_widget_add_css_class(m_pill, "vertical");
    gtk_widget_set_halign(m_pill, GTK_ALIGN_END);
    gtk_widget_set_valign(m_pill, GTK_ALIGN_START);
    gtk_widget_set_margin_top(m_pill, 10);
    gtk_widget_set_margin_end(m_pill, 8);
    struct PillEntry { const char *pane, *icon, *tip; };
    for (const PillEntry &e : {PillEntry{"sequence", "restricto-sequence-symbolic", "Sequence"},
                               PillEntry{"enzymes", "restricto-enzymes-symbolic", "Enzymes"},
                               PillEntry{"features", "restricto-features-symbolic", "Features"},
                               PillEntry{"primers", "restricto-primers-symbolic", "Primers"}}) {
        GtkWidget *b = capsuleButton(e.icon, e.tip);
        g_object_set_data(G_OBJECT(b), "pane", const_cast<char *>(e.pane));
        g_signal_connect(b, "clicked", G_CALLBACK((+[](GtkButton *b, gpointer p) {
                             static_cast<Window *>(p)->pillClicked(
                                 static_cast<const char *>(g_object_get_data(G_OBJECT(b), "pane")));
                         })),
                         this);
        gtk_box_append(GTK_BOX(m_pill), b);
        m_pillButtons.emplace_back(e.pane, b);
    }

    // The status at the bottom: view toggles, and beside them, while there is
    // something to show, the caret or selection and the base under the mouse.
    GtkWidget *toggles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toggles, "restricto-capsule");
    struct ToggleEntry { const char *icon, *tip; };
    int which = 0;
    for (const ToggleEntry &e : {ToggleEntry{"restricto-complement-symbolic", "Complement strand"},
                                 ToggleEntry{"restricto-translation-symbolic",
                                             "Translation of open reading frames (green forward, red reverse)"},
                                 ToggleEntry{"restricto-enzymes-symbolic", "Restriction sites"},
                                 ToggleEntry{"restricto-features-symbolic", "Features"}}) {
        GtkWidget *b = capsuleButton(e.icon, e.tip);
        g_object_set_data(G_OBJECT(b), "toggle", GINT_TO_POINTER(which++));
        g_signal_connect(b, "clicked", G_CALLBACK((+[](GtkButton *b, gpointer p) {
                             static_cast<Window *>(p)->toggleView(
                                 GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "toggle")));
                         })),
                         this);
        gtk_box_append(GTK_BOX(toggles), b);
        m_toggles.push_back(b);
    }
    m_status = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(m_status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(m_status, 6);
    gtk_widget_set_margin_end(m_status, 6);
    gtk_widget_set_margin_top(m_status, 4);
    gtk_widget_set_margin_bottom(m_status, 4);
    m_info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(m_info, "restricto-capsule");
    gtk_box_append(GTK_BOX(m_info), m_status);
    gtk_widget_set_visible(m_info, FALSE);
    m_statusRow = toggles;

    // The toggles stay centred whether or not the second capsule shows (so
    // they don't move under the pointer); the second sits to their right, or
    // above them when there is no room beside them.
    m_middle = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(m_middle), middleBox);
    gtk_overlay_add_overlay(GTK_OVERLAY(m_middle), m_pill);
    gtk_overlay_add_overlay(GTK_OVERLAY(m_middle), m_statusRow);
    gtk_overlay_add_overlay(GTK_OVERLAY(m_middle), m_info);
    g_signal_connect(m_middle, "get-child-position",
                     G_CALLBACK((+[](GtkOverlay *overlay, GtkWidget *child, GdkRectangle *at, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         if (child != self->m_statusRow && child != self->m_info)
                             return gboolean(FALSE);
                         const int width = gtk_widget_get_width(GTK_WIDGET(overlay));
                         const int height = gtk_widget_get_height(GTK_WIDGET(overlay));
                         int tw = 0, th = 0, iw = 0, ih = 0, unused = 0;
                         gtk_widget_measure(self->m_statusRow, GTK_ORIENTATION_HORIZONTAL, -1, &unused, &tw, nullptr, nullptr);
                         gtk_widget_measure(self->m_statusRow, GTK_ORIENTATION_VERTICAL, -1, &unused, &th, nullptr, nullptr);
                         const int tx = std::max(0, (width - tw) / 2), ty = std::max(0, height - th - 12);
                         if (child == self->m_statusRow) {
                             *at = {tx, ty, tw, th};
                             return gboolean(TRUE);
                         }
                         int minw = 0;
                         gtk_widget_measure(self->m_info, GTK_ORIENTATION_HORIZONTAL, -1, &minw, &iw, nullptr, nullptr);
                         gtk_widget_measure(self->m_info, GTK_ORIENTATION_VERTICAL, -1, &unused, &ih, nullptr, nullptr);
                         if (tx + tw + 10 + iw <= width - 12) {
                             *at = {tx + tw + 10, ty + (th - ih) / 2, iw, ih};
                         } else {
                             const int w = std::max(minw, std::min(iw, width - 24));
                             *at = {std::max(0, (width - w) / 2), std::max(0, ty - ih - 8), w, ih};
                         }
                         return gboolean(TRUE);
                     })),
                     this);
    gtk_widget_set_size_request(m_middle, 300, -1);

    m_panel = buildPanel();
    m_outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(m_outer), m_middle);
    gtk_paned_set_end_child(GTK_PANED(m_outer), m_panel);
    gtk_paned_set_resize_end_child(GTK_PANED(m_outer), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(m_outer), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(m_outer), FALSE);
    g_signal_connect_swapped(m_outer, "notify::max-position", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->placePanel();
                             }),
                             this);

    // Map and sequence: the base under the mouse, the selection badge, and a
    // double-click on a feature edits it.
    auto hover = [this](int base) {
        m_hoverBase = base;
        updateStatus();
    };
    m_map->onHoverBase = hover;
    m_seq->onHoverBase = hover;
    m_map->onSelectDrag = [this](double x, double y) { showBadge(m_map->widget(), x, y); };
    m_seq->onSelectDrag = [this](double x, double y) { showBadge(m_seq->widget(), x, y); };
    auto edit = [this](int index) { featureDialog(index); };
    m_map->onEditFeature = edit;
    m_seq->onEditFeature = edit;

    // Empty state.
    GtkWidget *empty = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "io.github.mrgarbett.Restricto");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Open a Sequence");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty),
                                    "GenBank, FASTA and SnapGene .dna files.\n"
                                    "Open a file from the sidebar, or drop one here.\n\n"
                                    "Restricto " RESTRICTO_VERSION);
    GtkWidget *openPill = gtk_button_new_with_label("Open…");
    gtk_widget_add_css_class(openPill, "pill");
    gtk_widget_add_css_class(openPill, "suggested-action");
    gtk_widget_set_halign(openPill, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(openPill), "app.open");
    adw_status_page_set_child(ADW_STATUS_PAGE(empty), openPill);

    m_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(m_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(m_stack), m_outer, "document");
    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "empty");
    gtk_overlay_set_child(GTK_OVERLAY(m_page), m_stack);

    // The selection badge follows the mouse while bases are being selected.
    m_badgeLabel = gtk_label_new("");
    m_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(m_badge, "restricto-badge");
    gtk_box_append(GTK_BOX(m_badge), m_badgeLabel);
    gtk_widget_set_halign(m_badge, GTK_ALIGN_START);
    gtk_widget_set_valign(m_badge, GTK_ALIGN_START);
    gtk_widget_set_can_target(m_badge, FALSE);
    gtk_widget_set_visible(m_badge, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(m_page), m_badge);

    // Follow the system light/dark preference.
    AdwStyleManager *sm = adw_style_manager_get_default();
    g_signal_connect_object(sm, "notify::dark", G_CALLBACK(&Window::onDarkChanged), m_page, GConnectFlags(0));
    onDarkChanged(G_OBJECT(sm), nullptr, m_page);
}

GtkWidget *Window::buildPanel()
{
    m_panelStack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(m_panelStack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(m_panelStack, TRUE);

    // Sequence: holds the sequence while the map sits beside it.
    m_sequenceHolder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(m_panelStack), m_sequenceHolder, "sequence");

    // Enzymes.
    GtkWidget *ebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    m_enzymeSearch = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_enzymeSearch), "Find enzyme");
    gtk_widget_set_margin_start(m_enzymeSearch, 8);
    gtk_widget_set_margin_end(m_enzymeSearch, 8);
    gtk_widget_set_margin_top(m_enzymeSearch, 10);
    g_signal_connect_swapped(m_enzymeSearch, "search-changed", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->applyEnzymeSearch();
                             }),
                             this);
    static const char *filters[] = {"Unique cutters", "Up to 2 cuts", "Up to 3 cuts", "All cutters", nullptr};
    m_filter = gtk_drop_down_new_from_strings(filters);
    gtk_widget_set_hexpand(m_filter, TRUE);
    g_signal_connect(m_filter, "notify::selected", G_CALLBACK(&Window::onFilterChanged), this);
    m_onMapOnly = gtk_check_button_new_with_label("On map only");
    gtk_widget_set_tooltip_text(m_onMapOnly, "List only the enzymes shown on the map");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(m_onMapOnly), settingBool("Layout", "enzymesOnMapOnly", false));
    g_signal_connect_swapped(m_onMapOnly, "toggled", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<Window *>(p);
                                 setSettingBool("Layout", "enzymesOnMapOnly",
                                                gtk_check_button_get_active(GTK_CHECK_BUTTON(self->m_onMapOnly)));
                                 self->applyEnzymeSearch();
                             }),
                             this);
    GtkWidget *filterRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(filterRow, 8);
    gtk_widget_set_margin_end(filterRow, 8);
    gtk_box_append(GTK_BOX(filterRow), m_filter);
    gtk_box_append(GTK_BOX(filterRow), m_onMapOnly);
    m_enzymeList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_enzymeList), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_enzymeList, "navigation-sidebar");
    g_signal_connect(m_enzymeList, "row-activated", G_CALLBACK(&Window::onEnzymeRowActivated), this);
    GtkWidget *credit = gtk_label_new("Enzyme data: REBASE, rebase.neb.com");
    gtk_widget_add_css_class(credit, "dim-label");
    gtk_widget_add_css_class(credit, "caption");
    gtk_widget_set_margin_bottom(credit, 6);
    // Enzyme sets: built-in (Type IIS, by supplier) and saved ones.
    GtkWidget *setRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(setRow, 8);
    gtk_widget_set_margin_end(setRow, 8);
    m_setDrop = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(nullptr)), nullptr);
    gtk_widget_set_hexpand(m_setDrop, TRUE);
    gtk_widget_set_tooltip_text(m_setDrop, "Enzyme set");
    g_signal_connect(m_setDrop, "notify::selected", G_CALLBACK(&Window::onEnzymeSetChanged), this);
    GtkWidget *saveSet = gtk_button_new_from_icon_name("document-save-symbolic");
    gtk_widget_set_tooltip_text(saveSet, "Save the checked enzymes as a set");
    g_signal_connect_swapped(saveSet, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->saveEnzymeSetDialog();
                             }),
                             this);
    gtk_box_append(GTK_BOX(setRow), m_setDrop);
    gtk_box_append(GTK_BOX(setRow), saveSet);
    gtk_box_append(GTK_BOX(ebox), m_enzymeSearch);
    gtk_box_append(GTK_BOX(ebox), filterRow);
    gtk_box_append(GTK_BOX(ebox), setRow);
    gtk_box_append(GTK_BOX(ebox), scrolled(m_enzymeList));
    gtk_box_append(GTK_BOX(ebox), credit);
    gtk_stack_add_named(GTK_STACK(m_panelStack), ebox, "enzymes");

    // Features: a click selects one, a double-click edits it.
    m_featureList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_featureList), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_featureList, "navigation-sidebar");
    g_signal_connect(m_featureList, "row-activated", G_CALLBACK(&Window::onFeatureRowActivated), this);
    GtkGesture *twice = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(twice), GTK_PHASE_CAPTURE);
    g_signal_connect(twice, "pressed", G_CALLBACK((+[](GtkGestureClick *, int n, double, double y, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->m_featureList), int(y));
                         if (n == 2 && row)
                             self->featureDialog(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "feature")));
                     })),
                     this);
    gtk_widget_add_controller(m_featureList, GTK_EVENT_CONTROLLER(twice));
    GtkWidget *fscroll = scrolled(m_featureList);
    gtk_widget_set_margin_top(fscroll, 6);
    gtk_stack_add_named(GTK_STACK(m_panelStack), fscroll, "features");

    // Primers.
    m_primerList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_primerList), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_primerList, "navigation-sidebar");
    g_signal_connect(m_primerList, "row-activated", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "primer"));
                         const auto &primers = self->m_doc.sequence().primers;
                         if (i >= 0 && i < int(primers.size()))
                             self->m_doc.setSelection({primers[size_t(i)].binding.start,
                                                       primers[size_t(i)].binding.length()});
                     })),
                     this);
    GtkWidget *pscroll = scrolled(m_primerList);
    gtk_widget_set_vexpand(pscroll, TRUE);
    // Add Primer sits at the top, where it's seen.
    GtkWidget *addPrimer = gtk_button_new();
    GtkWidget *addContent = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(addContent), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(addContent), "Add Primer");
    gtk_button_set_child(GTK_BUTTON(addPrimer), addContent);
    gtk_widget_add_css_class(addPrimer, "suggested-action");
    gtk_widget_set_halign(addPrimer, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(addPrimer, "Add a primer: type it here and see where it binds");
    gtk_widget_set_margin_start(addPrimer, 10);
    gtk_widget_set_margin_top(addPrimer, 10);
    gtk_widget_set_margin_bottom(addPrimer, 4);
    g_signal_connect_swapped(addPrimer, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Window *>(p)->beginAddingPrimer("");
                             }),
                             this);
    GtkWidget *pcr = gtk_button_new_with_label("PCR…");
    gtk_widget_set_tooltip_text(pcr, "Make a PCR product from two of these primers");
    gtk_widget_set_margin_end(pcr, 10);
    gtk_widget_set_margin_top(pcr, 10);
    gtk_widget_set_margin_bottom(pcr, 4);
    gtk_widget_set_hexpand(pcr, TRUE);
    gtk_widget_set_halign(pcr, GTK_ALIGN_END);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(pcr), "win.pcr");
    GtkWidget *primerBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(primerBar), addPrimer);
    gtk_box_append(GTK_BOX(primerBar), pcr);
    GtkWidget *primerBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(primerBox), primerBar);
    gtk_box_append(GTK_BOX(primerBox), pscroll);
    m_primerStack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(m_primerStack), primerBox, "list");
    gtk_stack_add_named(GTK_STACK(m_panelStack), m_primerStack, "primers");

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(panel, "view");
    gtk_box_append(GTK_BOX(panel), m_panelStack);
    gtk_widget_set_size_request(panel, 280, -1);
    return panel;
}

// ---- layout ------------------------------------------------------------------------

void Window::applyLayout()
{
    const bool inPanel = !m_mapStacked && !m_mapHidden;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(m_split),
                                   m_mapStacked ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_visible(m_map->widget(), !m_mapHidden);
    GtkWidget *parent = gtk_widget_get_parent(m_seqPane);
    if (inPanel) {
        if (parent == m_split)
            gtk_paned_set_end_child(GTK_PANED(m_split), nullptr);
        if (parent != m_sequenceHolder) {
            gtk_widget_set_vexpand(m_seqPane, TRUE);
            gtk_box_append(GTK_BOX(m_sequenceHolder), m_seqPane);
        }
    } else {
        if (parent == m_sequenceHolder)
            gtk_box_remove(GTK_BOX(m_sequenceHolder), m_seqPane);
        if (parent != m_split)
            gtk_paned_set_end_child(GTK_PANED(m_split), m_seqPane);
    }
    // The pill and the status float over the content: whatever is under the
    // pill's corner lays out short of it, and the map's labels stay clear of
    // the status at the bottom, but both still fill the middle edge to edge.
    // In the middle, the sequence scrolls clear of the status floating over it.
    m_seq->setTrailingInset(!inPanel && m_mapHidden ? kPillRoom : 0);
    m_seq->setBottomInset(inPanel ? 0 : 96);
    gtk_widget_set_margin_bottom(m_findBar, inPanel ? 12 : 100);
    m_map->setInsets(kPillRoom, m_mapStacked ? 0 : 52);
    gtk_widget_set_margin_end(m_pill, 8 + (!inPanel && m_mapHidden ? m_seq->scrollbarWidth() : 0));
    if (inPanel)
        showPane("sequence");
    else if (m_pane == "sequence")
        showPane(m_tablePane);
    refreshPill();
    // The panel opens to about half the room when it holds the sequence.
    m_panelWant = inPanel ? -1 : 320;
    m_placeSplit = true;
    placePanel();
    placeSplit();
}

void Window::placePanel()
{
    // max-position leaves out the panel's minimum width; the paned's own
    // width is what to split.
    int max = 0;
    g_object_get(m_outer, "max-position", &max, nullptr);
    const int width = gtk_widget_get_width(m_outer);
    if (m_panelWant == 0 || width <= 0 || max <= 0 || max == G_MAXINT)
        return;
    const int want = m_panelWant < 0 ? std::max(420, width / 2) : m_panelWant;
    gtk_paned_set_position(GTK_PANED(m_outer), std::clamp(width - want, 300, max));
    settle();
}

void Window::placeSplit()
{
    int max = 0;
    g_object_get(m_split, "max-position", &max, nullptr);
    const bool across = gtk_orientable_get_orientation(GTK_ORIENTABLE(m_split)) == GTK_ORIENTATION_HORIZONTAL;
    const int length = across ? gtk_widget_get_width(m_split) : gtk_widget_get_height(m_split);
    if (!m_placeSplit || length <= 0 || max <= 0 || max == G_MAXINT)
        return;
    // Map 45 %, sequence the rest.
    gtk_paned_set_position(GTK_PANED(m_split), std::min(max, int(length * 0.45)));
    settle();
}

// The sizes are placed again while the window settles (it may be laid out a
// few times as it opens); after that, a divider the user drags stays put.
void Window::settle()
{
    if (m_settleTimer)
        return;
    m_settleTimer = g_timeout_add(400, [](gpointer p) -> gboolean {
        auto *self = static_cast<Window *>(p);
        self->m_settleTimer = 0;
        self->m_panelWant = 0;
        self->m_placeSplit = false;
        return G_SOURCE_REMOVE;
    }, this);
}

void Window::setMapStacked(bool stacked)
{
    m_mapStacked = stacked;
    setSettingBool("Layout", "mapStacked", stacked);
    applyLayout();
    updateActions();
}

void Window::setMapHidden(bool hidden)
{
    m_mapHidden = hidden;
    setSettingBool("Layout", "mapHidden", hidden);
    applyLayout();
    updateActions();
}

void Window::setPanelShown(bool shown)
{
    const bool sliding = m_panelAnim && adw_animation_get_state(m_panelAnim) == ADW_ANIMATION_PLAYING;
    m_panelShown = shown;
    refreshPill();
    updateActions();
    const int width = gtk_widget_get_width(m_outer);
    if (!gtk_widget_get_mapped(m_outer) || width <= 0) {
        gtk_widget_set_visible(m_panel, shown);
        return;
    }
    if (!sliding && shown == gtk_widget_get_visible(m_panel))
        return;
    if (shown && (m_panelRest < 300 || m_panelRest > width - 200)) {
        // Never shown yet, or the window has changed size since: the panel's
        // usual width.
        const bool inPanel = !m_mapStacked && !m_mapHidden;
        m_panelRest = std::max(300, width - (inPanel ? std::max(420, width / 2) : 320));
    }
    // Slide the divider: from the edge to where it rests, or back to the edge.
    int from = gtk_paned_get_position(GTK_PANED(m_outer));
    if (!sliding) {
        if (shown) {
            from = width;
            gtk_paned_set_position(GTK_PANED(m_outer), width);
            gtk_widget_set_visible(m_panel, TRUE);
        } else {
            m_panelRest = from;
        }
    }
    if (!m_panelAnim) {
        AdwAnimationTarget *target = adw_callback_animation_target_new(
            +[](double v, gpointer p) {
                auto *self = static_cast<Window *>(p);
                if (self->m_panelAnim)
                    gtk_paned_set_position(GTK_PANED(self->m_outer), int(std::lround(v)));
            },
            this, nullptr);
        m_panelAnim = adw_timed_animation_new(m_outer, 0, 1, 220, target);
        adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(m_panelAnim), ADW_EASE_OUT_CUBIC);
        g_signal_connect(m_panelAnim, "done", G_CALLBACK(+[](AdwAnimation *, gpointer p) {
                             auto *self = static_cast<Window *>(p);
                             gtk_paned_set_shrink_end_child(GTK_PANED(self->m_outer), FALSE);
                             if (!self->m_panelShown) {
                                 gtk_widget_set_visible(self->m_panel, FALSE);
                                 gtk_paned_set_position(GTK_PANED(self->m_outer), self->m_panelRest);
                             }
                         }),
                         this);
    }
    // While it slides, the panel may be narrower than it can be laid out.
    gtk_paned_set_shrink_end_child(GTK_PANED(m_outer), TRUE);
    adw_animation_reset(m_panelAnim);
    adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(m_panelAnim), from);
    adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(m_panelAnim), shown ? m_panelRest : width);
    adw_animation_play(m_panelAnim);
}

void Window::showPane(const std::string &pane)
{
    const bool inPanel = !m_mapStacked && !m_mapHidden;
    m_pane = pane == "sequence" && !inPanel ? m_tablePane : pane;
    gtk_stack_set_visible_child_name(GTK_STACK(m_panelStack), m_pane.c_str());
    refreshPill();
}

void Window::pillClicked(const std::string &pane)
{
    const bool wasShowing = pane == m_pane;
    if (pane != "sequence") {
        m_tablePane = pane;
        setSettingString("Layout", "pane", pane);
    }
    showPane(pane);
    // A hidden panel opens on the pane picked; picking the pane already
    // showing hides the panel again.
    if (!m_panelShown)
        setPanelShown(true);
    else if (wasShowing)
        setPanelShown(false);
}

void Window::refreshPill()
{
    const bool inPanel = !m_mapStacked && !m_mapHidden;
    for (const auto &[pane, button] : m_pillButtons) {
        setOn(button, m_panelShown && pane == m_pane);
        if (pane == "sequence")
            gtk_widget_set_visible(button, inPanel);
    }
}

void Window::toggleView(int which)
{
    view::MapOptions options = m_map->options();
    switch (which) {
    case 0:
        m_seq->setShowComplement(!m_seq->showComplement());
        break;
    case 1:
        options.frames = m_seq->frames() == 0 ? 6 : 0;
        m_seq->setFrames(options.frames);
        break;
    case 2:
        options.enzymes = !options.enzymes;
        m_seq->setShowEnzymes(options.enzymes);
        break;
    default:
        // Hide every feature type, or show them all again.
        if (options.hiddenTypes.empty()) {
            for (const Feature &f : m_doc.sequence().features)
                options.hiddenTypes.insert(f.type);
        } else {
            options.hiddenTypes.clear();
        }
        m_seq->setHiddenTypes(options.hiddenTypes);
        break;
    }
    m_map->setOptions(options);
    updateActions();
}

void Window::refreshToggles()
{
    const view::MapOptions &o = m_map->options();
    const bool on[] = {m_seq->showComplement(), m_seq->frames() != 0, o.enzymes, o.hiddenTypes.empty()};
    for (size_t i = 0; i < m_toggles.size() && i < 4; ++i)
        setOn(m_toggles[i], on[i]);
}

void Window::showFindBar(bool show)
{
    if (show) {
        // With the map beside it, the sequence is in the panel: show it there.
        if (!m_mapStacked && !m_mapHidden) {
            if (!m_panelShown)
                setPanelShown(true);
            showPane("sequence");
        }
        gtk_widget_set_visible(m_findBar, TRUE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(m_findBar), TRUE);
        gtk_widget_grab_focus(m_search);
    } else {
        gtk_revealer_set_reveal_child(GTK_REVEALER(m_findBar), FALSE);
        gtk_widget_set_visible(m_findBar, FALSE);
        gtk_widget_grab_focus(m_seq->widget());
    }
}

void Window::showBadge(GtkWidget *from, double x, double y)
{
    if (m_badgeTimer) {
        g_source_remove(m_badgeTimer);
        m_badgeTimer = 0;
    }
    if (x < 0 && y < 0) {
        // Fade after a moment, so the numbers can still be read.
        m_badgeTimer = g_timeout_add(1500, [](gpointer p) -> gboolean {
            auto *self = static_cast<Window *>(p);
            self->m_badgeTimer = 0;
            fade(self->m_badge, false);
            return G_SOURCE_REMOVE;
        }, this);
        return;
    }
    if (!m_doc.hasSelection() || m_doc.selection().length <= 0) {
        fade(m_badge, false);
        return;
    }
    const Span s = m_doc.selection();
    const int n = m_doc.sequence().length();
    int end = s.start + s.length;
    if (end > n)
        end -= n;
    std::string text = view::withThousands(s.start + 1) + "–" + view::withThousands(end) + "  ·  "
                     + view::formatBp(s.length);
    // Q5's Tm, as the status capsule and the primer editor show it.
    if (s.length >= 8 && s.length <= 100) {
        const int tm = polymeraseTm(cleanBases(m_doc.selectedBases()), Polymerase::Q5);
        if (tm > 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "  ·  Q5 Tm %d °C", tm);
            text += buf;
        }
    }
    gtk_label_set_text(GTK_LABEL(m_badgeLabel), text.c_str());
    graphene_point_t at;
    const graphene_point_t pt = GRAPHENE_POINT_INIT(float(x), float(y));
    if (!gtk_widget_compute_point(from, m_page, &pt, &at))
        return;
    // Keep it inside the window: to the left of the pointer near the right edge
    // (measured once shown: a hidden widget measures 0).
    if (!gtk_widget_get_visible(m_badge))
        gtk_widget_set_opacity(m_badge, 0);
    gtk_widget_set_visible(m_badge, TRUE);
    // Measured without the last margin, which would count as its width.
    gtk_widget_set_margin_start(m_badge, 0);
    int bw = 0, unused = 0;
    gtk_widget_measure(m_badge, GTK_ORIENTATION_HORIZONTAL, -1, &unused, &bw, nullptr, nullptr);
    int bx = int(at.x) + 12;
    if (bx + bw > gtk_widget_get_width(m_page) - 4)
        bx = int(at.x) - 12 - bw;
    gtk_widget_set_margin_start(m_badge, std::max(0, bx));
    gtk_widget_set_margin_top(m_badge, std::max(0, int(at.y) - 34));
    fade(m_badge, true);
}

void Window::applyEnzymeSearch()
{
    gchar *folded = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(m_enzymeSearch)), -1);
    const std::string query = folded;
    g_free(folded);
    const bool onMapOnly = gtk_check_button_get_active(GTK_CHECK_BUTTON(m_onMapOnly));
    for (const EnzymeRow &r : m_enzymeRows) {
        const bool matches = query.empty() || toLower(r.name).find(query) != std::string::npos;
        const bool onMap = m_doc.enzymePassesFilter(r.name) && !m_doc.isEnzymeHidden(r.name);
        gtk_widget_set_visible(r.row, matches && (!onMapOnly || onMap));
    }
}

void Window::onDarkChanged(GObject *sm, GParamSpec *, gpointer page)
{
    Window *self = static_cast<Window *>(g_object_get_data(G_OBJECT(page), "oc-page"));
    if (!self)
        return;
    const bool dark = adw_style_manager_get_dark(ADW_STYLE_MANAGER(sm));
    self->m_map->setDark(dark);
    self->m_seq->setDark(dark);
}

bool Window::load(const std::string &path, std::string *error)
{
    Sequence seq;
    std::vector<Sequence> more;
    FileFormat format = FileFormat::Unknown;
    if (!loadSequenceFile(path, &seq, &more, error, &format))
        return false;
    // A trace can't be written back, and saving one record would drop the
    // others from a multi-record file: those save to a new file.
    m_doc.setSequence(std::move(seq), format == FileFormat::Abif || !more.empty() ? std::string() : path);
    for (Sequence &extra : more) {
        Window *w = Window::create(gtk_window_get_application(gtkWindow()));
        w->document().setSequence(std::move(extra));
        w->present();
    }
    if (!more.empty())
        toast(baseName(path) + " holds " + std::to_string(more.size() + 1) + " records; each opened in its own tab");
    std::unique_ptr<Trace> trace;
    std::string data;
    if (format == FileFormat::Abif && readFile(path, &data)) {
        trace = std::make_unique<Trace>();
        if (!readAbif(data, trace.get()))
            trace.reset();
    }
    showTrace(std::move(trace));
    return true;
}

void Window::showTrace(std::unique_ptr<Trace> trace)
{
    m_trace = std::move(trace);
    gtk_widget_set_visible(m_traceBox, m_trace != nullptr);
    if (!m_trace)
        return;
    const view::TraceLayout layout(*m_trace, view::Theme::light(), 190);
    gtk_adjustment_configure(m_traceAdj, 0, 0, layout.totalWidth(), 40, 400, 400);
    gtk_widget_queue_draw(m_traceArea);
}

void Window::scrollTraceToSelection()
{
    if (!m_trace || !gtk_widget_get_visible(m_traceBox))
        return;
    if (m_doc.hasSelection()) {
        const view::TraceLayout layout(*m_trace, view::Theme::light(), 190);
        const double x = layout.xOfBase(m_doc.selection().start);
        const double v = gtk_adjustment_get_value(m_traceAdj), page = gtk_adjustment_get_page_size(m_traceAdj);
        if (x < v + 20 || x > v + page - 20)
            gtk_adjustment_set_value(m_traceAdj, x - page / 3);
    }
    gtk_widget_queue_draw(m_traceArea);
}

void Window::drawTrace(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (!self->m_trace)
        return;
    const bool dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    const view::TraceLayout layout(*self->m_trace, dark ? view::Theme::darkTheme() : view::Theme::light(), h);
    // Keep the scrollbar's page in step with the width.
    if (gtk_adjustment_get_page_size(self->m_traceAdj) != w)
        gtk_adjustment_configure(self->m_traceAdj, std::min(gtk_adjustment_get_value(self->m_traceAdj),
                                                            std::max(0.0, layout.totalWidth() - w)),
                                 0, layout.totalWidth(), 40, w * 0.8, w);
    const Span sel = self->m_doc.hasSelection() ? self->m_doc.selection() : Span{0, 0};
    view::drawScene(cr, layout.render(gtk_adjustment_get_value(self->m_traceAdj), w, sel));
}

void Window::updateTitle()
{
    if (isCurrent())
        m_shell->refreshChrome();
    else
        m_shell->setTabTitle(this);
    updateStatus();
    // The folder browsers show unsaved documents and red dots.
    const std::string key = m_doc.path() + '\n' + displayName() + (m_doc.isModified() ? "*" : "")
                          + (m_doc.isEmpty() ? "-" : "");
    if (key != m_browserKey) {
        m_browserKey = key;
        Shell::documentsChanged(m_shell->app());
    }
}

void Window::updateStatus()
{
    std::string text = m_statusMessage;
    if (text.empty()) {
        text = m_doc.statusText(false);
        if (m_hoverBase >= 0 && m_hoverBase < m_doc.sequence().length())
            text += (text.empty() ? "" : "  ·  ") + std::string("Base ") + view::withThousands(m_hoverBase + 1);
    }
    gtk_label_set_text(GTK_LABEL(m_status), text.c_str());
    fade(m_info, !text.empty());
}

void Window::rebuildFeatureList()
{
    gtk_list_box_remove_all(GTK_LIST_BOX(m_featureList));
    const Sequence &s = m_doc.sequence();
    for (size_t i = 0; i < s.features.size(); ++i) {
        const Feature &f = s.features[i];
        if (!view::FeatureStyle::isDrawn(f))
            continue;
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), f.name.c_str());
        std::string sub = f.type + " · " + locationText(s, f);
        if (f.strand == Strand::Forward)
            sub += " · →";
        else if (f.strand == Strand::Reverse)
            sub += " · ←";
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub.c_str());
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), swatch(view::FeatureStyle::fill(f.type, f.color)));
        GtkWidget *edit = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_add_css_class(edit, "flat");
        gtk_widget_set_valign(edit, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(edit, "Edit feature");
        g_object_set_data(G_OBJECT(edit), "feature", GINT_TO_POINTER(int(i)));
        g_signal_connect(edit, "clicked", G_CALLBACK((+[](GtkButton *b, gpointer p) {
                             static_cast<Window *>(p)->featureDialog(
                                 GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "feature")));
                         })),
                         this);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "feature", GINT_TO_POINTER(int(i)));
        gtk_list_box_append(GTK_LIST_BOX(m_featureList), row);
    }
}

void Window::rebuildPrimerList()
{
    gtk_list_box_remove_all(GTK_LIST_BOX(m_primerList));
    const Sequence &s = m_doc.sequence();
    for (size_t i = 0; i < s.primers.size(); ++i) {
        const Primer &pr = s.primers[i];
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), pr.name.c_str());
        // Q5's Tm (as NEB's calculator gives it) of the part that anneals.
        const PrimerCheck check = checkPrimer(pr.bases);
        const int annealing = pr.binding.length() > 0 && pr.binding.length() <= int(pr.bases.size())
                                ? pr.binding.length() : int(pr.bases.size());
        const int tm = polymeraseTm(cleanBases(pr.bases.substr(pr.bases.size() - size_t(annealing))), Polymerase::Q5);
        std::string sub = std::to_string(pr.bases.size()) + " nt";
        if (tm != INT_MIN && !std::isnan(check.gc)) {
            char buf[48];
            std::snprintf(buf, sizeof buf, " · Q5 Tm %d °C · GC %.0f%%", tm, check.gc * 100);
            sub += buf;
        }
        if (pr.binding.length() > 0)
            sub += " · " + view::siteText(pr.binding, s.length())
                 + (pr.strand == Strand::Reverse ? " · ←" : " · →");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub.c_str());
        // The Tm warned about is Q5's, as shown.
        std::vector<std::string> warnings;
        for (const std::string &w : check.warnings)
            if (w.rfind("Tm ", 0) != 0)
                warnings.push_back(w);
        if (tm != INT_MIN && (tm < 50 || tm > 72))
            warnings.push_back("Q5 Tm " + std::to_string(tm) + " °C; 50–72 °C is typical");
        if (!warnings.empty()) {
            std::string tip;
            for (const std::string &w : warnings)
                tip += (tip.empty() ? "" : "\n") + w;
            GtkWidget *warn = gtk_image_new_from_icon_name("dialog-warning-symbolic");
            gtk_widget_add_css_class(warn, "warning");
            gtk_widget_set_tooltip_text(warn, tip.c_str());
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), warn);
        }
        GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(del, "flat");
        gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(del, "Delete primer");
        g_object_set_data(G_OBJECT(del), "primer", GINT_TO_POINTER(int(i)));
        g_signal_connect(del, "clicked", G_CALLBACK((+[](GtkButton *b, gpointer p) {
                             static_cast<Window *>(p)->m_doc.removePrimer(
                                 GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "primer")));
                         })),
                         this);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), del);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "primer", GINT_TO_POINTER(int(i)));
        gtk_list_box_append(GTK_LIST_BOX(m_primerList), row);
    }
}

void Window::beginAddingPrimer(const std::string &bases)
{
    if (m_doc.isEmpty())
        return;
    m_tablePane = "primers";
    showPane("primers");
    if (!m_panelShown)
        setPanelShown(true);
    if (m_primerEditor) {
        if (!bases.empty())
            m_primerEditor->setBases(bases);
        m_primerEditor->focus();
        return;
    }
    m_primerEditor = std::make_unique<PrimerEditor>(&m_doc, bases);
    m_primerEditor->onClose = [this] {
        // Called from one of the editor's own buttons: let go of it once that
        // click is over.
        if (!m_endPrimerIdle)
            m_endPrimerIdle = g_idle_add_once([](gpointer p) {
                auto *self = static_cast<Window *>(p);
                self->m_endPrimerIdle = 0;
                self->endAddingPrimer();
            }, this);
    };
    gtk_stack_add_named(GTK_STACK(m_primerStack), m_primerEditor->widget(), "editor");
    gtk_stack_set_visible_child_name(GTK_STACK(m_primerStack), "editor");
    m_primerEditor->focus();
}

// One wizard per tab, over this window.
void Window::openPcrWizard()
{
    if (m_doc.isEmpty())
        return;
    if (m_pcr) {
        m_pcr->present();
        return;
    }
    m_pcr = new PcrWizard(&m_doc, m_window, displayName());
    m_pcr->onClosed = [this] { m_pcr = nullptr; };
    m_pcr->onAddPrimer = [this] {
        present();
        beginAddingPrimer("");
    };
    m_pcr->onOpenProduct = [this](const Sequence &product) {
        Window *w = Window::create(gtk_window_get_application(gtkWindow()));
        w->document().setSequence(product);
        w->document().markUnsaved();
        w->present();
    };
    m_pcr->present();
}

void Window::openGelWizard()
{
    if (m_doc.isEmpty())
        return;
    if (m_gel) {
        m_gel->present();
        return;
    }
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    m_gel = new GelWizard(&m_doc, m_window, displayName(), [app] {
        std::vector<GelSource> out;
        for (Window *w : Window::all(app)) {
            // Lanes are named as the sequence is, not as its file.
            const std::string &name = w->document().sequence().name;
            out.push_back({&w->document(), name.empty() ? w->displayName() : name});
        }
        return out;
    });
    m_gel->onClosed = [this] { m_gel = nullptr; };
    m_gel->present();
}

void Window::endAddingPrimer()
{
    if (!m_primerEditor)
        return;
    gtk_stack_set_visible_child_name(GTK_STACK(m_primerStack), "list");
    gtk_stack_remove(GTK_STACK(m_primerStack), m_primerEditor->widget());
    m_primerEditor.reset();
    rebuildPrimerList();
}

void Window::rebuildEnzymeList()
{
    gtk_list_box_remove_all(GTK_LIST_BOX(m_enzymeList));
    m_enzymeRows.clear();
    std::vector<const Enzyme *> cutters;
    for (const Enzyme &e : m_doc.enzymes())
        if (m_doc.cutCount(e.name) > 0)
            cutters.push_back(&e);
    std::sort(cutters.begin(), cutters.end(), [](const Enzyme *a, const Enzyme *b) {
        return toLower(a->name) < toLower(b->name);
    });
    for (const Enzyme *e : cutters) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), e->name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), e->site.c_str());
        GtkWidget *check = gtk_check_button_new();
        gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(check, "Show on the map and sequence");
        g_object_set_data_full(G_OBJECT(check), "enzyme", g_strdup(e->name.c_str()), g_free);
        g_signal_connect(check, "toggled", G_CALLBACK(&Window::onEnzymeToggled), this);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        const int n = m_doc.cutCount(e->name);
        GtkWidget *count = gtk_label_new((std::to_string(n) + (n == 1 ? " cut" : " cuts")).c_str());
        gtk_widget_add_css_class(count, "dim-label");
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), count);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data_full(G_OBJECT(row), "enzyme", g_strdup(e->name.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(m_enzymeList), row);
        m_enzymeRows.push_back({e->name, row, check, count});
    }
    refreshEnzymeRows();
}

void Window::refreshEnzymeRows()
{
    m_updatingRows = true;
    for (EnzymeRow &r : m_enzymeRows) {
        const bool passes = m_doc.enzymePassesFilter(r.name);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(r.check), passes && !m_doc.isEnzymeHidden(r.name));
        gtk_widget_set_sensitive(r.check, passes);
        if (passes)
            gtk_widget_remove_css_class(r.row, "dim-label");
        else
            gtk_widget_add_css_class(r.row, "dim-label");
    }
    m_updatingRows = false;
}

void Window::onFilterChanged(GObject *dd, GParamSpec *, gpointer p)
{
    static const int values[] = {1, 2, 3, 0};
    const guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    static_cast<Window *>(p)->m_doc.setMaxCuts(values[std::min<guint>(i, 3)]);
}

namespace {
std::string enzymeSetsPath()
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "restricto", "enzyme-sets.txt", nullptr);
    std::string out = path;
    g_free(path);
    return out;
}

std::vector<EnzymeSet> savedEnzymeSets()
{
    std::string text;
    return readFile(enzymeSetsPath(), &text) ? parseEnzymeSets(text) : std::vector<EnzymeSet>{};
}
} // namespace

void Window::rebuildEnzymeSets(const std::string &select)
{
    m_sets = {{"All enzymes", {}}};
    for (const EnzymeSet &s : savedEnzymeSets())
        m_sets.push_back(s);
    for (const EnzymeSet &s : builtinEnzymeSets())
        m_sets.push_back(s);
    GtkStringList *names = gtk_string_list_new(nullptr);
    guint selected = 0;
    for (size_t i = 0; i < m_sets.size(); ++i) {
        gtk_string_list_append(names, m_sets[i].name.c_str());
        if (!select.empty() && m_sets[i].name == select)
            selected = guint(i);
    }
    m_settingSet = true;
    gtk_drop_down_set_model(GTK_DROP_DOWN(m_setDrop), G_LIST_MODEL(names));
    g_object_unref(names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(m_setDrop), selected);
    m_settingSet = false;
}

void Window::onEnzymeSetChanged(GObject *dd, GParamSpec *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    const guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (self->m_settingSet || i >= self->m_sets.size())
        return;
    std::set<std::string> names;
    if (i == 0) {
        for (const Enzyme &e : self->m_doc.enzymes())
            names.insert(e.name);
    } else {
        names.insert(self->m_sets[i].enzymes.begin(), self->m_sets[i].enzymes.end());
    }
    self->m_doc.showOnlyEnzymes(names, self->m_doc.maxCuts());
}

void Window::saveEnzymeSetDialog()
{
    std::vector<std::string> checked;
    for (const Enzyme &e : m_doc.enzymes())
        if (!m_doc.isEnzymeHidden(e.name))
            checked.push_back(e.name);
    AdwDialog *dialog = adw_alert_dialog_new("Save Enzyme Set", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "%zu checked enzymes. Saved sets appear in every window.",
                                 checked.size());
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Name");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel", "save", "Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    g_object_set_data(G_OBJECT(dialog), "entry", entry);
    g_object_set_data_full(G_OBJECT(dialog), "enzymes", new std::vector<std::string>(checked),
                           [](gpointer v) { delete static_cast<std::vector<std::string> *>(v); });
    g_signal_connect(dialog, "response", G_CALLBACK((+[](AdwAlertDialog *d, const char *response, gpointer p) {
                         if (std::string(response) != "save")
                             return;
                         auto *self = static_cast<Window *>(p);
                         auto *entry = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(d), "entry"));
                         std::string name = trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
                         std::replace(name.begin(), name.end(), '\n', ' ');
                         if (name.empty())
                             return;
                         EnzymeSet set{name, *static_cast<std::vector<std::string> *>(
                                                 g_object_get_data(G_OBJECT(d), "enzymes"))};
                         std::vector<EnzymeSet> sets = savedEnzymeSets();
                         sets.erase(std::remove_if(sets.begin(), sets.end(),
                                                   [&](const EnzymeSet &s) { return s.name == name; }),
                                    sets.end());
                         sets.push_back(set);
                         const std::string path = enzymeSetsPath();
                         gchar *dir = g_path_get_dirname(path.c_str());
                         g_mkdir_with_parents(dir, 0755);
                         g_free(dir);
                         std::string error;
                         if (!writeFile(path, formatEnzymeSets(sets), &error)) {
                             self->showError("Could not save the enzyme set", error);
                             return;
                         }
                         self->rebuildEnzymeSets(name);
                         self->toast("Saved enzyme set " + name);
                     })),
                     this);
    presentDialog(dialog);
    adw_dialog_set_focus(dialog, entry);
}

void Window::onEnzymeToggled(GtkCheckButton *b, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (self->m_updatingRows)
        return;
    const char *name = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "enzyme"));
    self->m_doc.setEnzymeHidden(name, !gtk_check_button_get_active(b));
}

void Window::onFeatureRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer p)
{
    static_cast<Window *>(p)->m_doc.activateFeature(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "feature")));
}

void Window::onEnzymeRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    const std::string name = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "enzyme"));
    const auto &sites = self->m_doc.visibleSites();
    // Cycle through this enzyme's sites on repeated activation.
    int first = -1, next = -1;
    for (size_t i = 0; i < sites.size(); ++i) {
        if (sites[i].enzyme->name != name)
            continue;
        if (first < 0)
            first = int(i);
        if (next < 0 && int(i) > self->m_doc.activeSite())
            next = int(i);
    }
    if (first < 0) {
        self->toast(name + " is hidden by the current filter");
        return;
    }
    self->m_doc.activateSite(next >= 0 ? next : first);
}

void Window::find(const std::string &query)
{
    int count = 0;
    Strand strand = Strand::Forward;
    const int n = m_doc.findNext(query, &count, &strand);
    // After findNext: its new selection clears the message.
    if (n < 0)
        m_statusMessage = "Find takes DNA bases (IUPAC codes such as N and R work too)";
    else if (n == 0)
        m_statusMessage = "No match for " + toUpper(query) + " on either strand";
    else if (count > 1)
        m_statusMessage = "Match " + std::to_string(n) + " of " + std::to_string(count)
                        + (strand == Strand::Reverse ? " (bottom strand)" : "");
    updateStatus();
}

void Window::onSearchActivate(GtkSearchEntry *e, gpointer p)
{
    static_cast<Window *>(p)->find(gtk_editable_get_text(GTK_EDITABLE(e)));
}

void Window::toast(const std::string &text)
{
    m_shell->toast(text);
}

void Window::showError(const std::string &heading, const std::string &body)
{
    AdwDialog *d = adw_alert_dialog_new(heading.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "close", "Close");
    presentDialog(d);
}

// ---- actions --------------------------------------------------------------------

void Window::copyText(const std::string &text, const char *what)
{
    if (text.empty()) {
        toast("Select some bases first");
        return;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(m_window), text.c_str());
    toast(std::string("Copied ") + view::formatBp(int(text.size())) + what);
}

void Window::actCopy(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    // Let text fields keep their own Ctrl+C.
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self->m_window));
    if (focus && GTK_IS_TEXT(focus)) {
        gtk_widget_activate_action(focus, "clipboard.copy", nullptr);
        return;
    }
    self->copyText(self->m_doc.selectedBases(), "");
    g_copied = self->m_doc.selectedFragment();
}

void Window::actCopyRc(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    self->copyText(reverseComplement(self->m_doc.selectedBases()), " (reverse complement)");
    g_copied = {};
}

void Window::actFind(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    // Again while typing in it closes the bar.
    const bool open = gtk_widget_get_visible(self->m_findBar);
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self->m_window));
    self->showFindBar(!(open && focus && gtk_widget_is_ancestor(focus, self->m_findBar)));
}

void Window::actClose(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    self->m_shell->closePage(self);
}

void Window::actToggleSidebar(GSimpleAction *, GVariant *state, gpointer shell)
{
    const bool shown = g_variant_get_boolean(state);
    static_cast<Shell *>(shell)->setSidebarShown(shown);
    setSettingBool("Layout", "sidebar", shown);
}

void Window::actShowFolder(GSimpleAction *, GVariant *, gpointer shell)
{
    static_cast<Shell *>(shell)->chooseFolder();
}

void Window::actFullscreen(GSimpleAction *a, GVariant *state, gpointer shell)
{
    g_simple_action_set_state(a, state);
    GtkWindow *w = static_cast<Shell *>(shell)->gtkWindow();
    if (g_variant_get_boolean(state))
        gtk_window_fullscreen(w);
    else
        gtk_window_unfullscreen(w);
}

void Window::actShowPanel(GSimpleAction *, GVariant *state, gpointer p)
{
    static_cast<Window *>(p)->setPanelShown(g_variant_get_boolean(state));
}

void Window::actShowMap(GSimpleAction *, GVariant *state, gpointer p)
{
    static_cast<Window *>(p)->setMapHidden(!g_variant_get_boolean(state));
}

void Window::actStackMap(GSimpleAction *, GVariant *state, gpointer p)
{
    static_cast<Window *>(p)->setMapStacked(g_variant_get_boolean(state));
}

void Window::actExportGenBank(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (!self->m_doc.isEmpty())
        self->saveDialog("Export GenBank", self->m_doc.sequence().name + ".gb", SaveGenBank);
}

void Window::actComplement(GSimpleAction *a, GVariant *state, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    g_simple_action_set_state(a, state);
    self->m_seq->setShowComplement(g_variant_get_boolean(state));
    self->refreshToggles();
}

void Window::actFrames(GSimpleAction *a, GVariant *state, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    g_simple_action_set_state(a, state);
    const int frames = std::atoi(g_variant_get_string(state, nullptr));
    self->m_seq->setFrames(frames);
    // The map draws the ORFs as arrows while the translation is on.
    view::MapOptions options = self->m_map->options();
    options.frames = frames;
    self->m_map->setOptions(options);
    self->refreshToggles();
}

void Window::actClearReads(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->m_doc.clearReads();
}

void Window::actGroupTens(GSimpleAction *a, GVariant *state, gpointer p)
{
    g_simple_action_set_state(a, state);
    static_cast<Window *>(p)->m_seq->setGroupTens(g_variant_get_boolean(state));
}

void Window::stepZoom(int direction)
{
    double zoom = 1.0;
    const double now = savedZoom();
    if (direction > 0) {
        zoom = kZoomSteps[std::size(kZoomSteps) - 1];
        for (double z : kZoomSteps)
            if (z > now + 1e-6) {
                zoom = z;
                break;
            }
    } else if (direction < 0) {
        zoom = kZoomSteps[0];
        for (double z : kZoomSteps)
            if (z < now - 1e-6)
                zoom = z;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", zoom);
    setSettingString("View", "zoom", buf);
    // Every window and tab, so they all read the same.
    for (Window *w : all(gtk_window_get_application(GTK_WINDOW(m_window)))) {
        w->m_map->setZoom(zoom);
        w->m_seq->setZoom(zoom);
    }
}

void Window::actZoomIn(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->stepZoom(1);
}

void Window::actZoomOut(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->stepZoom(-1);
}

void Window::actZoomReset(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->stepZoom(0);
}

void Window::actRevert(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->revert();
}

void Window::actCopyMap(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->copyMapImage();
}

void Window::actGoTo(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->goToDialog();
}

void Window::actRename(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->renameDialog();
}

void Window::actAnnotate(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    const size_t before = self->m_doc.sequence().features.size();
    const int added = self->m_doc.annotateCommonFeatures();
    if (added == 0) {
        self->toast("No common elements found that aren't annotated already");
        return;
    }
    std::string names;
    const auto &features = self->m_doc.sequence().features;
    for (size_t i = before; i < features.size(); ++i)
        names += (i == before ? "" : ", ") + features[i].name;
    self->toast("Added " + std::to_string(added) + (added == 1 ? " feature: " : " features: ") + names);
}

void Window::actDeletePrimer(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->deletePrimerDialog();
}

void Window::actCheckPrimer(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->checkPrimerDialog();
}

void Window::revert()
{
    const std::string path = m_doc.path();
    if (path.empty() || !m_doc.isModified())
        return;
    AdwDialog *d = adw_alert_dialog_new("Revert to the saved file?",
                                        ("Your changes to " + baseName(path) + " since it was last saved will be lost.").c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel", "revert", "Revert", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "revert", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
    g_signal_connect(d, "response", G_CALLBACK((+[](AdwAlertDialog *, const char *response, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         if (std::string(response) != "revert")
                             return;
                         const std::string path = self->m_doc.path();
                         std::string error;
                         if (self->load(path, &error))
                             self->toast("Reverted to " + baseName(path));
                         else
                             self->showError("Could not revert", error);
                     })),
                     this);
    presentDialog(d);
}

// The map as on Export Map, light, at 2x, as an image on the clipboard.
void Window::copyMapImage()
{
    if (m_doc.isEmpty())
        return;
    const int w = 900, h = 760, scale = 2;
    view::PangoTextMeasurer tm;
    view::MapLayout layout(m_doc.sequence(), m_doc.visibleSites(), view::Theme::light(), tm, w, h, m_map->options());
    view::MapState st;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    view::drawScene(cr, layout.render(st));
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    GBytes *bytes = g_bytes_new(cairo_image_surface_get_data(surface), size_t(stride) * size_t(h * scale));
    cairo_surface_destroy(surface);
    GdkTexture *texture = gdk_memory_texture_new(w * scale, h * scale, GDK_MEMORY_DEFAULT, bytes, size_t(stride));
    g_bytes_unref(bytes);
    gdk_clipboard_set_texture(gtk_widget_get_clipboard(m_window), texture);
    g_object_unref(texture);
    toast("Copied the map image");
}

void Window::actSave(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->save();
}

void Window::actSaveAs(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (self->m_doc.isEmpty())
        return;
    const std::string ext = fileExtension(self->m_doc.path());
    self->saveDialog("Save As (.dna, .gb or .fasta)", self->m_doc.sequence().name + (ext == "dna" ? ".dna" : ".gb"),
                     SaveSequence);
}

namespace {
// The format a path saves in; FASTA drops features, so it is export-only.
FileFormat saveFormatFor(const std::string &path)
{
    const std::string ext = fileExtension(path);
    if (ext == "dna")
        return FileFormat::SnapGene;
    if (ext == "embl" || ext == "emb")
        return FileFormat::Embl;
    if (ext == "fa" || ext == "fasta" || ext == "fna" || ext == "fas")
        return FileFormat::Fasta;
    return FileFormat::GenBank;
}
}

bool Window::saveTo(const std::string &path, FileFormat format)
{
    std::string error;
    if (!saveSequenceFile(path, m_doc.sequence(), format, &error)) {
        showError("Could not save", error);
        return false;
    }
    if (format != FileFormat::Fasta) {
        m_doc.setPath(path);
        m_doc.markSaved();
    }
    toast("Saved " + baseName(path));
    return true;
}

bool Window::save(bool closeAfter)
{
    if (m_doc.isEmpty())
        return false;
    const std::string &path = m_doc.path();
    if (!path.empty() && saveFormatFor(path) != FileFormat::Fasta) {
        if (!saveTo(path, saveFormatFor(path)))
            return false;
        if (closeAfter)
            finishClose();
        return true;
    }
    saveDialog("Save As (.dna, .gb or .fasta)", m_doc.sequence().name + ".dna", SaveSequence, closeAfter);
    return false;
}

void Window::presentDialog(AdwDialog *dialog)
{
    m_dialogs.push_back(dialog);
    g_signal_connect(dialog, "closed", G_CALLBACK((+[](AdwDialog *d, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         self->m_dialogs.erase(std::remove(self->m_dialogs.begin(), self->m_dialogs.end(), d),
                                               self->m_dialogs.end());
                     })),
                     this);
    adw_dialog_present(dialog, m_window);
}

void Window::closeTools()
{
    // The tab's dialogs go with it (their handlers hold this window and its
    // document). Closing one removes it from the list, so work on a copy.
    const std::vector<AdwDialog *> dialogs = m_dialogs;
    for (AdwDialog *d : dialogs) {
        g_signal_handlers_disconnect_by_data(d, this);
        adw_dialog_force_close(d);
    }
    m_dialogs.clear();
    if (m_pcr) {
        m_pcr->onClosed = nullptr;
        m_pcr->close();
        m_pcr = nullptr;
    }
    if (m_gel) {
        m_gel->onClosed = nullptr;
        m_gel->close();
        m_gel = nullptr;
    }
}

bool Window::confirmClose(bool closeWindow)
{
    if (m_closeConfirmed || !m_doc.isModified())
        return true;
    m_closeWindow = closeWindow;
    present();   // show the document being asked about
    if (m_closeAlert) {
        // Already being asked (a second Ctrl+W, or Ctrl+Q over a tab's
        // question): the one alert stands, so its answer can't be given twice.
        gtk_widget_grab_focus(GTK_WIDGET(m_closeAlert));
        return false;
    }
    AdwDialog *d = adw_alert_dialog_new(("Save changes to " + displayName() + "?").c_str(),
                                        closeWindow ? "Unsaved changes are lost when the window closes."
                                                    : "Unsaved changes are lost when the tab closes.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel", "discard", "Discard", "save", "Save",
                                   nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
    g_signal_connect(d, "response", G_CALLBACK((+[](AdwAlertDialog *, const char *response, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         self->m_closeAlert = nullptr;
                         if (g_strcmp0(response, "discard") == 0)
                             self->finishClose();
                         else if (g_strcmp0(response, "save") == 0)
                             self->save(true);
                     })),
                     this);
    m_closeAlert = d;
    adw_dialog_present(d, m_window);
    return false;
}

// After the answer: closes the tab, or carries on closing the window (which
// asks about its next unsaved document).
void Window::finishClose()
{
    m_closeConfirmed = true;
    if (m_closeWindow)
        gtk_window_close(gtkWindow());
    else
        m_shell->closePage(this);
}

namespace {
// Tools from the first alpha that wait for their own tested step. They stay
// built and reachable with Experimental Tools on; off, their menu items and
// shortcuts are gone.
const char *const kExperimentalActions[] = {
    "search-folder", "print", "choose-enzymes", "orfs", "protein", "clone", "golden-gate", "ta-cloning",
    "overlap-extension", "mutagenesis", "history", "align", "compare", "clear-reads", "map-options", "frames",
};

std::string settingsPath()
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "restricto", "settings.ini", nullptr);
    std::string out = path;
    g_free(path);
    return out;
}

int g_experimental = -1;   // -1 = not read yet
} // namespace

bool Window::experimentalTools()
{
    if (g_experimental < 0) {
        GKeyFile *kf = g_key_file_new();
        g_experimental = g_key_file_load_from_file(kf, settingsPath().c_str(), G_KEY_FILE_NONE, nullptr) &&
                         g_key_file_get_boolean(kf, "Tools", "experimental", nullptr);
        g_key_file_free(kf);
    }
    return g_experimental > 0;
}

void Window::setExperimentalTools(GtkApplication *app, bool on)
{
    g_experimental = on;
    const std::string path = settingsPath();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_set_boolean(kf, "Tools", "experimental", on);
    gchar *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
    g_key_file_free(kf);
    for (Shell *s : Shell::all(app))
        s->rebuildMenu();
    for (Window *w : all(app))
        w->updateActions();
}

void Window::updateActions()
{
    m_seq->setEditable(!m_doc.isEmpty());
    refreshToggles();
    // The window's actions follow its current tab.
    if (!isCurrent())
        return;
    auto enable = [this](const char *name, bool on) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(m_window), name);
        if (a)
            g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
    };
    const bool has = !m_doc.isEmpty();
    const bool sel = m_doc.hasSelection() && m_doc.selection().length > 0;
    enable("undo", m_doc.canUndo());
    enable("redo", m_doc.canRedo());
    for (const char *name : {"save", "save-as", "export-fasta", "export-genbank", "export-map", "copy-map", "go-to", "rename",
                             "annotate", "print", "paste", "select-all", "find", "copy", "copy-rc",
                             "reverse-complement", "circular", "digest", "pcr", "clone", "golden-gate", "gibson", "ta-cloning", "overlap-extension", "insertion", "choose-enzymes", "history", "align", "compare", "orfs", "protein", "map-options"})
        enable(name, has);
    enable("cut", sel);
    enable("add-feature", sel);
    enable("add-primer", has);
    enable("mutagenesis", has && m_doc.hasSelection() && !m_doc.selection().wraps(m_doc.sequence().length()));
    enable("design-primers", sel && !m_doc.selection().wraps(m_doc.sequence().length()));
    enable("edit-feature", m_doc.activeFeature() >= 0);
    enable("delete-feature", m_doc.activeFeature() >= 0);
    enable("set-origin", has && m_doc.sequence().circular && m_doc.hasSelection());
    enable("clear-reads", !m_doc.reads().empty());
    enable("revert", !m_doc.path().empty() && m_doc.isModified());
    // Saving an unchanged file would only rewrite it (and drop what Restricto
    // doesn't read); a new sequence can always be saved.
    enable("save", has && (m_doc.isModified() || m_doc.path().empty()));
    enable("delete-primer", !m_doc.sequence().primers.empty());
    auto state = [this](const char *name, GVariant *value) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(m_window), name);
        if (a)
            g_simple_action_set_state(G_SIMPLE_ACTION(a), value);
        else
            g_variant_unref(g_variant_ref_sink(value));
    };
    state("circular", g_variant_new_boolean(m_doc.sequence().circular));
    state("show-complement", g_variant_new_boolean(m_seq->showComplement()));
    state("frames", g_variant_new_string(std::to_string(m_seq->frames()).c_str()));
    state("group-tens", g_variant_new_boolean(m_seq->groupTens()));
    state("show-map", g_variant_new_boolean(!m_mapHidden));
    state("stack-map", g_variant_new_boolean(m_mapStacked));
    state("show-panel", g_variant_new_boolean(m_panelShown));
    enable("stack-map", !m_mapHidden);
    if (!experimentalTools())
        for (const char *name : kExperimentalActions)
            enable(name, false);
}

void Window::actUndo(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (!self->m_doc.canUndo())
        return;
    const std::string label = self->m_doc.undoLabel();
    self->m_doc.undo();
    self->toast("Undid " + toLower(label));
}

void Window::actRedo(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (!self->m_doc.canRedo())
        return;
    const std::string label = self->m_doc.redoLabel();
    self->m_doc.redo();
    self->toast("Redid " + toLower(label));
}

void Window::actCut(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self->m_window));
    if (focus && GTK_IS_TEXT(focus)) {
        gtk_widget_activate_action(focus, "clipboard.cut", nullptr);
        return;
    }
    const std::string bases = self->m_doc.selectedBases();
    if (bases.empty())
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(self->m_window), bases.c_str());
    g_copied = self->m_doc.selectedFragment();
    self->m_doc.deleteSelection();
    self->toast("Cut " + view::formatBp(int(bases.size())));
}

void Window::pasteText(const std::string &text)
{
    if (!g_copied.bases.empty() && cleanBases(text) == g_copied.bases) {
        m_doc.pasteFragment(g_copied);
        return;
    }
    if (!m_doc.typeBases(text))
        toast("The clipboard holds no DNA bases");
}

void Window::actPaste(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self->m_window));
    if (focus && GTK_IS_TEXT(focus)) {
        gtk_widget_activate_action(focus, "clipboard.paste", nullptr);
        return;
    }
    gdk_clipboard_read_text_async(
        gtk_widget_get_clipboard(self->m_window), nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr);
            // The tab may have closed meanwhile; look it up again by weak pointer.
            auto *weak = static_cast<GWeakRef *>(data);
            GObject *win = static_cast<GObject *>(g_weak_ref_get(weak));
            if (win && text)
                if (Window *self = static_cast<Window *>(g_object_get_data(win, "oc-page")))
                    self->pasteText(text);
            if (win)
                g_object_unref(win);
            g_free(text);
            g_weak_ref_clear(weak);
            delete weak;
        },
        [&] {
            auto *weak = new GWeakRef;
            g_weak_ref_init(weak, self->m_page);
            return weak;
        }());
}

void Window::actSelectAll(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self->m_window));
    if (focus && GTK_IS_TEXT(focus)) {
        gtk_editable_select_region(GTK_EDITABLE(focus), 0, -1);
        return;
    }
    if (focus && GTK_IS_TEXT_VIEW(focus)) {   // the primer's sequence box
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(focus));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gtk_text_buffer_select_range(buffer, &start, &end);
        return;
    }
    self->m_doc.setSelection({0, self->m_doc.sequence().length()});
}

void Window::actReverseComplement(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->m_doc.reverseComplementAll();
}

void Window::actSetOrigin(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->m_doc.setOriginAtSelection();
}

void Window::actCircular(GSimpleAction *, GVariant *state, gpointer p)
{
    static_cast<Window *>(p)->m_doc.setCircular(g_variant_get_boolean(state));
}

void Window::actAddFeature(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->featureDialog(-1);
}

void Window::actEditFeature(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (self->m_doc.activeFeature() >= 0)
        self->featureDialog(self->m_doc.activeFeature());
}

void Window::actDeleteFeature(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    const int i = self->m_doc.activeFeature();
    if (i < 0)
        return;
    const std::string name = self->m_doc.sequence().features[size_t(i)].name;
    self->m_doc.removeFeature(i);
    self->toast("Deleted " + name);
}

// Opens a new primer's inputs in the Primers pane, filled in with the
// selected bases (top strand, so a forward primer) when there is a selection.
void Window::actAddPrimer(GSimpleAction *, GVariant *, gpointer p)
{
    auto *w = static_cast<Window *>(p);
    const bool sel = w->m_doc.hasSelection() && w->m_doc.selection().length > 0;
    w->beginAddingPrimer(sel ? w->m_doc.selectedBases() : "");
}

void Window::actDigest(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->openGelWizard();
}

void Window::actPcr(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->openPcrWizard();
}

void Window::actClone(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->cloningDialog();
}

void Window::actGoldenGate(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->goldenGateDialog();
}

void Window::actGibson(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->gibsonDialog();
}

void Window::actDesignPrimers(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->designPrimersDialog();
}

void Window::actChooseEnzymes(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->chooseEnzymesDialog();
}

void Window::actMutagenesis(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->mutagenesisDialog();
}

void Window::actHistory(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->historyDialog();
}

void Window::actAlign(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->alignDialog();
}

void Window::actCompare(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->alignDialog(true);
}

void Window::actOrfs(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->orfDialog();
}

void Window::actMapOptions(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->mapOptionsDialog();
}

void Window::showOnlyEnzymes(const std::set<std::string> &names, int maxCuts)
{
    m_doc.showOnlyEnzymes(names, maxCuts);
    // Keep the cut filter in the enzyme panel in step (1, 2, 3, all).
    gtk_drop_down_set_selected(GTK_DROP_DOWN(m_filter), guint(maxCuts >= 1 && maxCuts <= 3 ? maxCuts - 1 : 3));
    toast(std::to_string(names.size()) + (names.size() == 1 ? " enzyme shown" : " enzymes shown"));
}

void Window::actExportFasta(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (!self->m_doc.isEmpty())
        self->saveDialog("Export FASTA", self->m_doc.sequence().name + ".fasta", SaveFasta);
}

void Window::actExportMap(GSimpleAction *, GVariant *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (!self->m_doc.isEmpty())
        self->saveDialog("Export Map (PNG, SVG or PDF)", self->m_doc.sequence().name + "-map.png", SaveMap);
}

namespace {
struct SaveRequest {
    Window *window;
    int kind;
    bool closeAfter;
};
}

void Window::saveDialog(const std::string &title, const std::string &suggested, int kind, bool closeAfter)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title.c_str());
    gtk_file_dialog_set_initial_name(dialog, suggested.c_str());
    auto *req = new SaveRequest{this, kind, closeAfter};
    gtk_file_dialog_save(dialog, gtkWindow(), nullptr,
                         [](GObject *src, GAsyncResult *res, gpointer data) {
                             auto *r = static_cast<SaveRequest *>(data);
                             GError *err = nullptr;
                             GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
                             if (file) {
                                 char *cpath = g_file_get_path(file);
                                 std::string path = cpath ? cpath : "";
                                 g_free(cpath);
                                 g_object_unref(file);
                                 Window *w = r->window;
                                 if (r->kind == SaveMap) {
                                     w->exportMap(path);
                                 } else if (r->kind == SaveGenBank) {
                                     // A copy: the document keeps its own file.
                                     std::string error;
                                     if (saveSequenceFile(path, w->m_doc.sequence(), FileFormat::GenBank, &error))
                                         w->toast("Exported " + baseName(path));
                                     else
                                         w->showError("Could not export", error);
                                 } else {
                                     const FileFormat fmt =
                                         r->kind == SaveFasta ? FileFormat::Fasta : saveFormatFor(path);
                                     if (w->saveTo(path, fmt) && r->closeAfter && !w->m_doc.isModified())
                                         w->finishClose();
                                 }
                             } else if (err) {
                                 g_error_free(err);   // cancelled
                             }
                             delete r;
                         },
                         req);
    g_object_unref(dialog);
}

void Window::actTaCloning(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->taCloningDialog();
}

void Window::actOverlapExtension(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->overlapExtensionDialog();
}

void Window::actProtein(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->proteinDialog();
}

void Window::actInsertion(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->insertionDialog();
}

void Window::actSearchFolder(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->searchFolderDialog();
}

void Window::actPrint(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->print();
}

namespace {
struct PrintJob {
    Window *window;
    std::vector<view::Scene> pages;
};
}

void Window::print()
{
    if (m_doc.isEmpty())
        return;
    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_job_name(op, m_doc.sequence().name.c_str());
    auto *job = new PrintJob{this, {}};
    g_object_set_data_full(G_OBJECT(op), "job", job, [](gpointer p) { delete static_cast<PrintJob *>(p); });
    g_signal_connect(op, "begin-print", G_CALLBACK((+[](GtkPrintOperation *op, GtkPrintContext *ctx, gpointer p) {
                         auto *job = static_cast<PrintJob *>(p);
                         Window *w = job->window;
                         view::PrintSettings ps;
                         // Lay out at a fixed width and scale to the paper, so text keeps its size.
                         ps.height = ps.width * gtk_print_context_get_height(ctx) / gtk_print_context_get_width(ctx);
                         ps.mapOptions = w->m_map->options();
                         ps.complement = w->m_seq->showComplement();
                         view::PangoTextMeasurer tm;
                         job->pages = view::printPages(w->m_doc.sequence(), w->m_doc.visibleSites(),
                                                       w->m_doc.reads(), tm, ps);
                         gtk_print_operation_set_n_pages(op, int(job->pages.size()));
                     })),
                     job);
    g_signal_connect(op, "draw-page",
                     G_CALLBACK((+[](GtkPrintOperation *, GtkPrintContext *ctx, int page, gpointer p) {
                         auto *job = static_cast<PrintJob *>(p);
                         if (page < 0 || page >= int(job->pages.size()))
                             return;
                         const view::Scene &sc = job->pages[size_t(page)];
                         cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
                         const double k = gtk_print_context_get_width(ctx) / sc.width;
                         cairo_scale(cr, k, k);
                         view::drawScene(cr, sc);
                     })),
                     job);
    GError *err = nullptr;
    const GtkPrintOperationResult res =
        gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, gtkWindow(), &err);
    if (res == GTK_PRINT_OPERATION_RESULT_ERROR && err) {
        showError("Could not print", err->message);
        g_error_free(err);
    }
    g_object_unref(op);
}

void Window::exportMap(const std::string &rawPath)
{
    std::string path = rawPath;
    const std::string ext = fileExtension(path);
    if (ext != "png" && ext != "svg" && ext != "pdf")
        path += ".png";
    const double w = std::max(900, gtk_widget_get_width(m_map->widget()));
    const double h = std::max(760, gtk_widget_get_height(m_map->widget()));
    view::PangoTextMeasurer tm;
    view::MapLayout layout(m_doc.sequence(), m_doc.visibleSites(), view::Theme::light(), tm, w, h, m_map->options());
    view::MapState st;   // exported maps carry no selection
    std::string error;
    if (view::exportScene(layout.render(st), path, &error))
        toast("Exported " + baseName(path));
    else
        showError("Could not export the map", error);
}

// ---- screenshot ------------------------------------------------------------------

namespace {
struct ShotRequest {
    Window *window;
    std::string path;
    void (*done)(bool, gpointer);
    gpointer data;
};
}

void Window::screenshot(const std::string &path, void (*done)(bool, gpointer), gpointer data)
{
    auto *req = new ShotRequest{this, path, done, data};
    // Give GTK a moment to map, allocate and draw the window.
    g_timeout_add(1200, [](gpointer p) -> gboolean {
        auto *r = static_cast<ShotRequest *>(p);
        GtkWidget *w = r->window->m_window;
        const int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
        bool ok = false;
        GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(w));
        if (width > 0 && height > 0 && renderer) {
            GdkPaintable *paintable = gtk_widget_paintable_new(w);
            GtkSnapshot *snap = gtk_snapshot_new();
            gdk_paintable_snapshot(paintable, snap, width, height);
            GskRenderNode *node = gtk_snapshot_free_to_node(snap);
            if (node) {
                graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, float(width), float(height));
                GdkTexture *tex = gsk_renderer_render_texture(renderer, node, &bounds);
                ok = gdk_texture_save_to_png(tex, r->path.c_str());
                g_object_unref(tex);
                gsk_render_node_unref(node);
            }
            g_object_unref(paintable);
        }
        r->done(ok, r->data);
        delete r;
        return G_SOURCE_REMOVE;
    }, req);
}

// ---- app-level helpers -------------------------------------------------------------

std::string withSandboxHint(const std::string &path, const std::string &error)
{
    if (access("/.flatpak-info", F_OK) != 0 || access(path.c_str(), R_OK) == 0)
        return error;
    return error + " (inside Flatpak, pass files with --file-forwarding @@ FILE @@)";
}

namespace {

std::string canonical(const std::string &path)
{
    char *c = g_canonicalize_filename(path.c_str(), nullptr);
    char *real = realpath(c, nullptr);
    std::string out = real ? real : c;
    free(real);
    g_free(c);
    return out;
}

} // namespace

void openFiles(GtkApplication *app, const std::vector<std::string> &paths, Window *preferred)
{
    // While a wizard is open over the current tab, new tabs open behind it,
    // so the wizard stays over the tab it belongs to (the gel lists them).
    Shell *activeShell = Shell::active(app);
    Window *busy = activeShell ? activeShell->current() : nullptr;
    if (busy && !busy->hasTools())
        busy = nullptr;
    for (const std::string &path : paths) {
        // A file that's already open: go to its tab rather than open it twice.
        const std::string want = canonical(path);
        Window *open = nullptr;
        for (Window *w : Window::all(app))
            if (!w->path().empty() && canonical(w->path()) == want)
                open = w;
        if (open) {
            if (busy && open != busy)
                busy->toast(baseName(path) + " is already open in another tab");
            else if (!busy)
                open->present();
            continue;
        }
        Window *target = (preferred && preferred->isEmpty()) ? preferred : nullptr;
        if (!target) {
            // Reuse the current tab if it is still empty.
            Shell *shell = Shell::active(app);
            Window *w = shell ? shell->current() : nullptr;
            target = (w && w->isEmpty()) ? w : Window::create(app);
        }
        std::string error;
        if (!target->load(path, &error)) {
            target->present();
            target->showError("Could not open " + baseName(path), withSandboxHint(path, error));
            continue;
        }
        if (busy) {
            busy->present();
            busy->toast("Opened " + baseName(path) + " in a new tab");
        } else {
            target->present();
        }
        preferred = nullptr;
    }
}

void showOpenDialog(GtkApplication *app, GtkWindow *parent)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Sequence");
    GtkFileFilter *seqs = gtk_file_filter_new();
    gtk_file_filter_set_name(seqs, "Sequence files");
    for (const char *pat : {"*.gb", "*.gbk", "*.genbank", "*.gbff", "*.ape", "*.embl", "*.dna", "*.fa", "*.fasta", "*.fna",
                            "*.fas", "*.seq", "*.txt", "*.ab1", "*.abi"})
        gtk_file_filter_add_pattern(seqs, pat);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, seqs);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, seqs);
    g_object_unref(filters);
    g_object_unref(seqs);
    g_object_unref(all);
    gtk_file_dialog_open_multiple(dialog, parent, nullptr,
                                  [](GObject *src, GAsyncResult *res, gpointer data) {
                                      auto *app = static_cast<GtkApplication *>(data);
                                      GError *err = nullptr;
                                      GListModel *files =
                                          gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, &err);
                                      if (!files) {
                                          if (err)
                                              g_error_free(err);
                                          g_object_unref(app);
                                          return;
                                      }
                                      std::vector<std::string> paths;
                                      for (guint i = 0; i < g_list_model_get_n_items(files); ++i) {
                                          GFile *f = G_FILE(g_list_model_get_item(files, i));
                                          char *p = g_file_get_path(f);
                                          if (p)
                                              paths.push_back(p);
                                          g_free(p);
                                          g_object_unref(f);
                                      }
                                      g_object_unref(files);
                                      openFiles(app, paths);
                                      g_object_unref(app);
                                  },
                                  g_object_ref(app));
    g_object_unref(dialog);
}

void disconnectTree(GtkWidget *root, gpointer data)
{
    g_signal_handlers_disconnect_by_data(root, data);
    for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        disconnectTree(c, data);
}

void showWholeNames(AdwComboRow *row)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                         GtkWidget *label = gtk_label_new(nullptr);
                         gtk_label_set_xalign(GTK_LABEL(label), 0);
                         gtk_list_item_set_child(item, label);
                     }),
                     nullptr);
    g_signal_connect(factory, "bind", G_CALLBACK(+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer) {
                         auto *text = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
                         gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                                            text ? gtk_string_object_get_string(text) : "");
                     }),
                     nullptr);
    adw_combo_row_set_list_factory(row, factory);
    g_object_unref(factory);
}

} // namespace oc::gtk
