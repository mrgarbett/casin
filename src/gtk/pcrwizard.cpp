// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcrwizard.h"

#include "core/edit.h"
#include "core/primercheck.h"
#include "view/maplayout.h"

#include <adwaita.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oc::gtk {

namespace {

// A polymerase's cycling: the temperatures and times its maker suggests, and
// the longest product it's sold for.
struct Polymerase {
    const char *name;
    int denature;           // °C
    int initialSeconds;
    int denatureSeconds;
    double annealOffset;    // from the lower annealing Tm
    int annealSeconds;
    int extend;             // °C
    int secondsPerKb;
    int minExtendSeconds;
    int maxLength;
    int finalMinutes;

    // The cycling for a product of `length` bp whose primers' lower annealing Tm is `tmLow`.
    std::string program(double tmLow, int length) const
    {
        const int anneal = int(std::min(72.0, std::max(45.0, std::round(tmLow + annealOffset))));
        const int perKb = (length * secondsPerKb + 999) / 1000;
        const int extendSeconds = std::max(minExtendSeconds, (perKb + 4) / 5 * 5);
        auto time = [](int s) {
            if (s >= 60 && s % 60 == 0)
                return std::to_string(s / 60) + " min";
            if (s > 60)
                return std::to_string(s / 60) + " min " + std::to_string(s % 60) + " s";
            return std::to_string(s) + " s";
        };
        return std::to_string(denature) + " °C  " + time(initialSeconds) + "\n30 cycles:\n  "
             + std::to_string(denature) + " °C  " + time(denatureSeconds) + "\n  " + std::to_string(anneal) + " °C  "
             + time(annealSeconds) + "\n  " + std::to_string(extend) + " °C  " + time(extendSeconds) + "\n"
             + std::to_string(extend) + " °C  " + std::to_string(finalMinutes) + " min";
    }
};

const Polymerase kPolymerases[] = {
    {"Q5 / Phusion (high fidelity)", 98, 30, 10, 3, 20, 72, 30, 15, 20000, 2},
    {"Taq", 95, 120, 30, -5, 30, 68, 60, 30, 5000, 5},
};

GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "caption-heading");
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_margin_top(l, 8);
    return l;
}

GtkWidget *wrapLabel(const char *css)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, css);
    return l;
}

double annealingTm(const Primer &p)
{
    const int len = std::min(int(p.bases.size()), p.binding.length());
    return checkPrimer(p.bases.substr(p.bases.size() - size_t(len))).tm;
}

std::string fmt(const char *format, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

std::string joinLines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines)
        out += (out.empty() ? "" : "\n") + l;
    return out;
}

} // namespace

PcrWizard::PcrWizard(Document *templ, GtkWindow *parent, const std::string &name) : m_template(templ)
{
    m_map = std::make_unique<MapView>(&m_preview);
    m_seq = std::make_unique<SequenceView>(&m_preview);
    m_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(m_window), ("PCR · " + name).c_str());
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1040, 660);
    gtk_widget_set_size_request(m_window, 760, 480);
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(m_window), parent);
    gtk_window_set_hide_on_close(GTK_WINDOW(m_window), FALSE);
    adw_window_set_content(ADW_WINDOW(m_window), buildView());
    g_signal_connect_swapped(m_window, "destroy", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 if (self->onClosed)
                                     self->onClosed();
                                 delete self;
                             }),
                             this);
    fillPrimers();
    refresh();
}

PcrWizard::~PcrWizard() = default;

void PcrWizard::present()
{
    gtk_window_present(GTK_WINDOW(m_window));
}

void PcrWizard::close()
{
    gtk_window_destroy(GTK_WINDOW(m_window));
}

void PcrWizard::templateChanged()
{
    fillPrimers();
    refresh();
}

