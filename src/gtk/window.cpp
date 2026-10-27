// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"

#include "core/genbank.h"
#include "core/primercheck.h"
#include "core/seqio.h"
#include "view/cairo_render.h"
#include "view/style.h"
#include "view/tracelayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oc::gtk {

namespace {

enum SaveKind { SaveSequence, SaveFasta, SaveMap };

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

Window *Window::create(GtkApplication *app)
{
    return new Window(app);   // owned by the GtkWindow
}

Window *Window::fromWidget(GtkWidget *w)
{
    GtkRoot *root = gtk_widget_get_root(w);
    return root ? static_cast<Window *>(g_object_get_data(G_OBJECT(root), "oc-window")) : nullptr;
}

Window::Window(GtkApplication *app)
{
    buildUi(app);
    m_doc.onSequenceChanged([this] {
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
        updateStatus();
        updateActions();
        scrollTraceToSelection();
    });
    m_doc.onHistoryChanged([this] {
        updateTitle();
        updateActions();
    });
    updateTitle();
    updateStatus();
    updateActions();
}

Window::~Window() = default;

void Window::buildUi(GtkApplication *app)
{
    m_window = adw_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1320, 820);
    gtk_window_set_title(GTK_WINDOW(m_window), "OpenClone");
    g_object_set_data_full(G_OBJECT(m_window), "oc-window", this,
                           [](gpointer p) { delete static_cast<Window *>(p); });

    static const GActionEntry entries[] = {
        {"save", &Window::actSave, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"save-as", &Window::actSaveAs, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"undo", &Window::actUndo, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"redo", &Window::actRedo, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"cut", &Window::actCut, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"paste", &Window::actPaste, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"select-all", &Window::actSelectAll, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"reverse-complement", &Window::actReverseComplement, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"set-origin", &Window::actSetOrigin, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"circular", nullptr, nullptr, "false", &Window::actCircular, {0, 0, 0}},
        {"add-feature", &Window::actAddFeature, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"edit-feature", &Window::actEditFeature, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"delete-feature", &Window::actDeleteFeature, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"add-primer", &Window::actAddPrimer, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"digest", &Window::actDigest, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"pcr", &Window::actPcr, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"clone", &Window::actClone, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"golden-gate", &Window::actGoldenGate, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"gibson", &Window::actGibson, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"design-primers", &Window::actDesignPrimers, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"choose-enzymes", &Window::actChooseEnzymes, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"primer-by-sequence", &Window::actPrimerBySequence, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"mutagenesis", &Window::actMutagenesis, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"history", &Window::actHistory, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"align", &Window::actAlign, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"orfs", &Window::actOrfs, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"map-options", &Window::actMapOptions, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"export-fasta", &Window::actExportFasta, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"export-map", &Window::actExportMap, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"copy", &Window::actCopy, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"copy-rc", &Window::actCopyRc, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"find", &Window::actFind, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"close", &Window::actClose, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"show-complement", nullptr, nullptr, "true", &Window::actComplement, {0, 0, 0}},
        {"frames", nullptr, "s", "'0'", &Window::actFrames, {0, 0, 0}},
        {"clear-reads", &Window::actClearReads, nullptr, nullptr, nullptr, {0, 0, 0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(m_window), entries, G_N_ELEMENTS(entries), this);
    g_signal_connect(m_window, "close-request", G_CALLBACK(&Window::onCloseRequest), this);

    m_map = std::make_unique<MapView>(&m_doc);
    m_seq = std::make_unique<SequenceView>(&m_doc);

    // Header bar.
    GtkWidget *header = adw_header_bar_new();
    m_title = adw_window_title_new("OpenClone", "");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), m_title);

