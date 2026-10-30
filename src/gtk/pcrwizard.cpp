// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcrwizard.h"
#include "window.h"

#include "core/edit.h"
#include "core/primercheck.h"
#include "view/maplayout.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace oc::gtk {

namespace {

const Polymerase kPolymerases[] = {Polymerase::Q5, Polymerase::Phusion, Polymerase::Taq};

GtkWidget *label(const char *css, bool wrap = true)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(l), wrap);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css)
        gtk_widget_add_css_class(l, css);
    return l;
}

std::string joinLines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines)
        out += (out.empty() ? "" : "\n") + l;
    return out;
}

std::string capitalised(std::string s)
{
    if (!s.empty())
        s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// The part of a primer that binds the template: its 3' end.
std::string annealing(const Primer &p)
{
    const size_t len = std::min(p.bases.size(), size_t(std::max(0, p.binding.length())));
    return p.bases.substr(p.bases.size() - len);
}

std::string place(const Primer &p, int n)
{
    return std::string(p.strand == Strand::Forward ? "→ " : "← ") + view::siteText(p.binding, n);
}

void setModel(GtkWidget *row, const std::vector<std::string> &items)
{
    std::vector<const char *> strings;
    for (const std::string &s : items)
        strings.push_back(s.c_str());
    strings.push_back(nullptr);
    GtkStringList *model = gtk_string_list_new(strings.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
}

} // namespace

PcrWizard::PcrWizard(Document *templ, GtkWidget *parent, const std::string &name)
    : m_template(templ), m_parent(parent)
{
    m_vector.setSequence(templ->sequence());
    m_vectorMap = std::make_unique<MapView>(&m_vector);
    m_map = std::make_unique<MapView>(&m_preview);
    m_seq = std::make_unique<SequenceView>(&m_preview);
    m_dialog = GTK_WIDGET(adw_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(m_dialog), ("PCR · " + name).c_str());
    adw_dialog_set_child(ADW_DIALOG(m_dialog), buildView());
    adw_dialog_set_default_widget(ADW_DIALOG(m_dialog), m_open);
    // Centred over the template's window at about 80% of its size; the
    // window dims behind it.
    const int w = parent ? gtk_widget_get_width(parent) : 1300, h = parent ? gtk_widget_get_height(parent) : 820;
    adw_dialog_set_content_width(ADW_DIALOG(m_dialog), std::max(780, int(w * 0.8)));
    adw_dialog_set_content_height(ADW_DIALOG(m_dialog), std::max(500, int(h * 0.8)));
    g_signal_connect_swapped(m_dialog, "destroy", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 // The dialog's children are disposed after this; their
                                 // signals must not reach what's about to be deleted.
                                 self->m_closing = true;
                                 disconnectTree(self->m_dialog, self);
                                 if (self->onClosed)
                                     self->onClosed();
                                 // Deleted once the dialog is fully gone: a child's
                                 // last signal still finds a live object that does
                                 // nothing.
                                 g_idle_add_once([](gpointer q) { delete static_cast<PcrWizard *>(q); }, self);
                             }),
                             this);
    fillPrimers();
    refresh();
}

PcrWizard::~PcrWizard() = default;

void PcrWizard::present()
{
    if (gtk_widget_get_parent(m_dialog))
        gtk_widget_grab_focus(m_dialog);
    else
        adw_dialog_present(ADW_DIALOG(m_dialog), m_parent);
}

void PcrWizard::close()
{
    adw_dialog_force_close(ADW_DIALOG(m_dialog));
}

void PcrWizard::templateChanged()
{
    if (m_closing)
        return;
    m_vector.setSequence(m_template->sequence());
    fillPrimers();
    refresh();
}

