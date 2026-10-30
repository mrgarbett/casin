// SPDX-License-Identifier: GPL-3.0-or-later
// Editing dialogs: feature, primer, PCR, restriction cloning, Golden Gate, Gibson, primer design and new sequence.
#include "window.h"

#include "core/align.h"
#include "core/cloning.h"
#include "core/edit.h"
#include "core/orf.h"
#include "core/primercheck.h"
#include "core/protein.h"
#include "core/search.h"
#include "view/maplayout.h"   // formatBp
#include "view/style.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <map>

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

Form makeForm(const char *title, const char *okLabel, bool withCancel = true)
{
    Form f;
    f.dialog = adw_dialog_new();
    adw_dialog_set_title(f.dialog, title);
    adw_dialog_set_content_width(f.dialog, 420);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    if (withCancel) {
        GtkWidget *cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), f.dialog);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    }
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
    GtkWidget *colorRow, *colorButton, *colorReset;
    std::string color;   // "#rrggbb"; empty = the type's default
    bool settingColor = false;
    AdwDialog *dialog;
};

std::string hexColor(const GdkRGBA &c)
{
    auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c.red), byte(c.green), byte(c.blue));
    return buf;
}

// Shows the chosen colour, or the type's default when none is set.
void showFeatureColor(FeatureForm *st)
{
    const view::Color c = view::FeatureStyle::fill(comboText(st->type), st->color);
    const GdkRGBA rgba{float(c.r), float(c.g), float(c.b), 1.0f};
    st->settingColor = true;
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(st->colorButton), &rgba);
    st->settingColor = false;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->colorRow), st->color.empty() ? "Type default" : st->color.c_str());
    gtk_widget_set_sensitive(st->colorReset, !st->color.empty());
}

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

    st->color = st->original.color;
    st->colorRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->colorRow), "Colour");
    st->colorReset = gtk_button_new_with_label("Default");
    gtk_widget_add_css_class(st->colorReset, "flat");
    gtk_widget_set_valign(st->colorReset, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(st->colorReset, "Use the colour of the feature's type");
    GtkColorDialog *picker = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(picker, FALSE);
    st->colorButton = gtk_color_dialog_button_new(picker);   // takes the dialog
    gtk_widget_set_valign(st->colorButton, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->colorRow), st->colorReset);
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->colorRow), st->colorButton);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(notes), st->colorRow);
    showFeatureColor(st);
    g_signal_connect(st->colorButton, "notify::rgba", G_CALLBACK((+[](GObject *b, GParamSpec *, gpointer p) {
                         auto *st = static_cast<FeatureForm *>(p);
                         if (st->settingColor)
                             return;
                         st->color = hexColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(b)));
                         showFeatureColor(st);
                     })),
                     st);
    g_signal_connect(st->colorReset, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<FeatureForm *>(p);
                         st->color.clear();
                         showFeatureColor(st);
                     })),
                     st);
    // While no colour is set, the swatch follows the chosen type.
    g_signal_connect(st->type, "notify::selected", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         showFeatureColor(static_cast<FeatureForm *>(p));
                     })),
                     st);

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
                         out.color = st->color;
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
    presentDialog(f.dialog);
    gtk_widget_grab_focus(st->name);
}

namespace {
struct CloneForm {
    Window *window;
    Sequence vector;
    std::vector<Sequence> sources;               // open documents that can give the insert
    std::vector<CutSite> vectorSites, insertSites;
    std::vector<std::string> vectorEnzymes, insertEnzymes;   // combo contents
    GtkWidget *vLeft, *vRight, *source, *iLeft, *iRight, *orientation, *result;
    GtkWidget *okButton;
    AdwDialog *dialog;
    Sequence product;
    bool ok = false;
    bool rebuilding = false;
};

// Enzymes cutting `seq` between 1 and `maxCuts` times, sorted by name.
std::vector<std::string> cutters(const std::vector<CutSite> &sites, int maxCuts)
{
    std::map<std::string, int> count;
    for (const CutSite &c : sites)
        if (c.enzyme)
            ++count[c.enzyme->name];
    std::vector<std::string> out;
    for (const auto &[name, n] : count)
        if (n <= maxCuts)
            out.push_back(name);
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return toLower(a) < toLower(b); });
    return out;
}

void setComboItems(GtkWidget *row, const std::vector<std::string> &items, const std::string &prefer, guint fallback)
{
    std::vector<const char *> v;
    for (const std::string &s : items)
        v.push_back(s.c_str());
    v.push_back(nullptr);
    GtkStringList *list = gtk_string_list_new(v.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(list));
    g_object_unref(list);
    auto it = std::find(items.begin(), items.end(), prefer);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), it != items.end() ? guint(it - items.begin())
                                                                     : std::min(fallback, guint(items.size() - 1)));
}

std::string endName(const FragmentEnd &e)
{
    if (e.overhang == 0)
        return e.enzyme + " (blunt)";
    return e.enzyme + (e.overhang > 0 ? " (5′ " : " (3′ ") + e.bases + ")";
}

void updateClone(CloneForm *st)
{
    if (st->rebuilding)
        return;
    st->ok = false;
    std::string err;
    ClonePiece backbone, insert;
    const guint src = adw_combo_row_get_selected(ADW_COMBO_ROW(st->source));
    const Sequence &donor = st->sources[std::min<size_t>(src, st->sources.size() - 1)];
    std::string text;
    if (!cutWithEnzymes(st->vector, st->vectorSites, comboText(st->vLeft), comboText(st->vRight), true, &backbone, &err)) {
        text = "Vector: " + err;
    } else if (!cutWithEnzymes(donor, st->insertSites, comboText(st->iLeft), comboText(st->iRight), false, &insert,
                                 &err)) {
        text = "Insert: " + err;
    } else {
        const bool reverse = adw_combo_row_get_selected(ADW_COMBO_ROW(st->orientation)) == 1;
        const ClonePiece placed = reverse ? flipPiece(insert) : insert;
        std::string otherErr;
        Sequence other;
        if (ligateCircular({backbone, placed}, &st->product, &err)) {
            // The product as the core workflow makes it: named, numbered and with its history.
            st->ok = restrictionClone(st->vector, comboText(st->vLeft), comboText(st->vRight), donor,
                                      comboText(st->iLeft), comboText(st->iRight), reverse, &st->product, &err);
            const bool both = ligateCircular({backbone, reverse ? insert : flipPiece(insert)}, &other, &otherErr);
            text = view::formatBp(st->product.length()) + " product: " + view::formatBp(backbone.length())
                 + " backbone + " + view::formatBp(insert.length()) + " insert"
                 + (both ? "\nThe insert fits both ways, so screen for orientation" : "\nDirectional: only this orientation ligates");
        } else if (ligateCircular({backbone, reverse ? insert : flipPiece(insert)}, &other, &otherErr)) {
            text = "Ligates only in the other orientation";
        } else {
            text = "Ends don't match: " + err;
        }
        text = "Vector ends " + endName(backbone.right) + " and " + endName(backbone.left) + "\n" + text;
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), text.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}

void sourceChanged(CloneForm *st)
{
    const guint src = adw_combo_row_get_selected(ADW_COMBO_ROW(st->source));
    const Sequence &donor = st->sources[std::min<size_t>(src, st->sources.size() - 1)];
    st->insertSites = findSites(donor, commonEnzymes());
    st->insertEnzymes = cutters(st->insertSites, 2);
    if (st->insertEnzymes.empty())
        st->insertEnzymes.push_back("(none)");
    st->rebuilding = true;
    setComboItems(st->iLeft, st->insertEnzymes, comboText(st->vLeft), 0);
    setComboItems(st->iRight, st->insertEnzymes, comboText(st->vRight), 1);
    st->rebuilding = false;
    updateClone(st);
}
} // namespace