    GtkWidget *open = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(open, "Open a sequence (Ctrl+O)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "app.open");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), open);
    GtkWidget *undo = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_set_tooltip_text(undo, "Undo (Ctrl+Z)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(undo), "win.undo");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), undo);
    GtkWidget *redo = gtk_button_new_from_icon_name("edit-redo-symbolic");
    gtk_widget_set_tooltip_text(redo, "Redo (Shift+Ctrl+Z)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(redo), "win.redo");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), redo);

    GMenu *menu = g_menu_new();
    GMenu *fileSection = g_menu_new();
    g_menu_append(fileSection, "New Sequence…", "app.new");
    g_menu_append(fileSection, "Open…", "app.open");
    g_menu_append(fileSection, "Save", "win.save");
    g_menu_append(fileSection, "Save As…", "win.save-as");
    g_menu_append(fileSection, "Export FASTA…", "win.export-fasta");
    g_menu_append(fileSection, "Export Map…", "win.export-map");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(fileSection));
    GMenu *editSection = g_menu_new();
    g_menu_append(editSection, "Cut", "win.cut");
    g_menu_append(editSection, "Copy", "win.copy");
    g_menu_append(editSection, "Copy Reverse Complement", "win.copy-rc");
    g_menu_append(editSection, "Paste", "win.paste");
    g_menu_append(editSection, "Select All", "win.select-all");
    g_menu_append(editSection, "Find…", "win.find");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(editSection));
    GMenu *seqSection = g_menu_new();
    g_menu_append(seqSection, "Reverse Complement Sequence", "win.reverse-complement");
    g_menu_append(seqSection, "Set Origin at Selection", "win.set-origin");
    g_menu_append(seqSection, "Circular", "win.circular");
    g_menu_append(seqSection, "Choose Enzymes…", "win.choose-enzymes");
    g_menu_append(seqSection, "Digest and Gel…", "win.digest");
    GMenu *featureMenu = g_menu_new();
    g_menu_append(featureMenu, "Add Feature…", "win.add-feature");
    g_menu_append(featureMenu, "Edit Feature…", "win.edit-feature");
    g_menu_append(featureMenu, "Delete Feature", "win.delete-feature");
    g_menu_append(featureMenu, "Find ORFs…", "win.orfs");
    g_menu_append_submenu(seqSection, "Features", G_MENU_MODEL(featureMenu));
    GMenu *primerMenu = g_menu_new();
    g_menu_append(primerMenu, "Add Primer…", "win.add-primer");
    g_menu_append(primerMenu, "Add Primer by Sequence…", "win.primer-by-sequence");
    g_menu_append(primerMenu, "Design Primers for Selection…", "win.design-primers");
    g_menu_append_submenu(seqSection, "Primers", G_MENU_MODEL(primerMenu));
    GMenu *cloneMenu = g_menu_new();
    g_menu_append(cloneMenu, "PCR Product…", "win.pcr");
    g_menu_append(cloneMenu, "Restriction Cloning…", "win.clone");
    g_menu_append(cloneMenu, "Golden Gate Assembly…", "win.golden-gate");
    g_menu_append(cloneMenu, "Gibson Assembly…", "win.gibson");
    g_menu_append(cloneMenu, "Mutagenesis…", "win.mutagenesis");
    g_menu_append(cloneMenu, "Cloning History…", "win.history");
    g_menu_append_submenu(seqSection, "Cloning", G_MENU_MODEL(cloneMenu));
    g_menu_append(seqSection, "Align Read…", "win.align");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(seqSection));
    GMenu *viewSection = g_menu_new();
    g_menu_append(viewSection, "Map Display…", "win.map-options");
    g_menu_append(viewSection, "Show Complement Strand", "win.show-complement");
    GMenu *framesMenu = g_menu_new();
    g_menu_append(framesMenu, "None", "win.frames::0");
    g_menu_append(framesMenu, "Forward Frames", "win.frames::3");
    g_menu_append(framesMenu, "All Six Frames", "win.frames::6");
    g_menu_append_submenu(viewSection, "Translate Frames", G_MENU_MODEL(framesMenu));
    g_menu_append(viewSection, "Hide Aligned Reads", "win.clear-reads");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(viewSection));
    GMenu *appSection = g_menu_new();
    g_menu_append(appSection, "About OpenClone", "app.about");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(appSection));
    GtkWidget *menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menuButton), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menuButton), TRUE);
    gtk_widget_set_tooltip_text(menuButton, "Main menu");
    for (GMenu *m : {fileSection, editSection, seqSection, featureMenu, viewSection, appSection, menu})
        g_object_unref(m);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menuButton);

    GtkWidget *sidebarToggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebarToggle), "sidebar-show-right-symbolic");
    gtk_widget_set_tooltip_text(sidebarToggle, "Features and enzymes");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebarToggle), TRUE);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), sidebarToggle);

    m_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_search), "Find bases (both strands)");
    gtk_widget_set_size_request(m_search, 240, -1);
    g_signal_connect(m_search, "activate", G_CALLBACK(&Window::onSearchActivate), this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_search);

    // Content: map | sequence, with a sidebar on the right.
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), m_map->widget());
    gtk_paned_set_end_child(GTK_PANED(paned), m_seq->widget());
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 560);

    m_split = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(m_split), GTK_PACK_END);
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
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(content), paned);
    gtk_box_append(GTK_BOX(content), m_traceBox);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(m_split), content);
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(m_split), buildSidebar());
    adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(m_split), 260);
    adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(m_split), 300);
    g_object_bind_property(sidebarToggle, "active", m_split, "show-sidebar",
                           GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));

    // Empty state.
    GtkWidget *empty = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "io.github.openclone.OpenClone");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Open a Sequence");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty),
                                    "GenBank, FASTA and SnapGene .dna files.\nYou can also drop a file here.");
    GtkWidget *openPill = gtk_button_new_with_label("Open…");
    gtk_widget_add_css_class(openPill, "pill");
    gtk_widget_add_css_class(openPill, "suggested-action");
    gtk_widget_set_halign(openPill, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(openPill), "app.open");
    adw_status_page_set_child(ADW_STATUS_PAGE(empty), openPill);

    m_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(m_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(m_stack), m_split, "document");
    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "empty");

    // Status bar.
    m_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(m_status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(m_status), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(m_status, "dim-label");
    gtk_widget_add_css_class(m_status, "caption");
    gtk_widget_set_margin_start(m_status, 12);
    gtk_widget_set_margin_end(m_status, 12);
    gtk_widget_set_margin_top(m_status, 6);
    gtk_widget_set_margin_bottom(m_status, 6);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), m_stack);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), m_status);
    adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(toolbar), ADW_TOOLBAR_RAISED_BORDER);

    m_toasts = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(m_toasts), toolbar);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(m_window), m_toasts);

    // Drag and drop to open.
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(&Window::onDrop), this);
    gtk_widget_add_controller(m_window, GTK_EVENT_CONTROLLER(drop));

    // Follow the system light/dark preference.
    AdwStyleManager *sm = adw_style_manager_get_default();
    g_signal_connect_object(sm, "notify::dark", G_CALLBACK(&Window::onDarkChanged), m_window, GConnectFlags(0));
    // g_signal_connect_object passes the window as user data; route to us.
    onDarkChanged(G_OBJECT(sm), nullptr, m_window);
}