GtkWidget *PcrWizard::buildView()
{
    // Vector: the template's map, primers and features only; clicking a
    // primer picks it.
    view::MapOptions vectorOptions = m_vectorMap->options();
    vectorOptions.enzymes = false;
    m_vectorMap->setOptions(vectorOptions);
    m_vectorMap->onPickPrimer = [this](int index, bool adding) { pick(index, adding); };

    // Product: map over sequence, neither editable.
    m_split = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(m_split), m_map->widget());
    gtk_paned_set_end_child(GTK_PANED(m_split), m_seq->widget());
    gtk_paned_set_shrink_start_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_position(GTK_PANED(m_split), 330);
    const bool dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    m_vectorMap->setDark(dark);
    m_map->setDark(dark);
    m_seq->setDark(dark);

    m_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(m_stack, TRUE);
    gtk_stack_add_titled(GTK_STACK(m_stack), m_vectorMap->widget(), "vector", "Vector");
    gtk_stack_add_titled(GTK_STACK(m_stack), m_split, "product", "Product");
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(m_stack));

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(body), buildSidebar());
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(body), m_stack);

    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PcrWizard *>(p)->close();
                             }),
                             this);
    m_open = gtk_button_new_with_label("Open Product");
    gtk_widget_add_css_class(m_open, "suggested-action");
    gtk_widget_set_tooltip_text(m_open, "Open the product as a new, unsaved sequence");
    g_signal_connect_swapped(m_open, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PcrWizard *>(p)->openProduct();
                             }),
                             this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_open);

    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    m_toasts = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(m_toasts), body);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), m_toasts);
    return view;
}