void Window::cloningDialog()
{
    if (m_doc.isEmpty())
        return;
    if (!m_doc.sequence().circular) {
        toast("Restriction cloning needs a circular vector");
        return;
    }
    auto *st = new CloneForm{};
    st->window = this;
    st->vector = m_doc.sequence();
    st->vectorSites = findSites(st->vector, commonEnzymes());
    st->vectorEnzymes = cutters(st->vectorSites, 1);
    if (st->vectorEnzymes.empty()) {
        toast("No enzyme cuts this vector once");
        delete st;
        return;
    }
    // Insert sources: other windows first, then this one.
    std::vector<std::string> sourceNames;
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (!w || w == this || w->isEmpty())
            continue;
        st->sources.push_back(w->document().sequence());
        sourceNames.push_back(st->sources.back().name);
    }
    st->sources.push_back(st->vector);
    sourceNames.push_back(st->vector.name + " (this window)");

    Form f = makeForm("Restriction Cloning", "Open Product");
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *gv = addGroup(f, "Vector");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gv),
                                          (st->vector.name + ", cut with enzymes that cut once").c_str());
    st->vLeft = comboRow(gv, "First enzyme", nullptr, 0);
    st->vRight = comboRow(gv, "Second enzyme", nullptr, 0);
    // Start from the outermost enzymes that cut both the vector and the
    // first insert once: the usual way to lift an insert out with its flanks.
    std::string first, second;
    {
        const std::vector<CutSite> donorSites = findSites(st->sources.front(), commonEnzymes());
        const std::vector<std::string> once = cutters(donorSites, 1);
        int lo = INT_MAX, hi = INT_MIN;
        for (const CutSite &c : donorSites) {
            const std::string &name = c.enzyme->name;
            if (!std::binary_search(once.begin(), once.end(), name, [](const std::string &a, const std::string &b) {
                    return toLower(a) < toLower(b);
                }) || std::find(st->vectorEnzymes.begin(), st->vectorEnzymes.end(), name) == st->vectorEnzymes.end())
                continue;
            if (c.cutTop < lo) {
                lo = c.cutTop;
                first = name;
            }
            if (c.cutTop > hi) {
                hi = c.cutTop;
                second = name;
            }
        }
    }
    setComboItems(st->vLeft, st->vectorEnzymes, first, 0);
    setComboItems(st->vRight, st->vectorEnzymes, second, 1);
    GtkWidget *gi = addGroup(f, "Insert");
    std::vector<const char *> sn;
    for (const std::string &s : sourceNames)
        sn.push_back(s.c_str());
    sn.push_back(nullptr);
    st->source = comboRow(gi, "From", sn.data(), 0);
    st->iLeft = comboRow(gi, "First enzyme", nullptr, 0);
    st->iRight = comboRow(gi, "Second enzyme", nullptr, 0);
    static const char *const orient[] = {"Forward", "Reverse", nullptr};
    st->orientation = comboRow(gi, "Orientation", orient, 0);
    GtkWidget *gr = addGroup(f);
    st->result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), "Result");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gr), st->result);

    auto changed = +[](GObject *, GParamSpec *, gpointer p) { updateClone(static_cast<CloneForm *>(p)); };
    for (GtkWidget *w : {st->vLeft, st->vRight, st->iLeft, st->iRight, st->orientation})
        g_signal_connect(w, "notify::selected", G_CALLBACK(changed), st);
    g_signal_connect(st->source, "notify::selected", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         sourceChanged(static_cast<CloneForm *>(p));
                     })),
                     st);
    sourceChanged(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<CloneForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->product);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<CloneForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct GoldenGateForm {
    Window *window;
    Sequence vector;
    std::vector<Sequence> parts;          // other open documents
    std::vector<bool> chosen;
    std::vector<Enzyme> enzymes;          // Type IIS enzymes
    GtkWidget *enzyme, *result, *okButton;
    AdwDialog *dialog;
    Sequence product;
    bool ok = false;
};

void updateGoldenGate(GoldenGateForm *st)
{
    std::vector<Sequence> inputs{st->vector};
    for (size_t i = 0; i < st->parts.size(); ++i)
        if (st->chosen[i])
            inputs.push_back(st->parts[i]);
    const guint e = adw_combo_row_get_selected(ADW_COMBO_ROW(st->enzyme));
    std::string err, text;
    std::vector<AssemblyStep> steps;
    st->ok = e < st->enzymes.size() && goldenGate(inputs, st->enzymes[e], &st->product, &steps, &err);
    if (st->ok) {
        st->product.name = st->vector.name;
        text = view::formatBp(st->product.length()) + " product\n";
        for (const AssemblyStep &s : steps) {
            if (s.input > 0)
                st->product.name += "-" + inputs[size_t(s.input)].name;
            text += (s.input == 0 ? "" : " → ") + inputs[size_t(s.input)].name + (s.flipped ? " (reversed)" : "") + " "
                  + s.piece.left.bases + "…" + s.piece.right.bases;
        }
    } else {
        text = err;
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), text.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}
} // namespace

void Window::goldenGateDialog()
{
    if (m_doc.isEmpty())
        return;
    if (!m_doc.sequence().circular) {
        toast("Golden Gate needs a circular destination vector");
        return;
    }
    auto *st = new GoldenGateForm{};
    st->window = this;
    st->vector = m_doc.sequence();
    for (const Enzyme &e : commonEnzymes())
        if (e.cutsOutsideSite() && !e.cutsTwice && e.overhang() != 0)
            st->enzymes.push_back(e);
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (w && w != this && !w->isEmpty())
            st->parts.push_back(w->document().sequence());
    }
    if (st->parts.empty()) {
        toast("Open the parts in other windows first");
        delete st;
        return;
    }
    st->chosen.assign(st->parts.size(), true);
    // Start with an enzyme that cuts every input twice, else the vector.
    guint pick = 0;
    int best = 0;
    for (size_t i = 0; i < st->enzymes.size(); ++i) {
        const std::vector<Enzyme> one{st->enzymes[i]};
        if (findSites(st->vector, one).size() != 2)
            continue;
        int score = 1;
        for (const Sequence &part : st->parts)
            score += findSites(part, one).size() == 2 ? 1 : 0;
        if (score > best) {
            best = score;
            pick = guint(i);
        }
    }

    Form f = makeForm("Golden Gate Assembly", "Open Product");
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *gv = addGroup(f, "Destination vector");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gv), st->vector.name.c_str());
    std::vector<const char *> names;
    for (const Enzyme &e : st->enzymes)
        names.push_back(e.name.c_str());
    names.push_back(nullptr);
    st->enzyme = comboRow(gv, "Enzyme", names.data(), pick);
    GtkWidget *gp = addGroup(f, "Parts");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gp),
                                          "Open windows; order and orientation come from the overhangs");
    for (size_t i = 0; i < st->parts.size(); ++i) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), st->parts[i].name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), view::formatBp(st->parts[i].length()).c_str());
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        g_object_set_data(G_OBJECT(check), "index", GINT_TO_POINTER(int(i)));
        g_signal_connect(check, "toggled", G_CALLBACK((+[](GtkCheckButton *b, gpointer p) {
                             auto *st = static_cast<GoldenGateForm *>(p);
                             st->chosen[size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index")))] =
                                 gtk_check_button_get_active(b);
                             updateGoldenGate(st);
                         })),
                         st);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), row);
    }
    GtkWidget *gr = addGroup(f);
    st->result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), "Result");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gr), st->result);
    g_signal_connect(st->enzyme, "notify::selected", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         updateGoldenGate(static_cast<GoldenGateForm *>(p));
                     })),
                     st);
    updateGoldenGate(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<GoldenGateForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->product);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st,
                           [](gpointer p) { delete static_cast<GoldenGateForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct GibsonForm {
    Window *window;
    Sequence vector;
    std::vector<CutSite> vectorSites;
    std::vector<std::string> vectorEnzymes;
    std::vector<Sequence> parts;
    std::vector<bool> chosen;
    GtkWidget *vLeft, *vRight, *overlap, *result, *okButton;
    AdwDialog *dialog;
    Sequence product;
    bool ok = false;
};

// Tries the assembly with the vector opened by `left` and `right`.
bool tryGibson(GibsonForm *st, const std::string &left, const std::string &right, int minOverlap, std::string *text)
{
    std::string err;
    std::vector<Sequence> inputs;
    ClonePiece backbone;
    if (st->vector.circular) {
        if (!cutWithEnzymes(st->vector, st->vectorSites, left, right, true, &backbone, &err)) {
            *text = "Vector: " + err;
            return false;
        }
        backbone.top.name = st->vector.name;
        inputs.push_back(backbone.top);
    } else {
        inputs.push_back(st->vector);
    }
    for (size_t i = 0; i < st->parts.size(); ++i)
        if (st->chosen[i])
            inputs.push_back(st->parts[i]);
    std::vector<GibsonJoin> joins;
    if (!gibsonAssemble(inputs, minOverlap, &st->product, &joins, &err)) {
        *text = err;
        return false;
    }
    st->product.name = st->vector.name;
    // Keep the vector's numbering when its origin survived the cut.
    if (st->vector.circular && backbone.originOffset() > 0)
        rotateOrigin(st->product, backbone.originOffset());
    *text = view::formatBp(st->product.length()) + " product\n";
    for (const GibsonJoin &j : joins) {
        if (j.input > 0)
            st->product.name += "-" + inputs[size_t(j.input)].name;
        *text += (j.input == 0 ? "" : " → ") + inputs[size_t(j.input)].name + (j.flipped ? " (reversed)" : "");
    }
    int shortest = INT_MAX;
    for (const GibsonJoin &j : joins)
        shortest = std::min(shortest, j.overlap);
    *text += "\nShortest overlap " + std::to_string(shortest) + " bp";
    return true;
}

void updateGibson(GibsonForm *st)
{
    std::string text;
    const int minOverlap = int(adw_spin_row_get_value(ADW_SPIN_ROW(st->overlap)));
    st->ok = tryGibson(st, comboText(st->vLeft), comboText(st->vRight), minOverlap, &text);
    if (st->ok) {
        // The product as the core workflow makes it: named, numbered and with its history.
        std::vector<Sequence> frags;
        for (size_t i = 0; i < st->parts.size(); ++i)
            if (st->chosen[i])
                frags.push_back(st->parts[i]);
        std::string err;
        st->ok = gibsonIntoVector(st->vector, comboText(st->vLeft), comboText(st->vRight), frags, minOverlap,
                                  &st->product, nullptr, &err);
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), text.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}
} // namespace