GtkWidget *Window::buildSidebar()
{
    GtkWidget *stack = adw_view_stack_new();

    // Features.
    m_featureList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_featureList), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_featureList, "navigation-sidebar");
    g_signal_connect(m_featureList, "row-activated", G_CALLBACK(&Window::onFeatureRowActivated), this);
    GtkWidget *fscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fscroll), m_featureList);
    gtk_widget_set_vexpand(fscroll, TRUE);
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), fscroll, "features", "Features",
                                        "view-list-bullet-symbolic");

    // Enzymes.
    GtkWidget *ebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    static const char *filters[] = {"Unique cutters", "Up to 2 cuts", "Up to 3 cuts", "All cutters", nullptr};
    m_filter = gtk_drop_down_new_from_strings(filters);
    gtk_widget_set_margin_start(m_filter, 8);
    gtk_widget_set_margin_end(m_filter, 8);
    gtk_widget_set_margin_top(m_filter, 6);
    g_signal_connect(m_filter, "notify::selected", G_CALLBACK(&Window::onFilterChanged), this);
    m_enzymeList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_enzymeList), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_enzymeList, "navigation-sidebar");
    g_signal_connect(m_enzymeList, "row-activated", G_CALLBACK(&Window::onEnzymeRowActivated), this);
    GtkWidget *escroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(escroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(escroll), m_enzymeList);
    gtk_widget_set_vexpand(escroll, TRUE);
    GtkWidget *credit = gtk_label_new("Enzyme data: REBASE, rebase.neb.com");
    gtk_widget_add_css_class(credit, "dim-label");
    gtk_widget_add_css_class(credit, "caption");
    gtk_widget_set_margin_bottom(credit, 6);
    gtk_box_append(GTK_BOX(ebox), m_filter);
    gtk_box_append(GTK_BOX(ebox), escroll);
    gtk_box_append(GTK_BOX(ebox), credit);
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), ebox, "enzymes", "Enzymes", "edit-cut-symbolic");

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
    GtkWidget *pscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pscroll), m_primerList);
    gtk_widget_set_vexpand(pscroll, TRUE);
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(stack), pscroll, "primers", "Primers",
                                        "format-justify-left-symbolic");

    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_NARROW);
    gtk_widget_set_margin_top(switcher, 6);
    gtk_widget_set_margin_start(switcher, 6);
    gtk_widget_set_margin_end(switcher, 6);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(box), switcher);
    gtk_box_append(GTK_BOX(box), stack);
    gtk_widget_set_size_request(box, 260, -1);
    return box;
}