GtkWidget *PcrWizard::buildSidebar()
{
    auto changed = +[](gpointer p) {
        auto *self = static_cast<PcrWizard *>(p);
        if (!self->m_filling)
            self->refresh();
    };

    // The cue: green once the pair makes a product, otherwise what's missing.
    m_cue = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(m_cue, "restricto-cue");
    m_cueIcon = gtk_image_new();
    gtk_widget_set_valign(m_cueIcon, GTK_ALIGN_START);
    m_cueText = label(nullptr);
    gtk_widget_set_hexpand(m_cueText, TRUE);
    gtk_box_append(GTK_BOX(m_cue), m_cueIcon);
    gtk_box_append(GTK_BOX(m_cue), m_cueText);

    // Primers: picked from the lists, clicked on the map, or typed.
    GtkWidget *primers = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(primers), "Primers");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(primers),
                                          "Or click a primer on the map, and shift-click the second.");
    m_fwd = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_fwd), "Forward");
    m_rev = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_rev), "Reverse");
    for (GtkWidget *row : {m_fwd, m_rev}) {
        g_signal_connect_swapped(row, "notify::selected", G_CALLBACK(changed), this);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(primers), row);
    }
    // A primer typed here is added to the template and picked.
    m_typed = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_typed), "Type a primer 5′→3′");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(m_typed), TRUE);
    gtk_widget_add_css_class(m_typed, "monospace");
    g_signal_connect_swapped(m_typed, "apply", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PcrWizard *>(p)->addTyped();
                             }),
                             this);
    g_signal_connect_swapped(m_typed, "entry-activated", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PcrWizard *>(p)->addTyped();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(primers), m_typed);
    GtkWidget *addPrimer = gtk_button_new_with_label("Design…");
    gtk_widget_add_css_class(addPrimer, "flat");
    gtk_widget_set_valign(addPrimer, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(addPrimer, "Design a primer in the template's Primers pane");
    g_signal_connect_swapped(addPrimer, "clicked", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 auto add = self->onAddPrimer;
                                 self->close();
                                 if (add)
                                     add();
                             }),
                             this);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(primers), addPrimer);
    // Under the rows: what happened to the primer typed there.
    m_typedNote = label("caption");
    gtk_widget_set_margin_top(m_typedNote, 6);
    gtk_widget_set_visible(m_typedNote, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(primers), m_typedNote);

    GtkWidget *polymerase = adw_preferences_group_new();
    m_polymerase = adw_combo_row_new();
    showWholeNames(ADW_COMBO_ROW(m_polymerase));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_polymerase), "Polymerase");
    std::vector<std::string> names;
    for (Polymerase p : kPolymerases)
        names.push_back(polymeraseName(p));
    setModel(m_polymerase, names);
    g_signal_connect_swapped(m_polymerase, "notify::selected", G_CALLBACK(changed), this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(polymerase), m_polymerase);

    // The product: its size, big, then where it comes from.
    m_productGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_productGroup), "Product");
    GtkWidget *product = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    m_size = label("title-1", false);
    gtk_widget_add_css_class(m_size, "numeric");
    m_info = label("caption");
    gtk_label_set_selectable(GTK_LABEL(m_info), TRUE);
    gtk_box_append(GTK_BOX(product), m_size);
    gtk_box_append(GTK_BOX(product), m_info);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_productGroup), product);
    m_warnings = label("caption");
    gtk_widget_add_css_class(m_warnings, "warning");

    // The suggested program as a table, with Copy for the lab notebook.
    m_programGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_programGroup), "Suggested Program");
    GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_add_css_class(copy, "flat");
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(copy, "Copy the program as a Markdown table");
    g_signal_connect_swapped(copy, "clicked", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 if (self->m_markdown.empty())
                                     return gtk_widget_error_bell(self->m_dialog);
                                 gdk_clipboard_set_text(gtk_widget_get_clipboard(self->m_dialog),
                                                        self->m_markdown.c_str());
                                 AdwToast *toast = adw_toast_new("Program copied as a Markdown table");
                                 adw_toast_set_timeout(toast, 2);
                                 adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->m_toasts), toast);
                             }),
                             this);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(m_programGroup), copy);
    m_program = gtk_grid_new();
    gtk_widget_add_css_class(m_program, "card");
    gtk_widget_add_css_class(m_program, "restricto-program");
    gtk_grid_set_column_spacing(GTK_GRID(m_program), 14);
    gtk_grid_set_row_spacing(GTK_GRID(m_program), 5);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_programGroup), m_program);
    GtkWidget *cycles = adw_preferences_group_new();
    m_cycles = adw_spin_row_new_with_range(15, 45, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_cycles), "Cycles");
    adw_spin_row_set_value(ADW_SPIN_ROW(m_cycles), 30);
    g_signal_connect_swapped(m_cycles, "notify::value", G_CALLBACK(changed), this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(cycles), m_cycles);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_programGroup), cycles);
    m_note = label("caption");
    gtk_widget_add_css_class(m_note, "dim-label");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_programGroup), m_note);

    GtkWidget *inputs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(inputs, 16);
    gtk_widget_set_margin_end(inputs, 16);
    gtk_widget_set_margin_top(inputs, 14);
    gtk_widget_set_margin_bottom(inputs, 18);
    for (GtkWidget *w : {m_cue, primers, polymerase, m_productGroup, m_warnings, m_programGroup})
        gtk_box_append(GTK_BOX(inputs), w);
    GtkWidget *sidebar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar), inputs);
    gtk_widget_set_size_request(sidebar, 340, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);   // the maps take the extra width
    gtk_widget_add_css_class(sidebar, "restricto-wizard-sidebar");
    return sidebar;
}

int PcrWizard::picked(GtkWidget *row, const std::vector<int> &list) const
{
    const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    return i >= 1 && i <= list.size() ? list[i - 1] : -1;
}