void Window::gibsonDialog()
{
    if (m_doc.isEmpty())
        return;
    auto *st = new GibsonForm{};
    st->window = this;
    st->vector = m_doc.sequence();
    st->vectorSites = findSites(st->vector, commonEnzymes());
    st->vectorEnzymes = cutters(st->vectorSites, 1);
    if (st->vector.circular && st->vectorEnzymes.empty()) {
        toast("No enzyme cuts this vector once to open it");
        delete st;
        return;
    }
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (w && w != this && !w->isEmpty() && !w->document().sequence().circular)
            st->parts.push_back(w->document().sequence());
    }
    if (st->parts.empty()) {
        toast("Open the linear fragments in other windows first");
        delete st;
        return;
    }
    st->chosen.assign(st->parts.size(), true);

    Form f = makeForm("Gibson Assembly", "Open Product");
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *gv = addGroup(f, "Vector");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(gv),
        (st->vector.name + (st->vector.circular ? ", opened with one or two enzymes" : ", used as it is")).c_str());
    st->vLeft = comboRow(gv, "First enzyme", nullptr, 0);
    st->vRight = comboRow(gv, "Second enzyme", nullptr, 0);
    if (st->vector.circular) {
        // Start from the first way of opening the vector that assembles.
        std::string first = st->vectorEnzymes.front(), second = first, text;
        bool found = false;
        for (size_t i = 0; i < st->vectorEnzymes.size() && !found; ++i)
            for (size_t j = i; j < st->vectorEnzymes.size() && !found; ++j)
                if (tryGibson(st, st->vectorEnzymes[i], st->vectorEnzymes[j], 15, &text)) {
                    first = st->vectorEnzymes[i];
                    second = st->vectorEnzymes[j];
                    found = true;
                }
        setComboItems(st->vLeft, st->vectorEnzymes, first, 0);
        setComboItems(st->vRight, st->vectorEnzymes, second, 0);
    } else {
        gtk_widget_set_visible(st->vLeft, FALSE);
        gtk_widget_set_visible(st->vRight, FALSE);
    }
    GtkWidget *gp = addGroup(f, "Fragments");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gp),
                                          "Linear sequences in open windows; order and orientation come from the overlaps");
    for (size_t i = 0; i < st->parts.size(); ++i) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), st->parts[i].name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), view::formatBp(st->parts[i].length()).c_str());
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        g_object_set_data(G_OBJECT(check), "index", GINT_TO_POINTER(int(i)));
        g_signal_connect(check, "toggled", G_CALLBACK((+[](GtkCheckButton *b, gpointer p) {
                             auto *st = static_cast<GibsonForm *>(p);
                             st->chosen[size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index")))] =
                                 gtk_check_button_get_active(b);
                             updateGibson(st);
                         })),
                         st);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), row);
    }
    GtkWidget *gr = addGroup(f);
    st->overlap = spinRow(gr, "Minimum overlap (bp)", 15, 200);
    st->result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), "Result");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gr), st->result);
    auto changed = +[](GObject *, GParamSpec *, gpointer p) { updateGibson(static_cast<GibsonForm *>(p)); };
    g_signal_connect(st->vLeft, "notify::selected", G_CALLBACK(changed), st);
    g_signal_connect(st->vRight, "notify::selected", G_CALLBACK(changed), st);
    g_signal_connect(st->overlap, "notify::value", G_CALLBACK(changed), st);
    updateGibson(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<GibsonForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->product);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<GibsonForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct DesignForm {
    Window *window;
    Sequence seq;
    Span target;
    GtkWidget *tm, *flank, *fwd, *rev, *notes, *okButton;
    AdwDialog *dialog;
    PrimerDesign design;
    bool ok = false;
};

std::string primerSummary(const Primer &p, const PrimerCheck &c)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d nt · Tm %.1f °C · GC %.0f%%", c.length, c.tm, c.gc * 100);
    return p.bases + "\n" + buf;
}

void updateDesign(DesignForm *st)
{
    std::string err;
    st->ok = designPrimers(st->seq, st->target, adw_spin_row_get_value(ADW_SPIN_ROW(st->tm)), &st->design, &err,
                           int(adw_spin_row_get_value(ADW_SPIN_ROW(st->flank))));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->fwd),
                                st->ok ? primerSummary(st->design.forward, st->design.forwardCheck).c_str() : "");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rev),
                                st->ok ? primerSummary(st->design.reverse, st->design.reverseCheck).c_str() : "");
    std::string notes;
    if (!st->ok) {
        notes = err;
    } else {
        const std::pair<const char *, const std::vector<std::string> *> lists[] = {
            {"Forward: ", &st->design.forwardCheck.warnings},
            {"Reverse: ", &st->design.reverseCheck.warnings},
            {"", &st->design.pair.warnings}};
        for (const auto &[who, list] : lists)
            for (const std::string &w : *list)
                notes += (notes.empty() ? "" : "\n") + std::string(who) + w;
        if (notes.empty())
            notes = "No problems found";
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->notes), notes.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}
} // namespace