void Window::onDarkChanged(GObject *sm, GParamSpec *, gpointer windowWidget)
{
    Window *self = static_cast<Window *>(g_object_get_data(G_OBJECT(windowWidget), "oc-window"));
    if (!self)
        return;
    const bool dark = adw_style_manager_get_dark(ADW_STYLE_MANAGER(sm));
    self->m_map->setDark(dark);
    self->m_seq->setDark(dark);
}

bool Window::load(const std::string &path, std::string *error)
{
    Sequence seq;
    FileFormat format = FileFormat::Unknown;
    if (!loadSequenceFile(path, &seq, error, &format))
        return false;
    // A trace can't be written back: saving asks for a new file.
    m_doc.setSequence(std::move(seq), format == FileFormat::Abif ? std::string() : path);
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
    const Sequence &s = m_doc.sequence();
    if (m_doc.isEmpty()) {
        adw_window_title_set_title(ADW_WINDOW_TITLE(m_title), "OpenClone");
        adw_window_title_set_subtitle(ADW_WINDOW_TITLE(m_title), "");
        gtk_window_set_title(gtkWindow(), "OpenClone");
        return;
    }
    const std::string name = s.name.empty() ? "Untitled" : s.name;
    std::string sub = view::formatBp(s.length()) + " · " + (s.circular ? "circular" : "linear");
    if (!m_doc.path().empty())
        sub += " · " + baseName(m_doc.path());
    const std::string shown = m_doc.isModified() ? "• " + name : name;
    adw_window_title_set_title(ADW_WINDOW_TITLE(m_title), shown.c_str());
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(m_title), sub.c_str());
    gtk_window_set_title(gtkWindow(), (shown + " — OpenClone").c_str());
    updateStatus();
}

