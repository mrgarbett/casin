// SPDX-License-Identifier: GPL-3.0-or-later
// Editing dialogs: feature, primer and new sequence.
#include "window.h"

#include "view/style.h"

#include <algorithm>

namespace oc::gtk {

namespace {

const char *const kFeatureTypes[] = {
    "misc_feature", "CDS",        "gene",         "promoter",  "terminator", "rep_origin",   "RBS",
    "enhancer",     "polyA_signal", "primer_bind", "protein_bind", "sig_peptide", "LTR",     "ncRNA",
    "misc_RNA",     "regulatory", "mobile_element", "exon",    "intron",     "5'UTR",        "3'UTR",
};

const char *const kStrands[] = {"Forward", "Reverse", "None", nullptr};

// A dialog with Cancel and an action button in its header bar.
struct Form {
    AdwDialog *dialog;
    GtkWidget *page;
    GtkWidget *ok;
};

Form makeForm(const char *title, const char *okLabel)
{
    Form f;
    f.dialog = adw_dialog_new();
    adw_dialog_set_title(f.dialog, title);
    adw_dialog_set_content_width(f.dialog, 420);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), f.dialog);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    f.ok = gtk_button_new_with_label(okLabel);
    gtk_widget_add_css_class(f.ok, "suggested-action");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), f.ok);
    f.page = adw_preferences_page_new();
    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), f.page);
    adw_dialog_set_child(f.dialog, tv);
    adw_dialog_set_default_widget(f.dialog, f.ok);
    return f;
}

GtkWidget *addGroup(const Form &f, const char *title = nullptr)
{
    GtkWidget *g = adw_preferences_group_new();
    if (title)
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(f.page), ADW_PREFERENCES_GROUP(g));
    return g;
}

GtkWidget *entryRow(GtkWidget *group, const char *title, const std::string &text)
{
    GtkWidget *row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_editable_set_text(GTK_EDITABLE(row), text.c_str());
    adw_entry_row_set_activates_default(ADW_ENTRY_ROW(row), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}

GtkWidget *comboRow(GtkWidget *group, const char *title, const char *const *items, guint selected)
{
    GtkWidget *row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    GtkStringList *list = gtk_string_list_new(items);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(list));
    g_object_unref(list);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}

GtkWidget *spinRow(GtkWidget *group, const char *title, int value, int max)
{
    GtkWidget *row = adw_spin_row_new_with_range(1, std::max(1, max), 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_spin_row_set_value(ADW_SPIN_ROW(row), value);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}

std::string comboText(GtkWidget *row)
{
    GObject *item = static_cast<GObject *>(adw_combo_row_get_selected_item(ADW_COMBO_ROW(row)));
    return item ? gtk_string_object_get_string(GTK_STRING_OBJECT(item)) : "";
}

std::string text(GtkWidget *editable)
{
    return gtk_editable_get_text(GTK_EDITABLE(editable));
}

struct FeatureForm {
    Window *window;
    Document *doc;
    int index;
    Feature original;
    bool multiPart;
    GtkWidget *name, *type, *strand, *start, *end, *note;
    AdwDialog *dialog;
};

} // namespace