void Window::designPrimersDialog()
{
    if (!m_doc.hasSelection() || m_doc.selection().length <= 0)
        return;
    auto *st = new DesignForm{};
    st->window = this;
    st->seq = m_doc.sequence();
    st->target = m_doc.selection();
    Form f = makeForm("Design Primers", "Add Primers");
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *g = addGroup(f, "Amplify the selection");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(g), (view::withThousands(st->target.start + 1) + "–"
                                   + view::withThousands(st->target.start + st->target.length) + " · "
                                   + view::formatBp(st->target.length))
                                      .c_str());
    st->tm = spinRow(g, "Target Tm (°C)", 60, 72);
    adw_spin_row_set_range(ADW_SPIN_ROW(st->tm), 50, 72);
    adw_spin_row_set_value(ADW_SPIN_ROW(st->tm), 60);
    st->flank = spinRow(g, "Search outside the selection (bp)", 0, 500);
    adw_spin_row_set_range(ADW_SPIN_ROW(st->flank), 0, 500);
    // A selection too short for primers on its ends (a SNP, say) gets flanking primers.
    adw_spin_row_set_value(ADW_SPIN_ROW(st->flank), st->target.length < 18 ? 100 : 0);
    gtk_adjustment_set_step_increment(adw_spin_row_get_adjustment(ADW_SPIN_ROW(st->flank)), 10);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->flank), "0 = primers on the selection's ends");
    GtkWidget *gp = addGroup(f, "Primers");
    st->fwd = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->fwd), "Forward");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(st->fwd), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), st->fwd);
    st->rev = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->rev), "Reverse");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(st->rev), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), st->rev);
    st->notes = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->notes), "Checks");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), st->notes);
    for (GtkWidget *w : {st->tm, st->flank})
        g_signal_connect(w, "notify::value", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                             updateDesign(static_cast<DesignForm *>(p));
                         })),
                         st);
    updateDesign(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<DesignForm *>(p);
                         if (!st->ok)
                             return;
                         st->window->document().addPrimers({st->design.forward, st->design.reverse}, "Add Primers");
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<DesignForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct MutagenesisForm {
    Window *window;
    Sequence seq;
    Span target;
    GtkWidget *bases, *tm, *fwd, *rev, *result, *okButton;
    AdwDialog *dialog;
    MutagenesisDesign design;
    bool ok = false;
};

void updateMutagenesis(MutagenesisForm *st)
{
    std::string err;
    st->ok = designMutagenesis(st->seq, st->target, text(st->bases), adw_spin_row_get_value(ADW_SPIN_ROW(st->tm)),
                               &st->design, &err);
    auto primerText = [](const Primer &p, double tm) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%zu nt · anneals %d nt at %.1f °C", p.bases.size(), p.binding.length(), tm);
        return p.bases + "\n" + buf;
    };
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->fwd),
                                st->ok ? primerText(st->design.forward, st->design.forwardTm).c_str() : "");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rev),
                                st->ok ? primerText(st->design.reverse, st->design.reverseTm).c_str() : "");
    std::string msg;
    if (st->ok) {
        const int delta = st->design.mutant.length() - st->seq.length();
        msg = view::formatBp(st->design.mutant.length()) + " mutant"
            + (delta > 0 ? " (" + std::to_string(delta) + " bp inserted)"
               : delta < 0 ? " (" + std::to_string(-delta) + " bp deleted)" : "");
        for (const std::string &w : st->design.warnings)
            msg += "\n" + w;
    } else {
        msg = err;
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), msg.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}
} // namespace

void Window::mutagenesisDialog()
{
    if (m_doc.isEmpty() || !m_doc.hasSelection())
        return;
    auto *st = new MutagenesisForm{};
    st->window = this;
    st->seq = m_doc.sequence();
    st->target = m_doc.selection();
    Form f = makeForm("Mutagenesis", "Open Mutant");
    adw_dialog_set_content_height(f.dialog, 560);
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *g = addGroup(f, "Change");
    const std::string where =
        st->target.length == 0
            ? "Insert after base " + view::withThousands(st->target.start)
            : "Replace " + view::withThousands(st->target.start + 1) + "–"
                  + view::withThousands(st->target.start + st->target.length) + ": "
                  + (st->target.length <= 30 ? m_doc.selectedBases() : view::formatBp(st->target.length));
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g), where.c_str());
    st->bases = entryRow(g, st->target.length == 0 ? "Bases to insert" : "New bases (empty deletes)", "");
    st->tm = spinRow(g, "Annealing Tm (°C)", 60, 72);
    adw_spin_row_set_range(ADW_SPIN_ROW(st->tm), 50, 72);
    adw_spin_row_set_value(ADW_SPIN_ROW(st->tm), 60);
    GtkWidget *gp = addGroup(f, "Primers");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gp),
                                          "Back to back: amplify the whole plasmid, then ligate the ends");
    st->fwd = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->fwd), "Forward");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(st->fwd), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), st->fwd);
    st->rev = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->rev), "Reverse");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(st->rev), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), st->rev);
    st->result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), "Result");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), st->result);
    g_signal_connect(st->bases, "changed", G_CALLBACK((+[](GtkEditable *, gpointer p) {
                         updateMutagenesis(static_cast<MutagenesisForm *>(p));
                     })),
                     st);
    g_signal_connect(st->tm, "notify::value", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         updateMutagenesis(static_cast<MutagenesisForm *>(p));
                     })),
                     st);
    updateMutagenesis(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<MutagenesisForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->design.mutant);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st,
                           [](gpointer p) { delete static_cast<MutagenesisForm *>(p); });
    presentDialog(f.dialog);
    gtk_widget_grab_focus(st->bases);
}

void Window::historyDialog()
{
    const std::vector<std::string> &history = m_doc.sequence().history;
    Form f = makeForm("Cloning History", "Done", false);
    adw_dialog_set_content_height(f.dialog, 420);
    g_signal_connect_swapped(f.ok, "clicked", G_CALLBACK(adw_dialog_close), f.dialog);
    GtkWidget *g = addGroup(f, m_doc.sequence().name.c_str());
    if (history.empty())
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),
                                              "No recorded steps. Products of PCR, cloning, assembly and "
                                              "mutagenesis remember how they were made.");
    for (const std::string &h : history) {
        GtkWidget *row = adw_action_row_new();
        const size_t sp = h.find(' ');
        const bool dated = sp == 10;
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), dated ? h.substr(11).c_str() : h.c_str());
        if (dated)
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), h.substr(0, 10).c_str());
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), row);
    }
    presentDialog(f.dialog);
}

void Window::mapOptionsDialog()
{
    Form f = makeForm("Map Display", "Done", false);
    g_signal_connect_swapped(f.ok, "clicked", G_CALLBACK(adw_dialog_close), f.dialog);
    // Each switch carries what it controls: "@enzymes", "@primers" or a feature type.
    auto addSwitch = [this](GtkWidget *group, const std::string &title, const std::string &key, bool on) {
        GtkWidget *row = adw_switch_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), on);
        g_object_set_data_full(G_OBJECT(row), "key", g_strdup(key.c_str()), g_free);
        g_signal_connect(row, "notify::active", G_CALLBACK((+[](GObject *o, GParamSpec *, gpointer p) {
                             auto *self = static_cast<Window *>(p);
                             const std::string k = static_cast<const char *>(g_object_get_data(o, "key"));
                             const bool active = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
                             view::MapOptions opt = self->m_map->options();
                             if (k == "@enzymes")
                                 opt.enzymes = active;
                             else if (k == "@primers")
                                 opt.primers = active;
                             else if (active)
                                 opt.hiddenTypes.erase(k);
                             else
                                 opt.hiddenTypes.insert(k);
                             self->m_map->setOptions(opt);
                         })),
                         this);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    };
    const view::MapOptions &opt = m_map->options();
    GtkWidget *g = addGroup(f, "Show on the Map");
    addSwitch(g, "Enzyme sites", "@enzymes", opt.enzymes);
    addSwitch(g, "Primers", "@primers", opt.primers);
    std::map<std::string, int> types;
    for (const Feature &ft : m_doc.sequence().features)
        if (view::FeatureStyle::isDrawn(ft))
            ++types[ft.type];
    if (!types.empty()) {
        GtkWidget *tg = addGroup(f, "Feature Types");
        for (const auto &kv : types)
            addSwitch(tg, kv.first + " (" + std::to_string(kv.second) + ")", kv.first, !opt.hiddenTypes.count(kv.first));
    }
    presentDialog(f.dialog);
}

namespace {
struct SearchForm {
    Window *window;
    std::string folder;
    GtkWidget *folderRow, *query, *list, *summary;
};

void runSearch(SearchForm *st)
{
    gtk_list_box_remove_all(GTK_LIST_BOX(st->list));
    const std::string q = text(st->query);
    if (st->folder.empty() || trim(q).empty())
        return;
    const std::vector<FolderHit> hits = searchFolder(st->folder, q);
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(st->summary),
        hits.empty() ? "Nothing found" : (std::to_string(hits.size()) + (hits.size() == 1 ? " file" : " files")
                                          + "; click one to open it").c_str());
    for (const FolderHit &h : hits) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), h.name.c_str());
        const std::string sub = view::formatBp(h.length) + (h.circular ? " circular" : " linear") + " · "
                              + std::filesystem::path(h.path).filename().string() + "\n" + h.match;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub.c_str());
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data_full(G_OBJECT(row), "path", g_strdup(h.path.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(st->list), row);
    }
}
} // namespace

namespace {
struct TaForm {
    Window *window;
    Sequence vector;
    std::vector<Sequence> inserts;
    GtkWidget *insert, *method, *orientation, *result, *okButton;
    AdwDialog *dialog;
    Sequence product;
    bool ok = false;
};

void updateTa(TaForm *st)
{
    const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(st->insert));
    const guint m = adw_combo_row_get_selected(ADW_COMBO_ROW(st->method));
    const bool reversed = adw_combo_row_get_selected(ADW_COMBO_ROW(st->orientation)) == 1;
    const char overhang = m == 0 ? 'T' : m == 1 ? 'C' : 0;
    std::string err;
    st->ok = i < st->inserts.size() && overhangClone(st->vector, st->inserts[i], overhang, reversed, &st->product, &err);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result),
                                st->ok ? (st->product.name + ", " + view::formatBp(st->product.length()) + " circular").c_str()
                                       : err.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}
} // namespace