void Window::updateStatus()
{
    gtk_label_set_text(GTK_LABEL(m_status), m_doc.statusText().c_str());
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
        const PrimerCheck check = checkPrimer(pr.bases);
        std::string sub = std::to_string(pr.bases.size()) + " nt";
        if (!std::isnan(check.tm)) {
            char buf[48];
            std::snprintf(buf, sizeof buf, " · Tm %.1f °C · GC %.0f%%", check.tm, check.gc * 100);
            sub += buf;
        }
        if (pr.binding.length() > 0)
            sub += " · " + view::withThousands(pr.binding.start + 1) + "–" + view::withThousands(pr.binding.end)
                 + (pr.strand == Strand::Reverse ? " · ←" : " · →");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub.c_str());
        if (!check.warnings.empty()) {
            std::string tip;
            for (const std::string &w : check.warnings)
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
    if (n < 0)
        toast("Find takes DNA bases (IUPAC codes such as N and R work too)");
    else if (n == 0)
        toast("No match for " + toUpper(query) + " on either strand");
    else if (count > 1)
        toast("Match " + std::to_string(n) + " of " + std::to_string(count)
              + (strand == Strand::Reverse ? " (bottom strand)" : ""));
}

void Window::onSearchActivate(GtkSearchEntry *e, gpointer p)
{
    static_cast<Window *>(p)->find(gtk_editable_get_text(GTK_EDITABLE(e)));
}

gboolean Window::onDrop(GtkDropTarget *, const GValue *value, double, double, gpointer p)
{
    auto *self = static_cast<Window *>(p);
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
    openFiles(gtk_window_get_application(self->gtkWindow()), paths, self);
    return TRUE;
}

void Window::toast(const std::string &text)
{
    AdwToast *t = adw_toast_new(text.c_str());
    adw_toast_set_timeout(t, 3);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(m_toasts), t);
}

void Window::showError(const std::string &heading, const std::string &body)
{
    AdwDialog *d = adw_alert_dialog_new(heading.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "close", "Close");
    adw_dialog_present(d, m_window);
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
    gtk_widget_grab_focus(static_cast<Window *>(p)->m_search);
}

void Window::actClose(GSimpleAction *, GVariant *, gpointer p)
{
    gtk_window_close(static_cast<Window *>(p)->gtkWindow());
}

void Window::actComplement(GSimpleAction *a, GVariant *state, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    g_simple_action_set_state(a, state);
    self->m_seq->setShowComplement(g_variant_get_boolean(state));
}

void Window::actFrames(GSimpleAction *a, GVariant *state, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    g_simple_action_set_state(a, state);
    self->m_seq->setFrames(std::atoi(g_variant_get_string(state, nullptr)));
}

void Window::actClearReads(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->m_doc.clearReads();
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
        if (closeAfter) {
            m_closeConfirmed = true;
            gtk_window_close(gtkWindow());
        }
        return true;
    }
    saveDialog("Save As (.dna, .gb or .fasta)", m_doc.sequence().name + ".dna", SaveSequence, closeAfter);
    return false;
}