GtkWidget *PcrWizard::buildView()
{
    m_fwd = gtk_drop_down_new_from_strings(nullptr);
    m_rev = gtk_drop_down_new_from_strings(nullptr);
    std::vector<const char *> names;
    for (const Polymerase &p : kPolymerases)
        names.push_back(p.name);
    names.push_back(nullptr);
    m_polymerase = gtk_drop_down_new_from_strings(names.data());
    for (GtkWidget *d : {m_fwd, m_rev, m_polymerase})
        g_signal_connect_swapped(d, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                     auto *self = static_cast<PcrWizard *>(p);
                                     if (!self->m_filling)
                                         self->refresh();
                                 }),
                                 this);
    m_addPrimer = gtk_button_new_with_label("Add Primer");
    gtk_widget_set_halign(m_addPrimer, GTK_ALIGN_START);
    gtk_widget_set_margin_top(m_addPrimer, 4);
    gtk_widget_set_tooltip_text(m_addPrimer, "Add a primer in the template's Primers pane");
    g_signal_connect_swapped(m_addPrimer, "clicked", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 if (self->onAddPrimer)
                                     self->onAddPrimer();
                             }),
                             this);
    m_info = wrapLabel("caption");
    m_warnings = wrapLabel("caption");
    gtk_widget_add_css_class(m_warnings, "warning");
    m_program = wrapLabel("monospace");
    gtk_widget_add_css_class(m_program, "dim-label");
    m_productHeading = heading("PRODUCT");
    // The program's heading, with a button to copy it for the cycler's notes.
    GtkWidget *copy = gtk_button_new_with_label("Copy");
    gtk_widget_add_css_class(copy, "flat");
    gtk_widget_set_tooltip_text(copy, "Copy the program as text");
    g_signal_connect_swapped(copy, "clicked", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 gdk_clipboard_set_text(gtk_widget_get_clipboard(self->m_window),
                                                        gtk_label_get_text(GTK_LABEL(self->m_program)));
                             }),
                             this);
    m_programRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *programHeading = heading("SUGGESTED PROGRAM");
    gtk_widget_set_hexpand(programHeading, TRUE);
    gtk_box_append(GTK_BOX(m_programRow), programHeading);
    gtk_box_append(GTK_BOX(m_programRow), copy);

    GtkWidget *inputs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(inputs, 16);
    gtk_widget_set_margin_end(inputs, 16);
    gtk_widget_set_margin_top(inputs, 8);
    gtk_widget_set_margin_bottom(inputs, 16);
    for (GtkWidget *w : {heading("FORWARD PRIMER"), m_fwd, heading("REVERSE PRIMER"), m_rev, m_addPrimer,
                         heading("POLYMERASE"), m_polymerase, m_productHeading, m_info, m_warnings, m_programRow,
                         m_program})
        gtk_box_append(GTK_BOX(inputs), w);
    GtkWidget *sidebar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar), inputs);
    gtk_widget_set_size_request(sidebar, 300, -1);

    // The product: map over sequence, neither editable.
    m_split = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(m_split), m_map->widget());
    gtk_paned_set_end_child(GTK_PANED(m_split), m_seq->widget());
    gtk_paned_set_shrink_start_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_position(GTK_PANED(m_split), 340);
    m_map->setDark(adw_style_manager_get_dark(adw_style_manager_get_default()));
    m_seq->setDark(adw_style_manager_get_dark(adw_style_manager_get_default()));
    // Shown instead of the product when there is none.
    m_empty = adw_status_page_new();
    m_stack = gtk_stack_new();
    gtk_widget_set_hexpand(m_stack, TRUE);
    gtk_stack_add_named(GTK_STACK(m_stack), m_split, "product");
    gtk_stack_add_named(GTK_STACK(m_stack), m_empty, "empty");

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(body), sidebar);
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(body), m_stack);

    GtkWidget *header = adw_header_bar_new();
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PcrWizard *>(p)->close();
                             }),
                             this);
    m_open = gtk_button_new_with_label("Open Product");
    gtk_widget_add_css_class(m_open, "suggested-action");
    gtk_widget_set_tooltip_text(m_open, "Open the product as a new, unsaved sequence");
    g_signal_connect_swapped(m_open, "clicked", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PcrWizard *>(p);
                                 if (!self->m_hasProduct)
                                     return;
                                 if (self->onOpenProduct)
                                     self->onOpenProduct(self->m_preview.sequence());
                                 self->close();
                             }),
                             this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_open);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);

    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), body);
    // Esc cancels; Enter opens the product.
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey *, guint key, guint, GdkModifierType,
                                                         gpointer p) -> gboolean {
                         auto *self = static_cast<PcrWizard *>(p);
                         if (key == GDK_KEY_Escape) {
                             self->close();
                             return TRUE;
                         }
                         if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && self->m_hasProduct) {
                             gtk_widget_activate(self->m_open);
                             return TRUE;
                         }
                         return FALSE;
                     }),
                     this);
    gtk_widget_add_controller(m_window, keys);
    return view;
}

// Lists the template's primers that read the right way in each dropdown,
// keeping what was picked when it's still there.
void PcrWizard::fillPrimers()
{
    auto picked = [](GtkWidget *d, const std::vector<Choice> &list) {
        const guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
        return i < list.size() ? list[i].index : -1;
    };
    const int keepF = picked(m_fwd, m_forward), keepR = picked(m_rev, m_reverse);
    m_filling = true;
    const auto &primers = m_template->sequence().primers;
    for (bool forward : {true, false}) {
        std::vector<Choice> &list = forward ? m_forward : m_reverse;
        GtkWidget *d = forward ? m_fwd : m_rev;
        list.clear();
        std::vector<std::string> items;
        for (size_t i = 0; i < primers.size(); ++i) {
            const Primer &p = primers[i];
            if (p.binding.length() <= 0 || (p.strand == Strand::Forward) != forward)
                continue;
            list.push_back({int(i), p.binding});
            items.push_back(p.name + "   " + (forward ? "→ " : "← ") + view::withThousands(p.binding.start + 1) + "–"
                            + view::withThousands(p.binding.end));
        }
        if (items.empty())
            items.push_back(forward ? "No forward primer" : "No reverse primer");
        std::vector<const char *> strings;
        for (const std::string &s : items)
            strings.push_back(s.c_str());
        strings.push_back(nullptr);
        GtkStringList *model = gtk_string_list_new(strings.data());
        gtk_drop_down_set_model(GTK_DROP_DOWN(d), G_LIST_MODEL(model));
        g_object_unref(model);
        gtk_widget_set_sensitive(d, !list.empty());
        const int keep = forward ? keepF : keepR;
        guint select = 0;
        for (size_t k = 0; k < list.size(); ++k)
            if (list[k].index == keep)
                select = guint(k);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(d), select);
    }
    m_filling = false;
}