void Window::taCloningDialog()
{
    if (m_doc.sequence().circular) {
        toast("TA, GC and TOPO cloning start from a linearized vector; open it as a linear sequence");
        return;
    }
    auto *st = new TaForm{};
    st->window = this;
    st->vector = m_doc.sequence();
    std::vector<std::string> names;
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (w && w != this && !w->isEmpty() && !w->document().sequence().circular) {
            st->inserts.push_back(w->document().sequence());
            names.push_back(st->inserts.back().name);
        }
    }
    if (st->inserts.empty()) {
        toast("Open the PCR product (a linear sequence) in another window first");
        delete st;
        return;
    }
    Form f = makeForm("TA, GC and TOPO Cloning", "Open Product");
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *g = addGroup(f, ("Into " + st->vector.name).c_str());
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),
                                          "This window is the linearized vector without its overhangs");
    std::vector<const char *> v;
    for (const std::string &n : names)
        v.push_back(n.c_str());
    v.push_back(nullptr);
    st->insert = comboRow(g, "PCR product", v.data(), 0);
    static const char *methods[] = {"TA (3′ T overhangs)", "GC (3′ C overhangs)", "Blunt (TOPO blunt)", nullptr};
    st->method = comboRow(g, "Method", methods, 0);
    static const char *orient[] = {"Forward", "Reversed", nullptr};
    st->orientation = comboRow(g, "Orientation", orient, 0);
    st->result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), "Product");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), st->result);
    for (GtkWidget *w : {st->insert, st->method, st->orientation})
        g_signal_connect(w, "notify::selected", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                             updateTa(static_cast<TaForm *>(p));
                         })),
                         st);
    updateTa(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<TaForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->product);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<TaForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct SoeForm {
    Window *window;
    std::vector<Sequence> parts;   // this window first, then the other linear windows
    std::vector<bool> chosen;
    GtkWidget *overlap, *result, *okButton;
    AdwDialog *dialog;
    Sequence product;
    bool ok = false;
};

void updateSoe(SoeForm *st)
{
    std::vector<Sequence> frags;
    for (size_t i = 0; i < st->parts.size(); ++i)
        if (st->chosen[i])
            frags.push_back(st->parts[i]);
    std::vector<GibsonJoin> joins;
    std::string err, text;
    st->ok = overlapExtension(frags, int(adw_spin_row_get_value(ADW_SPIN_ROW(st->overlap))), &st->product, &joins, &err);
    if (st->ok) {
        text = view::formatBp(st->product.length()) + " linear product\n";
        for (const GibsonJoin &j : joins)
            text += (j.input == 0 ? "" : " → ") + frags[size_t(j.input)].name + (j.flipped ? " (reversed)" : "");
    } else {
        text = err;
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), text.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
}
} // namespace

void Window::overlapExtensionDialog()
{
    if (m_doc.sequence().circular) {
        toast("Overlap extension PCR joins linear fragments; this sequence is circular");
        return;
    }
    auto *st = new SoeForm{};
    st->window = this;
    st->parts.push_back(m_doc.sequence());
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (w && w != this && !w->isEmpty() && !w->document().sequence().circular)
            st->parts.push_back(w->document().sequence());
    }
    if (st->parts.size() < 2) {
        toast("Open the other linear fragments in other windows first");
        delete st;
        return;
    }
    st->chosen.assign(st->parts.size(), true);
    Form f = makeForm("Overlap Extension PCR", "Open Product");
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *gp = addGroup(f, "Fragments");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(gp),
        "Joined from this window by their shared ends; orientation comes from the overlaps");
    for (size_t i = 0; i < st->parts.size(); ++i) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), st->parts[i].name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    (view::formatBp(st->parts[i].length()) + (i == 0 ? " · first fragment" : "")).c_str());
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        gtk_widget_set_sensitive(check, i > 0);
        g_object_set_data(G_OBJECT(check), "index", GINT_TO_POINTER(int(i)));
        g_signal_connect(check, "toggled", G_CALLBACK((+[](GtkCheckButton *b, gpointer p) {
                             auto *st = static_cast<SoeForm *>(p);
                             st->chosen[size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index")))] =
                                 gtk_check_button_get_active(b);
                             updateSoe(st);
                         })),
                         st);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        if (i > 0)
            adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), row);
    }
    GtkWidget *gr = addGroup(f);
    st->overlap = spinRow(gr, "Minimum overlap (bp)", 15, 200);
    st->result = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), "Result");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gr), st->result);
    g_signal_connect(st->overlap, "notify::value", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         updateSoe(static_cast<SoeForm *>(p));
                     })),
                     st);
    updateSoe(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<SoeForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->product);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<SoeForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
GtkWidget *valueRow(GtkWidget *group, const char *title, const std::string &value)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    GtkWidget *v = gtk_label_new(value.c_str());
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_add_css_class(v, "dim-label");
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), v);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}

// "39,990": formatBp without its unit.
std::string grouped(int n)
{
    std::string s = view::formatBp(n);
    return s.substr(0, s.find(' '));
}

std::string fixed(double v, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}
} // namespace

void Window::proteinDialog()
{
    const Span sel = m_doc.selection();
    if (sel.length < 3) {
        toast("Select the coding bases, or a CDS feature, first");
        return;
    }
    // A selected feature is read on its own strand, with its exons joined.
    const Sequence &seq = m_doc.sequence();
    std::string bases = m_doc.selectedBases();
    std::string from = "Selection, forward strand";
    const int fi = m_doc.activeFeature();
    if (fi >= 0 && size_t(fi) < seq.features.size()) {
        const Feature &f = seq.features[size_t(fi)];
        if (f.span(seq.length(), seq.circular) == sel) {
            bases = seq.featureBases(f);
            from = f.name + (f.strand == Strand::Reverse ? ", reverse strand" : "");
        }
    }
    const std::string protein = translate(bases);
    const ProteinStats ps = proteinStats(protein);
    std::string shown = protein;
    if (!shown.empty() && shown.back() == '*')
        shown.pop_back();

    Form f = makeForm("Protein Properties", "Copy Protein");
    adw_dialog_set_content_height(f.dialog, 600);
    GtkWidget *gp = addGroup(f, "Translation");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gp),
                                          (from + " · " + view::formatBp(int(bases.size()))).c_str());
    GtkWidget *text = gtk_label_new(shown.c_str());
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_CHAR);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));
    gtk_label_set_attributes(GTK_LABEL(text), attrs);
    pango_attr_list_unref(attrs);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_widget_add_css_class(text, "monospace");
    gtk_widget_add_css_class(text, "card");
    gtk_widget_set_margin_bottom(text, 4);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gp), text);
    // Leave room around the text inside the card.
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "label.card { padding: 10px 12px; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(text), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *gs = addGroup(f, "Properties");
    if (ps.truncated)
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gs),
                                              "A stop codon ends the protein early; figures are up to it");
    valueRow(gs, "Length", std::to_string(ps.length) + " aa");
    valueRow(gs, "Molecular weight", fixed(ps.mass / 1000, 2) + " kDa");
    valueRow(gs, "Isoelectric point", fixed(ps.pI, 2));
    valueRow(gs, "Charged residues", std::to_string(ps.positive) + " + (K, R), " + std::to_string(ps.negative) + " − (D, E)");
    valueRow(gs, "Extinction at 280 nm", grouped(ps.extinctionReduced) + " M⁻¹cm⁻¹ (Cys reduced)");
    if (ps.extinctionCystines != ps.extinctionReduced)
        valueRow(gs, "With cystines", grouped(ps.extinctionCystines) + " M⁻¹cm⁻¹");
    if (ps.unknown > 0)
        valueRow(gs, "Unknown residues", std::to_string(ps.unknown) + " (left out of the weight)");
    GtkWidget *note = addGroup(f);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(note),
                                          "Average masses; pI from EMBOSS pKa values; extinction after Pace et al. (1995)");
    g_object_set_data_full(G_OBJECT(f.dialog), "protein", g_strdup(shown.c_str()), g_free);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *b, gpointer d) {
                         AdwDialog *dialog = ADW_DIALOG(d);
                         const char *p = static_cast<const char *>(g_object_get_data(G_OBJECT(dialog), "protein"));
                         gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), p);
                         adw_dialog_close(dialog);
                     })),
                     f.dialog);
    gtk_widget_set_sensitive(f.ok, !shown.empty());
    presentDialog(f.dialog);
}