// Lists the template's primers that read the right way in each row, after a
// "Choose" entry, keeping what was picked when it's still there. At first, a
// pair is picked only when there's just one primer each way.
void PcrWizard::fillPrimers()
{
    const int keepF = picked(m_fwd, m_forward), keepR = picked(m_rev, m_reverse);
    m_filling = true;
    const auto &primers = m_template->sequence().primers;
    for (bool forward : {true, false}) {
        std::vector<int> &list = forward ? m_forward : m_reverse;
        GtkWidget *row = forward ? m_fwd : m_rev;
        list.clear();
        std::vector<std::string> items{"Choose…"};
        for (size_t i = 0; i < primers.size(); ++i) {
            const Primer &p = primers[i];
            if (p.binding.length() <= 0 || (p.strand == Strand::Forward) != forward)
                continue;
            list.push_back(int(i));
            items.push_back(p.name + "   " + place(p, m_template->sequence().length()));
        }
        setModel(row, items);
        gtk_widget_set_sensitive(row, !list.empty());
        const int keep = forward ? keepF : keepR;
        guint select = !m_filled && list.size() == 1 ? 1 : 0;
        for (size_t k = 0; k < list.size(); ++k)
            if (list[k] == keep)
                select = guint(k + 1);
        adw_combo_row_set_selected(ADW_COMBO_ROW(row), select);
    }
    m_filled = true;
    m_filling = false;
}

// A primer clicked on the map goes in for its strand. A plain click starts
// over with it; shift-click adds it to the one already picked.
void PcrWizard::pick(int primer, bool adding)
{
    if (m_closing)
        return;
    const auto &primers = m_template->sequence().primers;
    if (primer < 0 || size_t(primer) >= primers.size() || primers[size_t(primer)].binding.length() <= 0) {
        gtk_widget_error_bell(m_dialog);
        return;
    }
    const bool forward = primers[size_t(primer)].strand == Strand::Forward;
    const std::vector<int> &list = forward ? m_forward : m_reverse;
    const auto at = std::find(list.begin(), list.end(), primer);
    if (at == list.end())
        return;
    m_filling = true;
    adw_combo_row_set_selected(ADW_COMBO_ROW(forward ? m_fwd : m_rev), guint(at - list.begin() + 1));
    if (!adding)
        adw_combo_row_set_selected(ADW_COMBO_ROW(forward ? m_rev : m_fwd), 0);
    m_filling = false;
    refresh();
}

// Adds what's typed to the template as a new primer (one undo step there)
// and picks it.
void PcrWizard::addTyped()
{
    if (m_closing)
        return;
    const std::string typed = cleanPrimerBases(gtk_editable_get_text(GTK_EDITABLE(m_typed)));
    if (typed.empty()) {
        // Enter in an empty box does what Enter does elsewhere.
        openProduct();
        return;
    }
    const std::string bases = cleanBases(typed);
    const std::vector<PrimerSite> sites = findPrimerSites(m_template->sequence(), bases);
    gtk_widget_remove_css_class(m_typedNote, "dim-label");
    gtk_widget_add_css_class(m_typedNote, "warning");
    gtk_widget_set_visible(m_typedNote, TRUE);
    if (bases.size() < 15 || sites.empty()) {
        gtk_label_set_text(GTK_LABEL(m_typedNote), bases.size() < 15 ? "A primer needs at least 15 bases."
                                                                     : "That primer doesn't bind this sequence.");
        gtk_widget_error_bell(m_dialog);
        return;
    }
    std::set<std::string> names;
    for (const Primer &p : m_template->sequence().primers)
        names.insert(p.name);
    int n = 1;
    while (names.count("primer" + std::to_string(n)))
        ++n;
    Primer p;
    p.name = "primer" + std::to_string(n);
    p.bases = typed;
    p.binding = sites.front().binding;
    p.strand = sites.front().strand;
    // The template's window hears of it and refills the lists here.
    m_template->addPrimers({p}, "Add Primer");
    gtk_editable_set_text(GTK_EDITABLE(m_typed), "");
    const int index = int(m_template->sequence().primers.size()) - 1;
    pick(index, true);
    gtk_label_set_text(GTK_LABEL(m_typedNote), ("Added " + p.name + " " + place(p, m_template->sequence().length()) + " to the template.").c_str());
    gtk_widget_remove_css_class(m_typedNote, "warning");
    gtk_widget_add_css_class(m_typedNote, "dim-label");
}