gboolean Window::onCloseRequest(GtkWindow *, gpointer p)
{
    auto *self = static_cast<Window *>(p);
    if (self->m_closeConfirmed || !self->m_doc.isModified())
        return FALSE;
    const std::string name = self->m_doc.sequence().name.empty() ? "this sequence" : self->m_doc.sequence().name;
    AdwDialog *d = adw_alert_dialog_new(("Save changes to " + name + "?").c_str(),
                                        "Unsaved changes are lost when the window closes.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel", "discard", "Discard", "save", "Save",
                                   nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
    g_signal_connect(d, "response", G_CALLBACK((+[](AdwAlertDialog *, const char *response, gpointer p) {
                         auto *self = static_cast<Window *>(p);
                         if (g_strcmp0(response, "discard") == 0) {
                             self->m_closeConfirmed = true;
                             gtk_window_close(self->gtkWindow());
                         } else if (g_strcmp0(response, "save") == 0) {
                             self->save(true);
                         }
                     })),
                     self);
    adw_dialog_present(d, self->m_window);
    return TRUE;
}

void Window::updateActions()
{
    auto enable = [this](const char *name, bool on) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(m_window), name);
        if (a)
            g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
    };
    const bool has = !m_doc.isEmpty();
    const bool sel = m_doc.hasSelection() && m_doc.selection().length > 0;
    enable("undo", m_doc.canUndo());
    enable("redo", m_doc.canRedo());
    for (const char *name : {"save", "save-as", "export-fasta", "export-map", "paste", "select-all",
                             "reverse-complement", "circular", "digest", "pcr", "clone", "golden-gate", "gibson", "choose-enzymes", "primer-by-sequence", "history", "align", "orfs", "map-options"})
        enable(name, has);
    enable("cut", sel);
    enable("add-feature", sel);
    enable("add-primer", sel && !m_doc.selection().wraps(m_doc.sequence().length()));
    enable("mutagenesis", has && m_doc.hasSelection() && !m_doc.selection().wraps(m_doc.sequence().length()));
    enable("design-primers", sel && !m_doc.selection().wraps(m_doc.sequence().length()));
    enable("edit-feature", m_doc.activeFeature() >= 0);
    enable("delete-feature", m_doc.activeFeature() >= 0);
    enable("set-origin", has && m_doc.sequence().circular && m_doc.hasSelection());
    enable("clear-reads", !m_doc.reads().empty());
    GAction *circ = g_action_map_lookup_action(G_ACTION_MAP(m_window), "circular");
    if (circ)
        g_simple_action_set_state(G_SIMPLE_ACTION(circ), g_variant_new_boolean(m_doc.sequence().circular));
    m_seq->setEditable(has);
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
            // The window may have closed meanwhile; look it up again by weak pointer.
            auto *weak = static_cast<GWeakRef *>(data);
            GObject *win = static_cast<GObject *>(g_weak_ref_get(weak));
            if (win && text)
                if (Window *self = static_cast<Window *>(g_object_get_data(win, "oc-window")))
                    self->pasteText(text);
            if (win)
                g_object_unref(win);
            g_free(text);
            g_weak_ref_clear(weak);
            delete weak;
        },
        [&] {
            auto *weak = new GWeakRef;
            g_weak_ref_init(weak, self->m_window);
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

void Window::actAddPrimer(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->primerDialog();
}

void Window::actDigest(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->digestDialog();
}

void Window::actPcr(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->pcrDialog();
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

void Window::actPrimerBySequence(GSimpleAction *, GVariant *, gpointer p)
{
    static_cast<Window *>(p)->primerBySequenceDialog();
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
                                 } else {
                                     const FileFormat fmt =
                                         r->kind == SaveFasta ? FileFormat::Fasta : saveFormatFor(path);
                                     if (w->saveTo(path, fmt) && r->closeAfter && !w->m_doc.isModified()) {
                                         w->m_closeConfirmed = true;
                                         gtk_window_close(w->gtkWindow());
                                     }
                                 }
                             } else if (err) {
                                 g_error_free(err);   // cancelled
                             }
                             delete r;
                         },
                         req);
    g_object_unref(dialog);
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

void openFiles(GtkApplication *app, const std::vector<std::string> &paths, Window *preferred)
{
    for (const std::string &path : paths) {
        Window *target = (preferred && preferred->isEmpty()) ? preferred : nullptr;
        if (!target) {
            // Reuse the active window if it is still empty.
            GtkWindow *active = gtk_application_get_active_window(app);
            Window *w = active ? Window::fromWidget(GTK_WIDGET(active)) : nullptr;
            target = (w && w->isEmpty()) ? w : Window::create(app);
        }
        std::string error;
        if (!target->load(path, &error)) {
            target->present();
            target->showError("Could not open " + baseName(path), error);
            continue;
        }
        target->present();
        preferred = nullptr;
    }
}

void showOpenDialog(GtkApplication *app, GtkWindow *parent)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Sequence");
    GtkFileFilter *seqs = gtk_file_filter_new();
    gtk_file_filter_set_name(seqs, "Sequence files");
    for (const char *pat : {"*.gb", "*.gbk", "*.genbank", "*.gbff", "*.ape", "*.dna", "*.fa", "*.fasta", "*.fna",
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

} // namespace oc::gtk