namespace {
struct InsertionForm {
    Window *window;
    Sequence vector;
    Span site;
    std::vector<Sequence> inserts;
    std::vector<Span> targets;   // each insert's selection, or all of it
    GtkWidget *insert, *tm, *overlap, *target, *forward, *reverse, *result, *okButton;
    AdwDialog *dialog;
    InsertionDesign design;
    bool ok = false;
};

void updateInsertion(InsertionForm *st)
{
    const size_t i = adw_combo_row_get_selected(ADW_COMBO_ROW(st->insert));
    if (i >= st->inserts.size())
        return;
    const Span t = st->targets[i];
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->target),
                                (t.length == st->inserts[i].length()
                                     ? "All " + view::formatBp(t.length)
                                     : std::to_string(t.start + 1) + "–" + std::to_string(t.start + t.length) + " ("
                                           + view::formatBp(t.length) + ", its selection)")
                                    .c_str());
    std::string err;
    st->ok = designInsertion(st->vector, st->site, st->inserts[i], t, adw_spin_row_get_value(ADW_SPIN_ROW(st->tm)),
                             int(adw_spin_row_get_value(ADW_SPIN_ROW(st->overlap))), &st->design, &err);
    const int overlap = int(adw_spin_row_get_value(ADW_SPIN_ROW(st->overlap)));
    auto primerText = [&](const Primer &p, const PrimerCheck &c) {
        const std::string tail = p.bases.substr(0, size_t(overlap));
        std::string lower = tail;
        for (char &ch : lower)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        char tm[32];
        std::snprintf(tm, sizeof tm, "%.1f", c.tm);
        return lower + p.bases.substr(size_t(overlap)) + "\n" + std::to_string(p.bases.size()) + " nt · annealing part "
               + std::to_string(c.length) + " nt, Tm " + tm + " °C";
    };
    if (st->ok) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->forward),
                                    primerText(st->design.forward, st->design.annealing.forwardCheck).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->reverse),
                                    primerText(st->design.reverse, st->design.annealing.reverseCheck).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result),
                                    (view::formatBp(st->design.pcrProduct.length()) + " PCR product, "
                                     + view::formatBp(st->design.construct.length())
                                     + (st->design.construct.circular ? " circular" : " linear") + " construct")
                                        .c_str());
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->forward), "");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->reverse), "");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), err.c_str());
    }
    gtk_widget_set_sensitive(st->okButton, st->ok);
}

GtkWidget *subtitleRow(GtkWidget *group, const char *title)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}
} // namespace

void Window::insertionDialog()
{
    if (m_doc.isEmpty())
        return;
    auto *st = new InsertionForm{};
    st->window = this;
    st->vector = m_doc.sequence();
    st->site = m_doc.selection();
    std::vector<std::string> names;
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (w && w != this && !w->isEmpty()) {
            const Sequence &s = w->document().sequence();
            const Span sel = w->document().selection();
            st->inserts.push_back(s);
            st->targets.push_back(sel.length >= 18 && !sel.wraps(s.length()) ? sel : Span{0, s.length()});
            names.push_back(s.name);
        }
    }
    if (st->inserts.empty()) {
        toast("Open the insert in another window first, with the part to insert selected");
        delete st;
        return;
    }
    Form f = makeForm("Insertion by PCR", "Open Construct");
    adw_dialog_set_content_height(f.dialog, 640);
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *g = addGroup(f, ("Into " + st->vector.name).c_str());
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(g),
        (st->site.length > 0 ? "Replaces " + std::to_string(st->site.start + 1) + "–"
                                   + std::to_string(st->site.start + st->site.length)
                             : st->site.start == 0 ? std::string("Inserts before base 1")
                                                : "Inserts after base " + std::to_string(st->site.start))
            .c_str());
    std::vector<const char *> v;
    for (const std::string &n : names)
        v.push_back(n.c_str());
    v.push_back(nullptr);
    st->insert = comboRow(g, "Insert", v.data(), 0);
    st->target = subtitleRow(g, "Amplified part");
    st->tm = spinRow(g, "Annealing Tm (°C)", 60, 75);
    st->overlap = spinRow(g, "Overlap with vector (bp)", 20, 40);
    GtkWidget *gp = addGroup(f, "Primers");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gp), "Vector tails in lower case");
    st->forward = subtitleRow(gp, "Forward");
    st->reverse = subtitleRow(gp, "Reverse");
    st->result = subtitleRow(gp, "Result");
    auto changed = +[](GObject *, GParamSpec *, gpointer p) { updateInsertion(static_cast<InsertionForm *>(p)); };
    g_signal_connect(st->insert, "notify::selected", G_CALLBACK(changed), st);
    g_signal_connect(st->tm, "notify::value", G_CALLBACK(changed), st);
    g_signal_connect(st->overlap, "notify::value", G_CALLBACK(changed), st);
    updateInsertion(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<InsertionForm *>(p);
                         if (!st->ok)
                             return;
                         GtkApplication *app = gtk_window_get_application(st->window->gtkWindow());
                         Window *w = Window::create(app);
                         w->document().setSequence(st->design.construct);
                         w->document().markUnsaved();
                         w->present();
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<InsertionForm *>(p); });
    presentDialog(f.dialog);
}

void Window::searchFolderDialog()
{
    auto *st = new SearchForm{};
    st->window = this;
    if (!m_doc.path().empty())
        st->folder = std::filesystem::path(m_doc.path()).parent_path().string();
    Form f = makeForm("Search a Folder", "Search");
    adw_dialog_set_content_height(f.dialog, 560);
    GtkWidget *g = addGroup(f, "Find sequence files");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),
                                          "Searches names, descriptions, features and primers, or a DNA sequence "
                                          "on both strands (4 bases or more)");
    st->folderRow = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->folderRow), "Folder");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->folderRow), st->folder.empty() ? "None chosen" : st->folder.c_str());
    GtkWidget *choose = gtk_button_new_with_label("Choose…");
    gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->folderRow), choose);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), st->folderRow);
    st->query = entryRow(g, "Text or DNA", "");
    st->summary = addGroup(f, "Results");
    st->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(st->list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(st->list, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(st->summary), st->list);
    g_signal_connect(choose, "clicked", G_CALLBACK((+[](GtkButton *b, gpointer p) {
                         GtkFileDialog *fd = gtk_file_dialog_new();
                         gtk_file_dialog_set_title(fd, "Folder to Search");
                         GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(b)));
                         gtk_file_dialog_select_folder(
                             fd, parent, nullptr,
                             +[](GObject *src, GAsyncResult *res, gpointer p) {
                                 GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
                                 if (!file)
                                     return;
                                 auto *st = static_cast<SearchForm *>(p);
                                 char *path = g_file_get_path(file);
                                 if (path) {
                                     st->folder = path;
                                     adw_action_row_set_subtitle(ADW_ACTION_ROW(st->folderRow), path);
                                     g_free(path);
                                     runSearch(st);
                                 }
                                 g_object_unref(file);
                             },
                             p);
                         g_object_unref(fd);
                     })),
                     st);
    g_signal_connect_swapped(f.ok, "clicked", G_CALLBACK(+[](gpointer p) { runSearch(static_cast<SearchForm *>(p)); }),
                             st);
    g_signal_connect(st->list, "row-activated", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *st = static_cast<SearchForm *>(p);
                         const char *path = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "path"));
                         openFiles(gtk_window_get_application(st->window->gtkWindow()), {path});
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<SearchForm *>(p); });
    presentDialog(f.dialog);
    adw_dialog_set_focus(f.dialog, st->query);
}