void PcrWizard::setCue(const std::string &text, const char *icon, const char *css)
{
    gtk_label_set_text(GTK_LABEL(m_cueText), text.c_str());
    gtk_image_set_from_icon_name(GTK_IMAGE(m_cueIcon), icon);
    for (const char *c : {"ready", "problem"})
        gtk_widget_remove_css_class(m_cue, c);
    if (css)
        gtk_widget_add_css_class(m_cue, css);
}

// Makes the product from the picked primers, draws it, and fills in the
// readout and the suggested program.
void PcrWizard::refresh()
{
    if (m_closing)
        return;
    const Sequence &templ = m_template->sequence();
    const int fi = picked(m_fwd, m_forward), ri = picked(m_rev, m_reverse);
    const Primer *fp = fi >= 0 ? &templ.primers[size_t(fi)] : nullptr;
    const Primer *rp = ri >= 0 ? &templ.primers[size_t(ri)] : nullptr;
    std::vector<std::string> lines, warn;
    m_hasProduct = false;
    if (fp && rp) {
        Sequence product;
        std::string err;
        if (pcrProduct(templ, *fp, *rp, &product, &err)) {
            m_preview.setSequence(product);
            m_preview.markUnsaved();
            m_hasProduct = true;
        } else {
            warn.push_back(capitalised(err.empty() ? "these primers make no product" : err) + ".");
        }
    }
    const Polymerase pol = kPolymerases[std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(m_polymerase)), 2)];
    const int cycles = int(adw_spin_row_get_value(ADW_SPIN_ROW(m_cycles)));
    PcrProgram program;
    m_markdown.clear();
    Span shown{0, 0};
    if (m_hasProduct) {
        const int length = m_preview.sequence().length();
        const int n = templ.length();
        int end = rp->binding.end;
        if (end <= fp->binding.start)
            end += n;   // across the origin
        else if (end - fp->binding.start > n)
            end -= n;   // the reverse site runs across the origin, past the forward one
        const int copied = end - fp->binding.start;
        gtk_label_set_text(GTK_LABEL(m_size), (view::withThousands(length) + " bp").c_str());
        const std::string name = templ.name.empty() ? std::string("the template") : templ.name;
        std::string copiedLine = "Copies " + view::withThousands(fp->binding.start + 1) + "–"
                               + view::withThousands((end - 1) % n + 1) + " of " + name;
        if (length > copied)
            copiedLine += ", plus " + std::to_string(length - copied) + " bp of primer tails";
        lines.push_back(copiedLine);
        // Annealing Tms in the polymerase's buffer: the parts that bind;
        // tails join in from cycle 2.
        program = pcrProgram(annealing(*fp), annealing(*rp), length, pol, cycles);
        if (!program.steps.empty()) {
            const int a = program.tm1, b = program.tm2;
            lines.push_back("Tm " + std::to_string(a) + " °C (" + fp->name + ") · " + std::to_string(b) + " °C ("
                            + rp->name + "), in " + polymeraseName(pol) + " buffer");
            if (std::abs(a - b) > 5)
                warn.push_back("The annealing Tms differ by " + std::to_string(std::abs(a - b)) + " °C; fit "
                               + (a > b ? fp->name : rp->name) + " closer in the Primers pane.");
            m_markdown = pcrProgramMarkdown(program, polymeraseName(pol) + " · " + fp->name + " + " + rp->name
                                                         + " · " + view::withThousands(length) + " bp");
        }
        for (const std::string &w : checkPrimerPair(fp->bases, rp->bases).warnings)
            if (w.rfind("Tm differs", 0) != 0)
                warn.push_back(w);
        if (length > polymeraseMaxLength(pol))
            warn.push_back(polymeraseName(pol) + " is sold for products up to "
                           + std::to_string(polymeraseMaxLength(pol) / 1000) + " kb.");
        shown = {fp->binding.start, copied};
    } else if (fp || rp) {
        // One primer picked: show where it binds.
        const Primer &one = fp ? *fp : *rp;
        shown = {one.binding.start, one.binding.length()};
    }
    // The template's window, and the vector map here, show what gets copied.
    if (shown.length > 0) {
        m_template->setSelection(shown);
        m_vector.setSelection(shown);
    }

    if (m_hasProduct)
        setCue("Ready: a " + view::withThousands(m_preview.sequence().length())
                   + " bp product. See the Product tab.",
               "object-select-symbolic", "ready");
    else if (m_forward.empty() || m_reverse.empty())
        setCue("PCR needs a primer on each strand facing the other. Type one below, or design one in the "
               "template's Primers pane.",
               "dialog-information-symbolic", nullptr);
    else if (!fp || !rp)
        setCue("Pick a forward and a reverse primer.", "dialog-information-symbolic", nullptr);
    else
        setCue("No product with these primers.", "dialog-warning-symbolic", "problem");

    gtk_stack_page_set_visible(gtk_stack_get_page(GTK_STACK(m_stack), m_split), m_hasProduct);
    if (!m_hasProduct)
        gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "vector");
    gtk_label_set_text(GTK_LABEL(m_info), joinLines(lines).c_str());
    gtk_widget_set_visible(m_productGroup, m_hasProduct);
    gtk_label_set_text(GTK_LABEL(m_warnings), joinLines(warn).c_str());
    gtk_widget_set_visible(m_warnings, !warn.empty());
    fillProgram(program);
    gtk_widget_set_sensitive(m_open, m_hasProduct);
}