// Makes the product from the picked primers, draws it, and fills in the
// readout and the suggested program.
void PcrWizard::refresh()
{
    const Sequence &templ = m_template->sequence();
    const guint fi = gtk_drop_down_get_selected(GTK_DROP_DOWN(m_fwd));
    const guint ri = gtk_drop_down_get_selected(GTK_DROP_DOWN(m_rev));
    const bool canPick = !m_forward.empty() && !m_reverse.empty();
    std::vector<std::string> lines, warn;
    std::string program;
    m_hasProduct = false;
    if (canPick && fi < m_forward.size() && ri < m_reverse.size()) {
        const Primer &fp = templ.primers[size_t(m_forward[fi].index)];
        const Primer &rp = templ.primers[size_t(m_reverse[ri].index)];
        Sequence product;
        std::string err;
        if (pcrProduct(templ, fp, rp, &product, &err)) {
            m_preview.setSequence(product);
            m_preview.markUnsaved();
            m_hasProduct = true;
        } else {
            if (err.empty())
                err = "these primers make no product";
            err[0] = char(std::toupper(static_cast<unsigned char>(err[0])));
            warn.push_back(err + ".");
        }
        if (m_hasProduct) {
            const Polymerase &pol =
                kPolymerases[std::min<guint>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_polymerase)), 1)];
            const int length = m_preview.sequence().length();
            const int n = templ.length();
            int end = rp.binding.end;
            if (end <= fp.binding.start)
                end += n;   // across the origin
            const int copied = end - fp.binding.start;
            lines.push_back(view::withThousands(length) + " bp");
            std::string copiedLine = "Copies " + view::withThousands(fp.binding.start + 1) + "–"
                                   + view::withThousands((end - 1) % n + 1) + " of "
                                   + (templ.name.empty() ? std::string("the template") : templ.name);
            if (length > copied)
                copiedLine += ", plus " + std::to_string(length - copied) + " bp of primer tails";
            lines.push_back(copiedLine);
            // Annealing Tms: the parts that bind; tails join in from cycle 2.
            const double a = annealingTm(fp), b = annealingTm(rp);
            if (!std::isnan(a) && !std::isnan(b)) {
                lines.push_back("Tm " + fmt("%.1f", a) + " °C (" + fp.name + ") · " + fmt("%.1f", b) + " °C ("
                                + rp.name + ")");
                if (std::abs(a - b) > 5)
                    warn.push_back("The annealing Tms differ by " + fmt("%.1f", std::abs(a - b)) + " °C; fit "
                                   + (a > b ? fp.name : rp.name) + " closer in the Primers pane.");
                program = pol.program(std::min(a, b), length);
            }
            for (const std::string &w : checkPrimerPair(fp.bases, rp.bases).warnings)
                if (w.rfind("Tm differs", 0) != 0)
                    warn.push_back(w);
            if (length > pol.maxLength)
                warn.push_back(std::string(pol.name) + " is sold for products up to "
                               + std::to_string(pol.maxLength / 1000) + " kb.");
            // The template's window shows what gets copied.
            m_template->setSelection({fp.binding.start, copied});
        }
    }
    if (!canPick) {
        adw_status_page_set_title(ADW_STATUS_PAGE(m_empty), "No Primer Pair");
        adw_status_page_set_description(ADW_STATUS_PAGE(m_empty),
                                        "PCR needs a primer on each strand facing the other. Add them in the "
                                        "template's Primers pane, then pick them here.");
    } else if (!m_hasProduct) {
        adw_status_page_set_title(ADW_STATUS_PAGE(m_empty), "No Product");
        adw_status_page_set_description(ADW_STATUS_PAGE(m_empty), "No product with these primers.");
    }
    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), m_hasProduct ? "product" : "empty");
    gtk_widget_set_visible(m_addPrimer, !(canPick && m_hasProduct));
    gtk_label_set_text(GTK_LABEL(m_info), joinLines(lines).c_str());
    gtk_widget_set_visible(m_info, !lines.empty());
    gtk_label_set_text(GTK_LABEL(m_warnings), joinLines(warn).c_str());
    gtk_widget_set_visible(m_warnings, !warn.empty());
    gtk_label_set_text(GTK_LABEL(m_program), program.c_str());
    gtk_widget_set_visible(m_program, !program.empty());
    gtk_widget_set_visible(m_programRow, !program.empty());
    gtk_widget_set_visible(m_productHeading, !lines.empty() || !warn.empty());
    gtk_widget_set_sensitive(m_open, m_hasProduct);
}

} // namespace oc::gtk