namespace {
struct AlignForm {
    Window *window;
    Sequence reference;
    std::vector<Sequence> reads;
    GtkWidget *read, *summary, *diffs, *okButton, *featureButton;
    AdwDialog *dialog;
    ReadAlignment alignment;
    bool ok = false;
};

std::string diffText(const AlignmentDiff &d)
{
    if (d.ref.empty())
        return "Insertion of " + d.read;
    if (d.read.empty())
        return "Deletion of " + d.ref;
    return d.ref + " → " + d.read;
}

void updateAlign(AlignForm *st)
{
    const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(st->read));
    std::string err;
    st->ok = i < st->reads.size() && alignRead(st->reference, st->reads[i].bases, &st->alignment, &err);
    gtk_list_box_remove_all(GTK_LIST_BOX(st->diffs));
    std::string text;
    if (st->ok) {
        const ReadAlignment &a = st->alignment;
        const int n = st->reference.length();
        int end = a.refSpan.start + a.refSpan.length;
        if (end > n)
            end -= n;
        char pct[16];
        std::snprintf(pct, sizeof pct, "%.1f%%", a.identity() * 100);
        text = std::string(a.reverse ? "Bottom strand, " : "Top strand, ") + view::withThousands(a.refSpan.start + 1)
             + "–" + view::withThousands(end) + " (" + view::formatBp(a.refSpan.length) + "), " + pct + " identity\n"
             + (a.diffs.empty() ? "No differences" : std::to_string(a.diffs.size()) + (a.diffs.size() == 1 ? " difference" : " differences"));
        for (const AlignmentDiff &d : a.diffs) {
            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), diffText(d).c_str());
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), ("at " + view::withThousands(d.refPos + 1)).c_str());
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
            g_object_set_data(G_OBJECT(row), "pos", GINT_TO_POINTER(d.refPos));
            g_object_set_data(G_OBJECT(row), "len", GINT_TO_POINTER(std::max<int>(1, int(d.ref.size()))));
            gtk_list_box_append(GTK_LIST_BOX(st->diffs), row);
        }
    } else {
        text = err;
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->summary), text.c_str());
    gtk_widget_set_sensitive(st->okButton, st->ok);
    gtk_widget_set_sensitive(st->featureButton, st->ok);
}
} // namespace

void Window::alignDialog(bool compare)
{
    if (m_doc.isEmpty())
        return;
    auto *st = new AlignForm{};
    st->window = this;
    st->reference = m_doc.sequence();
    std::vector<std::string> names;
    GtkApplication *app = gtk_window_get_application(gtkWindow());
    for (Window *w : Window::all(app)) {
        if (w && w != this && !w->isEmpty() && (compare || !w->document().sequence().circular)) {
            st->reads.push_back(w->document().sequence());
            names.push_back(st->reads.back().name);
        }
    }
    if (st->reads.empty()) {
        toast(compare ? "Open the other sequence in another window first"
                      : "Open a read (.ab1, FASTA or other linear sequence) in another window first");
        delete st;
        return;
    }
    Form f = makeForm(compare ? "Compare" : "Align Read", "Show Under Sequence");
    adw_dialog_set_content_height(f.dialog, 560);
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *g = addGroup(f, ("Align to " + st->reference.name).c_str());
    std::vector<const char *> v;
    for (const std::string &s : names)
        v.push_back(s.c_str());
    v.push_back(nullptr);
    st->read = comboRow(g, compare ? "Compare with" : "Read", v.data(), 0);
    if (!compare && st->reads.size() > 1) {
        // Every open read at once, each on its own row under the sequence.
        GtkWidget *all = gtk_button_new_with_label("Show All Reads");
        gtk_widget_add_css_class(all, "flat");
        adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(g), all);
        g_signal_connect(all, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                             auto *st = static_cast<AlignForm *>(p);
                             Document &doc = st->window->document();
                             int shown = 0;
                             for (const Sequence &r : st->reads) {
                                 ReadAlignment a;
                                 if (alignRead(st->reference, r.bases, &a)) {
                                     doc.addRead(readTrack(a, doc.sequence().length(), r.name));
                                     ++shown;
                                 }
                             }
                             st->window->toast("Showing " + std::to_string(shown) + " of "
                                               + std::to_string(st->reads.size()) + " reads under the sequence");
                             adw_dialog_close(st->dialog);
                         })),
                         st);
    }
    st->summary = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->summary), "Alignment");
    st->featureButton = gtk_button_new_with_label("Add as Feature");
    gtk_widget_set_valign(st->featureButton, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->summary), st->featureButton);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), st->summary);
    GtkWidget *gd = addGroup(f, "Differences");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(gd), "Click one to select it in the sequence");
    st->diffs = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(st->diffs), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(st->diffs, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gd), st->diffs);
    g_signal_connect(st->diffs, "row-activated", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *st = static_cast<AlignForm *>(p);
                         st->window->document().setSelection(
                             {GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pos")),
                              GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "len"))});
                     })),
                     st);
    g_signal_connect(st->read, "notify::selected", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         updateAlign(static_cast<AlignForm *>(p));
                     })),
                     st);
    updateAlign(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<AlignForm *>(p);
                         if (!st->ok)
                             return;
                         const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(st->read));
                         Document &doc = st->window->document();
                         doc.addRead(readTrack(st->alignment, doc.sequence().length(), st->reads[i].name));
                         const int pos = st->alignment.diffs.empty() ? st->alignment.refSpan.start
                                                                     : st->alignment.diffs.front().refPos;
                         doc.setSelection({pos, st->alignment.diffs.empty() ? 0 : 1});
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_signal_connect(st->featureButton, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<AlignForm *>(p);
                         if (!st->ok)
                             return;
                         const ReadAlignment &a = st->alignment;
                         const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(st->read));
                         std::string note;
                         for (const AlignmentDiff &d : a.diffs)
                             note += (note.empty() ? "" : "; ") + std::to_string(d.refPos + 1) + " " + diffText(d);
                         Document &doc = st->window->document();
                         Feature f;
                         f.name = st->reads[i].name;
                         f.type = "misc_feature";
                         f.strand = a.reverse ? Strand::Reverse : Strand::Forward;
                         const int n = doc.sequence().length();
                         const int end = a.refSpan.start + a.refSpan.length;
                         f.ranges = end <= n ? std::vector<Range>{{a.refSpan.start, end}}
                                             : std::vector<Range>{{a.refSpan.start, n}, {0, end - n}};
                         f.qualifiers = {{"label", f.name},
                                         {"note", note.empty() ? "read matches the reference" : "read differs: " + note}};
                         const int idx = doc.addFeature(f);
                         if (idx >= 0)
                             doc.activateFeature(idx);
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<AlignForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct OrfForm {
    Window *window;
    Sequence seq;
    std::vector<Orf> orfs;
    std::vector<bool> chosen;
    GtkWidget *minCodons, *list, *summary, *okButton;
    AdwDialog *dialog;
};

void updateOrfs(OrfForm *st)
{
    st->orfs = findOrfs(st->seq, int(adw_spin_row_get_value(ADW_SPIN_ROW(st->minCodons))));
    if (st->orfs.size() > 200)
        st->orfs.resize(200);
    st->chosen.assign(st->orfs.size(), true);
    gtk_list_box_remove_all(GTK_LIST_BOX(st->list));
    const int n = st->seq.length();
    for (size_t i = 0; i < st->orfs.size(); ++i) {
        const Orf &o = st->orfs[i];
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      (std::to_string(o.protein.size()) + " aa, "
                                       + (o.strand == Strand::Reverse ? "reverse" : "forward") + " strand")
                                          .c_str());
        int end = o.start + o.length;
        if (end > n)
            end -= n;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), (view::withThousands(o.start + 1) + "–"
                                                          + view::withThousands(end) + " · "
                                                          + o.protein.substr(0, 24) + (o.protein.size() > 24 ? "…" : ""))
                                                             .c_str());
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(check), "index", GINT_TO_POINTER(int(i)));
        g_signal_connect(check, "toggled", G_CALLBACK((+[](GtkCheckButton *b, gpointer p) {
                             auto *st = static_cast<OrfForm *>(p);
                             st->chosen[size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index")))] =
                                 gtk_check_button_get_active(b);
                         })),
                         st);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data(G_OBJECT(row), "index", GINT_TO_POINTER(int(i)));
        gtk_list_box_append(GTK_LIST_BOX(st->list), row);
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->summary),
                                st->orfs.empty() ? "None this long"
                                                 : (std::to_string(st->orfs.size()) + " found, longest first; "
                                                    "click one to select it").c_str());
    gtk_widget_set_sensitive(st->okButton, !st->orfs.empty());
}
} // namespace