// The program as a table: step, temperature, time, and the cycle count
// beside the first repeated step; a note when Ta was held or merged.
void PcrWizard::fillProgram(const PcrProgram &program)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(m_program)))
        gtk_grid_remove(GTK_GRID(m_program), child);
    gtk_widget_set_visible(m_programGroup, m_hasProduct && !program.steps.empty());
    if (program.steps.empty())
        return;
    auto cell = [&](const std::string &text, int col, int row, bool right, const char *css) {
        GtkWidget *l = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), right ? 1 : 0);
        if (css)
            gtk_widget_add_css_class(l, css);
        gtk_widget_set_hexpand(l, col == 0);
        gtk_grid_attach(GTK_GRID(m_program), l, col, row, 1, 1);
    };
    cell("STEP", 0, 0, false, "head");
    cell("°C", 1, 0, true, "head");
    cell("TIME", 2, 0, true, "head");
    cell("", 3, 0, false, "head");
    bool cyclesShown = false;
    int row = 1;
    for (const PcrStep &s : program.steps) {
        // The repeated steps stand out from the ones done once.
        const char *css = s.cycled ? nullptr : "once";
        cell(s.name, 0, row, false, css);
        cell(std::to_string(s.celsius), 1, row, true, css);
        cell(formatSeconds(s.seconds), 2, row, true, css);
        std::string repeats;
        if (s.cycled && !cyclesShown) {
            repeats = "× " + std::to_string(program.cycles);
            cyclesShown = true;
        }
        cell(repeats, 3, row, false, "accent");
        ++row;
    }
    std::vector<std::string> notes;
    if (program.capped)
        notes.push_back("Annealing held at its highest for this polymerase.");
    if (program.twoStep)
        notes.push_back("Primers this hot anneal at the extension temperature, so the cycle has two steps.");
    std::string note;
    for (const std::string &n : notes)
        note += (note.empty() ? "" : " ") + n;
    gtk_label_set_text(GTK_LABEL(m_note), note.c_str());
    gtk_widget_set_visible(m_note, !note.empty());
}

void PcrWizard::openProduct()
{
    if (m_closing)
        return;
    if (!m_hasProduct) {
        gtk_widget_error_bell(m_dialog);
        return;
    }
    const Sequence product = m_preview.sequence();
    auto open = onOpenProduct;
    close();
    if (open)
        open(product);
}

} // namespace oc::gtk