void Window::featureDialog(int index)
{
    const Sequence &seq = m_doc.sequence();
    const int n = seq.length();
    if (n == 0)
        return;
    auto *st = new FeatureForm{};
    st->window = this;
    st->doc = &m_doc;
    st->index = index;
    Span sp;
    if (index >= 0 && index < int(seq.features.size())) {
        st->original = seq.features[size_t(index)];
        sp = st->original.span(n, seq.circular);
    } else {
        if (!m_doc.hasSelection() || m_doc.selection().length <= 0) {
            toast("Select the bases for the new feature first");
            delete st;
            return;
        }
        st->index = -1;
        sp = m_doc.selection();
        st->original.type = "misc_feature";
        st->original.strand = Strand::Forward;
    }
    st->multiPart = st->original.ranges.size() > 2
                 || (st->original.ranges.size() == 2 && !sp.wraps(n));

    Form f = makeForm(st->index < 0 ? "Add Feature" : "Edit Feature", st->index < 0 ? "Add" : "Save");
    st->dialog = f.dialog;
    GtkWidget *g = addGroup(f);
    st->name = entryRow(g, "Name", st->original.name);

    std::vector<const char *> types(std::begin(kFeatureTypes), std::end(kFeatureTypes));
    if (std::none_of(types.begin(), types.end(), [&](const char *t) { return st->original.type == t; }))
        types.insert(types.begin(), st->original.type.c_str());
    guint typeIndex = 0;
    for (size_t i = 0; i < types.size(); ++i)
        if (st->original.type == types[i])
            typeIndex = guint(i);
    types.push_back(nullptr);
    st->type = comboRow(g, "Type", types.data(), typeIndex);
    st->strand = comboRow(g, "Strand", kStrands,
                          st->original.strand == Strand::Reverse ? 1 : st->original.strand == Strand::None ? 2 : 0);

    GtkWidget *loc = addGroup(f, "Location");
    int endBase = sp.end() > n ? sp.end() - n : sp.end();
    st->start = spinRow(loc, "Start", sp.start + 1, n);
    st->end = spinRow(loc, "End", endBase, n);
    if (st->multiPart) {
        gtk_widget_set_sensitive(st->start, FALSE);
        gtk_widget_set_sensitive(st->end, FALSE);
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(loc),
            ("This feature has " + std::to_string(st->original.ranges.size()) + " parts; its parts stay as they are.")
                .c_str());
    } else if (seq.circular) {
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(loc),
                                              "An end before the start runs across the origin.");
    }

    GtkWidget *notes = addGroup(f);
    st->note = entryRow(notes, "Note", st->original.qualifier("note"));

    if (st->index >= 0) {
        GtkWidget *dg = addGroup(f);
        GtkWidget *del = gtk_button_new_with_label("Delete Feature");
        gtk_widget_add_css_class(del, "destructive-action");
        gtk_widget_add_css_class(del, "pill");
        gtk_widget_set_halign(del, GTK_ALIGN_CENTER);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dg), del);
        g_signal_connect(del, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                             auto *st = static_cast<FeatureForm *>(p);
                             st->doc->removeFeature(st->index);
                             adw_dialog_close(st->dialog);
                         })),
                         st);
    }

    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<FeatureForm *>(p);
                         const Sequence &seq = st->doc->sequence();
                         const int n = seq.length();
                         Feature out = st->original;
                         out.type = comboText(st->type);
                         const std::string strand = comboText(st->strand);
                         out.strand = strand == "Reverse" ? Strand::Reverse
                                    : strand == "None"    ? Strand::None
                                                          : Strand::Forward;
                         out.name = trim(text(st->name));
                         if (out.name.empty())
                             out.name = defaultFeatureName(out.type, out.qualifiers);
                         // Keep other qualifiers; the first note is the one edited here.
                         const std::string note = trim(text(st->note));
                         auto it = std::find_if(out.qualifiers.begin(), out.qualifiers.end(),
                                                [](const Qualifier &q) { return q.key == "note"; });
                         if (it != out.qualifiers.end()) {
                             if (note.empty())
                                 out.qualifiers.erase(it);
                             else
                                 it->value = note;
                         } else if (!note.empty()) {
                             out.qualifiers.push_back({"note", note});
                         }
                         // A typed name becomes the label qualifier, which GenBank keeps.
                         auto label = std::find_if(out.qualifiers.begin(), out.qualifiers.end(),
                                                   [](const Qualifier &q) { return q.key == "label"; });
                         if (label != out.qualifiers.end())
                             label->value = out.name;
                         else if (out.name != defaultFeatureName(out.type, out.qualifiers))
                             out.qualifiers.insert(out.qualifiers.begin(), {"label", out.name});
                         if (!st->multiPart) {
                             const int a = int(adw_spin_row_get_value(ADW_SPIN_ROW(st->start)));
                             const int b = int(adw_spin_row_get_value(ADW_SPIN_ROW(st->end)));
                             if (a <= b) {
                                 out.ranges = {{a - 1, b}};
                             } else if (seq.circular) {
                                 out.ranges = {{a - 1, n}, {0, b}};
                             } else {
                                 st->window->toast("On a linear sequence the end must come after the start");
                                 return;
                             }
                         }
                         if (st->index >= 0)
                             st->doc->replaceFeature(st->index, out);
                         else
                             st->doc->addFeature(out);
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<FeatureForm *>(p); });
    adw_dialog_present(f.dialog, m_window);
    gtk_widget_grab_focus(st->name);
}