void Window::orfDialog()
{
    if (m_doc.isEmpty())
        return;
    auto *st = new OrfForm{};
    st->window = this;
    st->seq = m_doc.sequence();
    Form f = makeForm("Find ORFs", "Add as Features");
    adw_dialog_set_content_height(f.dialog, 560);
    st->dialog = f.dialog;
    st->okButton = f.ok;
    GtkWidget *g = addGroup(f);
    st->minCodons = spinRow(g, "Minimum length (codons)", 100, 3000);
    adw_spin_row_set_range(ADW_SPIN_ROW(st->minCodons), 10, 3000);
    adw_spin_row_set_value(ADW_SPIN_ROW(st->minCodons), 100);
    st->summary = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->summary), "Open reading frames, ATG to stop");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), st->summary);
    GtkWidget *gl = addGroup(f);
    st->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(st->list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(st->list, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gl), st->list);
    g_signal_connect(st->list, "row-activated", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *st = static_cast<OrfForm *>(p);
                         const Orf &o = st->orfs[size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "index")))];
                         st->window->document().setSelection({o.start, o.length});
                     })),
                     st);
    g_signal_connect(st->minCodons, "notify::value", G_CALLBACK((+[](GObject *, GParamSpec *, gpointer p) {
                         updateOrfs(static_cast<OrfForm *>(p));
                     })),
                     st);
    updateOrfs(st);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<OrfForm *>(p);
                         Document &doc = st->window->document();
                         int k = 0;
                         for (const Feature &existing : doc.sequence().features)
                             k += existing.name.rfind("ORF", 0) == 0 ? 1 : 0;
                         for (size_t i = 0; i < st->orfs.size(); ++i)
                             if (st->chosen[i])
                                 doc.addFeature(orfFeature(doc.sequence(), st->orfs[i], "ORF" + std::to_string(++k)));
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<OrfForm *>(p); });
    presentDialog(f.dialog);
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

// ---- the Mac's Sequence and Edit menu items ---------------------------------------

namespace {
struct TextForm {
    Window *window;
    Document *doc;
    GtkWidget *entry;
    AdwDialog *dialog;
};
} // namespace

void Window::renameDialog()
{
    if (m_doc.isEmpty())
        return;
    auto *st = new TextForm{this, &m_doc, nullptr, nullptr};
    Form f = makeForm("Rename Sequence", "Rename");
    st->dialog = f.dialog;
    st->entry = entryRow(addGroup(f), "Name", m_doc.sequence().name);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<TextForm *>(p);
                         const std::string name = trim(text(st->entry));
                         if (!name.empty() && name != st->doc->sequence().name)
                             st->doc->rename(name);
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<TextForm *>(p); });
    presentDialog(f.dialog);
    gtk_widget_grab_focus(st->entry);
}

void Window::goToDialog()
{
    const int n = m_doc.sequence().length();
    if (n == 0)
        return;
    auto *st = new TextForm{this, &m_doc, nullptr, nullptr};
    Form f = makeForm("Go to Position", "Go");
    st->dialog = f.dialog;
    GtkWidget *g = addGroup(f);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),
                                          ("A base number from 1 to " + view::withThousands(n) + ".").c_str());
    st->entry = entryRow(g, "Base", std::to_string(m_doc.hasSelection() ? m_doc.selection().start + 1 : 1));
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<TextForm *>(p);
                         std::string digits;
                         for (char c : text(st->entry))
                             if (std::isdigit(static_cast<unsigned char>(c)))
                                 digits.push_back(c);
                         const long base = digits.empty() || digits.size() > 9 ? 0 : std::atol(digits.c_str());
                         if (base < 1 || base > st->doc->sequence().length()) {
                             st->window->toast("Type a base number from 1 to "
                                               + view::withThousands(st->doc->sequence().length()));
                             return;
                         }
                         st->doc->setSelection({int(base) - 1, 0});
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st, [](gpointer p) { delete static_cast<TextForm *>(p); });
    presentDialog(f.dialog);
    gtk_widget_grab_focus(st->entry);
}

namespace {
struct DeletePrimerForm {
    Document *doc;
    GtkWidget *pick;
    AdwDialog *dialog;
};
} // namespace

void Window::deletePrimerDialog()
{
    const Sequence &seq = m_doc.sequence();
    if (seq.primers.empty()) {
        toast("This sequence has no primers");
        return;
    }
    std::vector<std::string> labels;
    for (const Primer &pr : seq.primers) {
        std::string site = "doesn't bind";
        if (pr.binding.length() > 0)
            site = view::siteText(pr.binding, seq.length())
                 + (pr.strand == Strand::Reverse ? " ←" : " →");
        labels.push_back(pr.name + " · " + site);
    }
    std::vector<const char *> items;
    for (const std::string &l : labels)
        items.push_back(l.c_str());
    items.push_back(nullptr);
    auto *st = new DeletePrimerForm{&m_doc, nullptr, nullptr};
    Form f = makeForm("Delete Primer", "Delete");
    gtk_widget_remove_css_class(f.ok, "suggested-action");
    gtk_widget_add_css_class(f.ok, "destructive-action");
    st->dialog = f.dialog;
    st->pick = comboRow(addGroup(f), "Primer", items.data(), 0);
    g_signal_connect(f.ok, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<DeletePrimerForm *>(p);
                         const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(st->pick));
                         if (i < st->doc->sequence().primers.size())
                             st->doc->removePrimer(int(i));
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st,
                           [](gpointer p) { delete static_cast<DeletePrimerForm *>(p); });
    presentDialog(f.dialog);
}

namespace {
struct CheckPrimerForm {
    GtkWidget *bases;
    GtkWidget *summary;
    std::vector<GtkWidget *> rows;   // length, GC, Tm, clamp, run, self-dimer, hairpin
};

void showPrimerCheck(CheckPrimerForm *st)
{
    const std::string bases = cleanBases(text(st->bases));
    const PrimerCheck c = checkPrimer(bases);
    char buf[64];
    std::vector<std::string> values;
    values.push_back(std::to_string(c.length) + " nt");
    std::snprintf(buf, sizeof buf, "%.0f%%", c.gc * 100);
    values.push_back(bases.empty() ? "—" : buf);
    std::snprintf(buf, sizeof buf, "%.1f °C", c.tm);
    values.push_back(std::isnan(c.tm) || bases.empty() ? "—" : buf);
    values.push_back(std::to_string(c.clampGC) + " of the last 5");
    values.push_back(std::to_string(c.longestRun));
    values.push_back(std::to_string(c.selfDimer) + " bp (" + std::to_string(c.selfDimer3) + " at the 3′ end)");
    values.push_back(std::to_string(c.hairpinStem) + " bp");
    for (size_t i = 0; i < st->rows.size() && i < values.size(); ++i)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->rows[i]), values[i].c_str());
    std::string warnings;
    for (const std::string &w : c.warnings)
        warnings += (warnings.empty() ? "" : "\n") + w;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->summary),
                                bases.empty() ? "Type or paste a primer." : warnings.empty() ? "Looks fine." : warnings.c_str());
}
} // namespace

void Window::checkPrimerDialog()
{
    auto *st = new CheckPrimerForm{};
    Form f = makeForm("Check a Primer", "Done", false);
    adw_dialog_set_content_height(f.dialog, 560);
    g_signal_connect_swapped(f.ok, "clicked", G_CALLBACK(adw_dialog_close), f.dialog);
    const bool sel = m_doc.hasSelection() && m_doc.selection().length > 0;
    st->bases = entryRow(addGroup(f), "Bases, 5′ to 3′", sel ? m_doc.selectedBases() : "");
    GtkWidget *g = addGroup(f);
    st->summary = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->summary), "Result");
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(st->summary), 0);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), st->summary);
    GtkWidget *details = addGroup(f);
    for (const char *title : {"Length", "GC", "Tm", "3′ GC clamp", "Longest run", "Self-dimer", "Hairpin stem"}) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(details), row);
        st->rows.push_back(row);
    }
    showPrimerCheck(st);
    g_signal_connect(st->bases, "changed", G_CALLBACK((+[](GtkEditable *, gpointer p) {
                         showPrimerCheck(static_cast<CheckPrimerForm *>(p));
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(f.dialog), "state", st,
                           [](gpointer p) { delete static_cast<CheckPrimerForm *>(p); });
    presentDialog(f.dialog);
    gtk_widget_grab_focus(st->bases);
}

} // namespace oc::gtk