namespace {
struct PrimerForm {
    Document *doc;
    GtkWidget *name, *strand;
    AdwDialog *dialog;
};
} // namespace

void Window::primerDialog()
{
    if (!m_doc.hasSelection() || m_doc.selection().length <= 0) {
        toast("Select the primer's binding site first");
        return;
    }
    if (m_doc.selection().wraps(m_doc.sequence().length())) {
        toast("Primers can't cross the origin yet");
        return;
    }
    auto *st = new PrimerForm{&m_doc, nullptr, nullptr, nullptr};
    Form f = makeForm("Add Primer", "Add");
    st->dialog = f.dialog;
    GtkWidget *g = addGroup(f);
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(g),
        ("Binds the selected " + view::formatBp(m_doc.selection().length) + ". A reverse primer reads the bottom strand.")
            .c_str());
    st->name = entryRow(g, "Name", "Primer " + std::to_string(m_doc.sequence().primers.size() + 1));
    static const char *const strands[] = {"Forward", "Reverse", nullptr};
    st->strand = comboRow(g, "Direction", strands, 0);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<PrimerForm *>(p);
                         st->doc->addPrimerFromSelection(text(st->name), comboText(st->strand) == "Reverse"
                                                                             ? Strand::Reverse
                                                                             : Strand::Forward);
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<PrimerForm *>(p); });
    adw_dialog_present(f.dialog, m_window);
}

namespace {
struct NewForm {
    GtkApplication *app;
    GtkWidget *name, *circular, *bases;
    AdwDialog *dialog;
};
} // namespace

void showNewSequenceDialog(GtkApplication *app, GtkWindow *parent)
{
    auto *st = new NewForm{app, nullptr, nullptr, nullptr, nullptr};
    Form f = makeForm("New Sequence", "Create");
    adw_dialog_set_content_height(f.dialog, 480);
    st->dialog = f.dialog;
    GtkWidget *g = addGroup(f);
    st->name = entryRow(g, "Name", "Untitled");
    st->circular = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->circular), "Circular");
    adw_switch_row_set_active(ADW_SWITCH_ROW(st->circular), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), st->circular);

    GtkWidget *bg = addGroup(f, "Bases");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(bg),
                                          "Type or paste DNA. Numbers, spaces and other characters are ignored.");
    st->bases = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->bases), GTK_WRAP_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->bases), TRUE);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->bases), 8);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->bases), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(st->bases), 8);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), st->bases);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    gtk_widget_add_css_class(scroll, "card");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(bg), scroll);

    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<NewForm *>(p);
                         GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->bases));
                         GtkTextIter a, b;
                         gtk_text_buffer_get_bounds(buf, &a, &b);
                         char *raw = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
                         Sequence seq;
                         seq.bases = cleanBases(raw ? raw : "");
                         g_free(raw);
                         if (seq.bases.empty()) {
                             gtk_widget_add_css_class(st->bases, "error");
                             return;
                         }
                         seq.name = trim(text(st->name));
                         if (seq.name.empty())
                             seq.name = "Untitled";
                         seq.circular = adw_switch_row_get_active(ADW_SWITCH_ROW(st->circular));
                         GtkWindow *active = gtk_application_get_active_window(st->app);
                         Window *w = active ? Window::fromWidget(GTK_WIDGET(active)) : nullptr;
                         if (!w || !w->isEmpty())
                             w = Window::create(st->app);
                         w->document().setSequence(std::move(seq));
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<NewForm *>(p); });
    adw_dialog_present(f.dialog, GTK_WIDGET(parent));
    gtk_widget_grab_focus(st->bases);
}

} // namespace oc::gtk
