// SPDX-License-Identifier: GPL-3.0-or-later
#include "primereditor.h"

#include "core/tm.h"
#include "view/maplayout.h"

#include <adwaita.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace oc::gtk {

namespace {

// ---- the parts that can go into a primer (as in the Mac's PrimerParts) -------------

struct AminoAcid {
    std::string name;                 // "Leu (L)"
    std::string shortName;            // "Leu", or "Stop"
    std::vector<std::string> codons;  // the one E. coli uses most first
};

const std::vector<AminoAcid> &aminoAcids()
{
    static const std::vector<AminoAcid> list = [] {
        const char *bases = "TCAG";
        const char *code = "FFLLSSSSYY**CC*WLLLLPPPPHHQQRRRRIIIMTTTTNNKKSSRRVVVVAAAADDEEGGGG";
        auto codonsOf = [&](char aa) {
            std::vector<std::string> all;
            for (int i = 0; i < 64; ++i)
                if (code[i] == aa)
                    all.push_back({bases[i / 16], bases[i / 4 % 4], bases[i % 4]});
            return all;
        };
        // The codon E. coli uses most for each amino acid.
        const std::pair<char, const char *> preferred[] = {
            {'A', "GCG"}, {'R', "CGC"}, {'N', "AAC"}, {'D', "GAT"}, {'C', "TGC"}, {'Q', "CAG"}, {'E', "GAA"},
            {'G', "GGC"}, {'H', "CAT"}, {'I', "ATT"}, {'L', "CTG"}, {'K', "AAA"}, {'M', "ATG"}, {'F', "TTT"},
            {'P', "CCG"}, {'S', "AGC"}, {'T', "ACC"}, {'W', "TGG"}, {'Y', "TAT"}, {'V', "GTG"}, {'*', "TAA"},
        };
        auto ordered = [&](char aa) {
            std::vector<std::string> all = codonsOf(aa);
            for (const auto &p : preferred)
                if (p.first == aa) {
                    auto it = std::find(all.begin(), all.end(), p.second);
                    if (it != all.end())
                        std::rotate(all.begin(), it, it + 1);
                }
            return all;
        };
        const std::pair<char, const char *> names[] = {
            {'A', "Ala"}, {'R', "Arg"}, {'N', "Asn"}, {'D', "Asp"}, {'C', "Cys"}, {'Q', "Gln"}, {'E', "Glu"},
            {'G', "Gly"}, {'H', "His"}, {'I', "Ile"}, {'L', "Leu"}, {'K', "Lys"}, {'M', "Met"}, {'F', "Phe"},
            {'P', "Pro"}, {'S', "Ser"}, {'T', "Thr"}, {'W', "Trp"}, {'Y', "Tyr"}, {'V', "Val"},
        };
        std::vector<AminoAcid> out;
        for (const auto &n : names)
            out.push_back({std::string(n.second) + " (" + n.first + ")", n.second, ordered(n.first)});
        out.push_back({"Stop", "Stop", ordered('*')});
        return out;
    }();
    return list;
}

struct Tag {
    const char *name;
    const char *protein;   // what it encodes; "/" marks where a protease cuts
    const char *bases;
    const char *shortName;
    bool proteaseSite;
};

// Common tags and protease sites, as E. coli-friendly codons.
const Tag kTags[] = {
    {"His6 tag", "HHHHHH", "CATCATCACCACCATCAC", "His6", false},
    {"FLAG tag", "DYKDDDDK", "GATTACAAGGATGACGACGATAAG", "FLAG", false},
    {"HA tag", "YPYDVPDYA", "TACCCATACGATGTTCCAGATTACGCT", "HA", false},
    {"Myc tag", "EQKLISEEDL", "GAACAAAAACTCATCTCAGAAGAGGATCTG", "Myc", false},
    {"Strep-tag II", "WSHPQFEK", "TGGAGCCACCCGCAGTTCGAAAAA", "Strep-tag II", false},
    {"TEV site", "ENLYFQ/G", "GAAAACCTGTATTTTCAGGGC", "TEV", true},
    {"HRV 3C (PreScission) site", "LEVLFQ/GP", "CTGGAAGTTCTGTTCCAGGGGCCC", "HRV 3C", true},
    {"Thrombin site", "LVPR/GS", "CTGGTGCCGCGTGGCAGC", "Thrombin", true},
    {"Factor Xa site", "IEGR/", "ATCGAAGGTCGT", "Factor Xa", true},
    {"Enterokinase site", "DDDDK/", "GATGACGATGACAAG", "Enterokinase", true},
};

// Bases put before an enzyme site at a primer's 5' end: most enzymes need a
// few bases past their site to cut near a DNA end. Chosen to make no common
// site of its own.
const char kSitePadding[] = "TTCTAT";

bool isBaseLetter(char c)
{
    static const std::string letters = "ACGTURYKMSWBDHVNacgturykmswbdhvn";
    return letters.find(c) != std::string::npos;
}

std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c = char(c - 32);
    return s;
}

std::string fmt(const char *format, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

// The Tm NEB's calculator gives for these bases in Q5's buffer (NaN for
// anything but ACGT): what the pane shows, and what Fit aims at.
double q5Tm(const std::string &bases)
{
    return polymeraseTmExact(cleanBases(bases), Polymerase::Q5);
}

std::string joinLines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines)
        out += (out.empty() ? "" : "\n") + l;
    return out;
}

bool startsWith(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "caption-heading");
    gtk_widget_add_css_class(l, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_set_margin_top(l, 8);
    return l;
}

GtkWidget *dropDown(const std::vector<std::string> &items)
{
    std::vector<const char *> strings;
    for (const std::string &s : items)
        strings.push_back(s.c_str());
    strings.push_back(nullptr);
    GtkWidget *d = gtk_drop_down_new_from_strings(strings.data());
    gtk_widget_set_hexpand(d, TRUE);
    return d;
}

void setDropDownItems(GtkWidget *d, const std::vector<std::string> &items)
{
    auto *list = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(d)));
    std::vector<const char *> strings;
    for (const std::string &s : items)
        strings.push_back(s.c_str());
    strings.push_back(nullptr);
    gtk_string_list_splice(list, 0, g_list_model_get_n_items(G_LIST_MODEL(list)), strings.data());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d), 0);
}

int selectedIndex(GtkWidget *d)
{
    const guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(d));
    return i == GTK_INVALID_LIST_POSITION ? -1 : int(i);
}

GdkRGBA partColor(PrimerPart::Kind k)
{
    switch (k) {
    case PrimerPart::Kind::Codon: return {0.18f, 0.76f, 0.49f, 1};        // green
    case PrimerPart::Kind::EnzymeSite: return {0.57f, 0.25f, 0.67f, 1};   // purple
    case PrimerPart::Kind::Tag: return {0.21f, 0.52f, 0.89f, 1};          // blue
    case PrimerPart::Kind::ProteaseSite: return {0.90f, 0.38f, 0.0f, 1};  // orange
    }
    return {0.5f, 0.5f, 0.5f, 1};
}

} // namespace

// ---- construction ------------------------------------------------------------------

PrimerEditor::PrimerEditor(Document *doc, const std::string &bases) : m_doc(doc)
{
    m_hadSelection = doc->hasSelection();
    m_selectionBefore = doc->selection();
    m_root = buildView();
    g_object_ref_sink(m_root);
    setBases(bases);
}

PrimerEditor::~PrimerEditor()
{
    if (!m_finished)
        m_doc->clearPendingPrimer();
    g_signal_handlers_disconnect_by_data(m_buffer, this);
    g_object_unref(m_root);
}

GtkWidget *PrimerEditor::buildView()
{
    GtkWidget *stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(stack, 12);
    gtk_widget_set_margin_end(stack, 12);
    gtk_widget_set_margin_bottom(stack, 12);

    m_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(m_name), "primer");
    gtk_editable_set_text(GTK_EDITABLE(m_name), nextPrimerName().c_str());
    m_description = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(m_description), "Optional");
    for (GtkWidget *e : {m_name, m_description}) {
        g_signal_connect_swapped(e, "changed", G_CALLBACK(+[](gpointer p) {
                                     static_cast<PrimerEditor *>(p)->refresh(false);
                                 }),
                                 this);
        g_signal_connect_swapped(e, "activate", G_CALLBACK(+[](gpointer p) {
                                     static_cast<PrimerEditor *>(p)->accept();
                                 }),
                                 this);
    }

    m_color = gtk_color_dialog_button_new(gtk_color_dialog_new());
    const GdkRGBA orange = {0.90f, 0.38f, 0.0f, 1};
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(m_color), &orange);
    gtk_widget_set_tooltip_text(m_color, "The primer's colour on the map and sequence");
    g_signal_connect_swapped(m_color, "notify::rgba", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PrimerEditor *>(p);
                                 self->m_colorChosen = true;
                                 self->refresh(false);
                             }),
                             this);
    GtkWidget *colorRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(colorRow), m_color);
    GtkWidget *colorLabel = gtk_label_new("Colour");
    gtk_box_append(GTK_BOX(colorRow), colorLabel);

    // The sequence box, with the parts' marks drawn over it.
    m_text = gtk_text_view_new();
    m_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(m_text));
    gtk_text_buffer_set_enable_undo(m_buffer, FALSE);   // the box keeps its own, with the marks
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_text), GTK_WRAP_CHAR);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(m_text), 16);
    gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(m_text), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(m_text), 6);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(m_text), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(m_text), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(m_text), 4);
    gtk_text_view_set_input_hints(GTK_TEXT_VIEW(m_text),
                                  GtkInputHints(GTK_INPUT_HINT_NO_SPELLCHECK | GTK_INPUT_HINT_NO_EMOJI));
    m_tintGood = gtk_text_buffer_create_tag(m_buffer, nullptr, "background-rgba", nullptr, nullptr);
    m_tintBad = gtk_text_buffer_create_tag(m_buffer, nullptr, "background-rgba", nullptr, nullptr);
    const GdkRGBA green = {0.18f, 0.76f, 0.49f, 0.25f}, amber = {0.90f, 0.38f, 0.0f, 0.22f};
    g_object_set(m_tintGood, "background-rgba", &green, nullptr);
    g_object_set(m_tintBad, "background-rgba", &amber, nullptr);
    g_signal_connect(m_buffer, "insert-text", G_CALLBACK(&PrimerEditor::insertText), this);
    g_signal_connect(m_buffer, "delete-range", G_CALLBACK(&PrimerEditor::deleteRange), this);
    g_signal_connect_swapped(m_buffer, "changed", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PrimerEditor *>(p);
                                 if (!self->m_restoring)
                                     self->refresh();
                                 gtk_widget_queue_draw(self->m_marks);
                             }),
                             this);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(&PrimerEditor::boxKey), this);
    gtk_widget_add_controller(m_text, keys);
    m_marks = gtk_drawing_area_new();
    gtk_widget_set_can_target(m_marks, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_marks), &PrimerEditor::drawMarks, this, nullptr);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), m_text);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), m_marks);
    GtkWidget *textScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(textScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(textScroll), overlay);
    gtk_widget_set_size_request(textScroll, -1, 110);
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_child(GTK_FRAME(frame), textScroll);

    // On when the bases are the reverse complement of what was typed, so the
    // primer binds the other strand; inserts then go in reverse complemented
    // too, so they still read on the strand they were typed on.
    m_flip = gtk_switch_new();
    gtk_widget_set_valign(m_flip, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(m_flip, "notify::active", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<PrimerEditor *>(p);
                                 if (self->m_restoring)
                                     return;
                                 self->noteUndo(false);
                                 // Noted after the switch moved: undo puts it back.
                                 self->m_undo.back().flipped = !gtk_switch_get_active(GTK_SWITCH(self->m_flip));
                                 self->flip();
                             }),
                             this);
    GtkWidget *flipRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(flipRow), m_flip);
    gtk_box_append(GTK_BOX(flipRow), gtk_label_new("Reverse complement"));
    gtk_widget_set_tooltip_text(flipRow, "Bind the other strand. What you insert still reads on the strand you typed.");

    m_info = gtk_label_new("");
    m_warnings = gtk_label_new("");
    for (GtkWidget *l : {m_info, m_warnings}) {
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_add_css_class(l, "caption");
    }
    gtk_widget_add_css_class(m_warnings, "warning");

    // Target Tm: the annealing part grows or shrinks along the template to meet it.
    m_tm = gtk_spin_button_new_with_range(30, 80, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(m_tm), 65);
    g_signal_connect_swapped(m_tm, "value-changed", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PrimerEditor *>(p)->fitToTm();
                             }),
                             this);
    GtkWidget *fit = gtk_button_new_with_label("Fit");
    gtk_widget_set_tooltip_text(fit, "Grow or shrink the annealing part along the template to this Tm (Q5, as NEB works it out)");
    g_signal_connect_swapped(fit, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PrimerEditor *>(p)->fitToTm();
                             }),
                             this);
    GtkWidget *tmRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(tmRow), m_tm);
    gtk_box_append(GTK_BOX(tmRow), gtk_label_new("°C"));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(tmRow), spacer);
    gtk_box_append(GTK_BOX(tmRow), fit);

    // Partner: none, or one of the document's primers.
    std::vector<std::string> partners = {"None"};
    const auto &primers = m_doc->sequence().primers;
    for (size_t i = 0; i < primers.size(); ++i)
        if (primers[i].binding.length() > 0) {
            partners.push_back(primers[i].name);
            m_partnerIndex.push_back(int(i));
        }
    m_partner = dropDown(partners);
    g_signal_connect_swapped(m_partner, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PrimerEditor *>(p)->refresh();
                             }),
                             this);

    // Codon: an amino acid, then one of its codons.
    std::vector<std::string> aas;
    for (const AminoAcid &aa : aminoAcids())
        aas.push_back(aa.name);
    m_aminoAcid = dropDown(aas);
    m_codon = dropDown({});
    gtk_widget_set_hexpand(m_codon, FALSE);
    fillCodons();
    g_signal_connect_swapped(m_aminoAcid, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PrimerEditor *>(p)->fillCodons();
                             }),
                             this);
    GtkWidget *codonRow = insertRow(m_aminoAcid, m_codon, +[](GtkButton *, gpointer p) {
        auto *self = static_cast<PrimerEditor *>(p);
        const int i = selectedIndex(self->m_codon);
        if (i >= 0 && i < int(self->m_codonParts.size()))
            self->insertPart(self->m_codonParts[size_t(i)]);
    });

    // Enzyme site: the unambiguous sites, with how often each cuts here.
    std::vector<const Enzyme *> enzymes;
    for (const Enzyme &e : m_doc->enzymes())
        if (!e.site.empty() && e.site.find_first_not_of("ACGT") == std::string::npos)
            enzymes.push_back(&e);
    std::sort(enzymes.begin(), enzymes.end(), [](const Enzyme *a, const Enzyme *b) {
        return g_ascii_strcasecmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    std::vector<std::string> enzymeNames;
    guint ecoRI = 0;
    for (const Enzyme *e : enzymes) {
        const int cuts = m_doc->cutCount(e->name);
        if (e->name == "EcoRI")
            ecoRI = guint(enzymeNames.size());
        enzymeNames.push_back(e->name + "  " + e->site + "  · "
                              + (cuts == 0 ? std::string("doesn't cut") : "cuts " + std::to_string(cuts) + "×"));
        m_enzymeParts.push_back({PrimerPart::Kind::EnzymeSite, e->name, e->site});
    }
    m_enzyme = dropDown(enzymeNames);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(m_enzyme), ecoRI);
    GtkWidget *enzymeRow = insertRow(m_enzyme, nullptr, +[](GtkButton *, gpointer p) {
        auto *self = static_cast<PrimerEditor *>(p);
        const int i = selectedIndex(self->m_enzyme);
        if (i >= 0 && i < int(self->m_enzymeParts.size()))
            self->insertPart(self->m_enzymeParts[size_t(i)]);
    });
    m_pad = gtk_check_button_new_with_label("Pad a 5′-end site so it cuts");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(m_pad), TRUE);
    gtk_widget_set_tooltip_text(m_pad, (std::string("Most enzymes cut poorly right at a DNA end; ") + kSitePadding
                                        + " goes in before a site at the 5′ end.")
                                           .c_str());

    // Tags and protease sites.
    std::vector<std::string> tagNames;
    for (const Tag &t : kTags) {
        tagNames.push_back(std::string(t.name) + "  · " + t.protein);
        m_tagParts.push_back({t.proteaseSite ? PrimerPart::Kind::ProteaseSite : PrimerPart::Kind::Tag, t.shortName,
                              t.bases});
    }
    m_tag = dropDown(tagNames);
    GtkWidget *tagRow = insertRow(m_tag, nullptr, +[](GtkButton *, gpointer p) {
        auto *self = static_cast<PrimerEditor *>(p);
        const int i = selectedIndex(self->m_tag);
        if (i >= 0 && i < int(self->m_tagParts.size()))
            self->insertPart(self->m_tagParts[size_t(i)]);
    });

    GtkWidget *hint = gtk_label_new("Inserts go in at the cursor, marked underneath with which way they read.");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "caption");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_set_margin_top(hint, 8);

    struct Item { const char *heading; GtkWidget *w; };
    for (const Item &it : {Item{"NAME", m_name}, Item{"DESCRIPTION", m_description}, Item{"TAG", colorRow},
                           Item{"SEQUENCE (5′ → 3′)", frame}, Item{nullptr, flipRow}, Item{nullptr, m_info},
                           Item{nullptr, m_warnings}, Item{"ANNEALING Tm (Q5)", tmRow},
                           Item{"PARTNER PRIMER", m_partner}, Item{"CODON", codonRow},
                           Item{"ENZYME SITE", enzymeRow}, Item{nullptr, m_pad},
                           Item{"TAG OR PROTEASE SITE", tagRow}, Item{nullptr, hint}}) {
        if (it.heading)
            gtk_box_append(GTK_BOX(stack), heading(it.heading));
        gtk_box_append(GTK_BOX(stack), it.w);
    }
    gtk_widget_set_margin_top(m_info, 4);

    // Cancel and Add stay at the top, beside the title, where they're seen.
    GtkWidget *title = gtk_label_new("New Primer");
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_set_hexpand(title, TRUE);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PrimerEditor *>(p)->cancel();
                             }),
                             this);
    m_add = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(m_add, "suggested-action");
    gtk_widget_set_tooltip_text(m_add, "Add the primer (Enter)");
    g_signal_connect_swapped(m_add, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<PrimerEditor *>(p)->accept();
                             }),
                             this);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_end(header, 12);
    gtk_widget_set_margin_top(header, 10);
    gtk_widget_set_margin_bottom(header, 4);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), cancel);
    gtk_box_append(GTK_BOX(header), m_add);

    // The inputs scroll when the panel is short; the header stays put.
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), stack);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), header);
    gtk_box_append(GTK_BOX(root), scroll);
    // Esc cancels from anywhere in the editor.
    GtkEventController *esc = gtk_event_controller_key_new();
    g_signal_connect(esc, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey *, guint key, guint, GdkModifierType,
                                                        gpointer p) -> gboolean {
                         if (key != GDK_KEY_Escape)
                             return FALSE;
                         static_cast<PrimerEditor *>(p)->cancel();
                         return TRUE;
                     }),
                     this);
    gtk_widget_add_controller(root, esc);
    return root;
}

GtkWidget *PrimerEditor::insertRow(GtkWidget *popup, GtkWidget *second, void (*insert)(GtkButton *, gpointer))
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), popup);
    if (second)
        gtk_box_append(GTK_BOX(row), second);
    GtkWidget *b = gtk_button_new_with_label("Insert");
    g_signal_connect(b, "clicked", G_CALLBACK(insert), this);
    gtk_box_append(GTK_BOX(row), b);
    return row;
}

void PrimerEditor::fillCodons()
{
    const int i = std::max(0, selectedIndex(m_aminoAcid));
    const AminoAcid &aa = aminoAcids()[size_t(i)];
    std::vector<std::string> items;
    m_codonParts.clear();
    for (size_t k = 0; k < aa.codons.size(); ++k) {
        items.push_back(k == 0 && aa.codons.size() > 1 ? aa.codons[k] + " (E. coli)" : aa.codons[k]);
        m_codonParts.push_back({PrimerPart::Kind::Codon, aa.shortName, aa.codons[k]});
    }
    setDropDownItems(m_codon, items);
}

std::string PrimerEditor::nextPrimerName() const
{
    for (int i = 1;; ++i) {
        const std::string name = "primer" + std::to_string(i);
        const auto &primers = m_doc->sequence().primers;
        if (std::none_of(primers.begin(), primers.end(), [&](const Primer &p) { return p.name == name; }))
            return name;
    }
}

void PrimerEditor::setBases(const std::string &bases)
{
    std::string clean;
    for (char c : bases)
        if (isBaseLetter(c))
            clean += c;
    m_undo.clear();
    m_redo.clear();
    m_restoring = true;
    gtk_switch_set_active(GTK_SWITCH(m_flip), FALSE);
    setText(clean, {});
    // A 5' tail is what gets added most, so start at the 5' end.
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(m_buffer, &start);
    gtk_text_buffer_place_cursor(m_buffer, &start);
    m_restoring = false;
    refresh();
}

void PrimerEditor::focus()
{
    gtk_widget_grab_focus(m_text);
}

void PrimerEditor::sequenceChanged()
{
    if (!m_finished)
        refresh(false);
}

// ---- the preview and the checks ----------------------------------------------------

std::string PrimerEditor::typed() const
{
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(m_buffer, &a, &b);
    gchar *t = gtk_text_buffer_get_text(m_buffer, &a, &b, FALSE);
    std::string out;
    for (const char *c = t; *c; ++c)
        if (!g_ascii_isspace(*c))
            out += *c;
    g_free(t);
    return out;
}

std::string PrimerEditor::primerName() const
{
    std::string name = gtk_editable_get_text(GTK_EDITABLE(m_name));
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    return name.empty() ? "primer" : name;
}

std::string PrimerEditor::hexColor() const
{
    if (!m_colorChosen)
        return {};
    const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(m_color));
    auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c->red), byte(c->green), byte(c->blue));
    return buf;
}

// Finds where the primer binds and draws it there on the document's views,
// selecting the site (which brings it into view) when `select`.
void PrimerEditor::refresh(bool select)
{
    if (m_finished)
        return;
    const std::string t = typed();
    const Sequence &seq = m_doc->sequence();
    const std::vector<PrimerSite> sites =
        t.empty() ? std::vector<PrimerSite>{} : findPrimerSites(seq, cleanBases(cleanPrimerBases(t)));
    m_site.reset();
    if (!sites.empty())
        m_site = sites[0];

    std::vector<std::string> lines, warn;
    const PrimerCheck check = t.empty() ? PrimerCheck{} : checkPrimer(t);
    if (!t.empty()) {
        std::string line = std::to_string(check.length) + " nt · GC " + fmt("%.0f", check.gc * 100) + "%";
        if (m_site) {
            // Tm of the part that anneals; a 5' tail doesn't count.
            const double tm = q5Tm(t.substr(t.size() - size_t(m_site->binding.length())));
            if (!std::isnan(tm)) {
                line += " · Q5 Tm " + fmt("%.0f", tm) + " °C";
                if (int(t.size()) > m_site->binding.length())
                    line += " annealing";
            }
        } else if (!std::isnan(q5Tm(t))) {
            line += " · Q5 Tm " + fmt("%.0f", q5Tm(t)) + " °C";
        }
        lines.push_back(line);
    }
    if (m_site) {
        const PrimerSite &s = *m_site;
        const bool reverse = s.strand == Strand::Reverse;
        lines.push_back("Binds " + view::siteText(s.binding, m_doc->sequence().length())
                        + (reverse ? " on the bottom strand ←" : " on the top strand →"));
        const int tail = int(t.size()) - s.binding.length();
        if (tail > 0)
            lines.push_back(std::to_string(tail) + "-nt 5′ tail");
        if (s.mismatches > 0)
            warn.push_back(std::to_string(s.mismatches) + (s.mismatches == 1 ? " mismatch" : " mismatches")
                           + " where it anneals.");
        if (sites.size() > 1)
            warn.push_back(otherSites(sites, s, tail));
        checkFrames(s, tail, lines, warn);
        // With a partner the selection shows the product; alone, the site.
        Span shown{s.binding.start, s.binding.length()};
        if (std::optional<Span> product = checkPartner(t, s, lines, warn))
            shown = *product;
        Primer p;
        p.name = primerName();
        p.bases = cleanPrimerBases(t);
        p.binding = s.binding;
        p.strand = s.strand;
        p.color = hexColor();
        m_doc->setPendingPrimer(p);
        if (select) {
            if (!m_doc->hasSelection() || m_doc->selection().start != shown.start
                || m_doc->selection().length != shown.length)
                m_doc->setSelection(shown);
            m_selectionShown = shown;
        }
    } else {
        m_doc->clearPendingPrimer();
        if (!t.empty())
            warn.push_back(t.size() < 15 ? "Type at least 15 bases." : "It doesn't bind this sequence.");
        else
            lines.push_back("Type or paste a primer; the map and sequence show where it binds.");
    }
    // A long primer is expected once it has a tail. The Tm warned about is
    // Q5's, as shown, and the annealing part's.
    const bool tailed = m_site && int(t.size()) > m_site->binding.length();
    for (const std::string &w : check.warnings) {
        if ((tailed && startsWith(w, "Long (")) || startsWith(w, "Tm "))
            continue;
        warn.push_back(w);
    }
    if (m_site) {
        const double tm = q5Tm(t.substr(t.size() - size_t(m_site->binding.length())));
        if (!std::isnan(tm) && (tm < 50 || tm > 72))
            warn.push_back("Q5 Tm " + fmt("%.0f", tm) + " °C; 50–72 °C is typical");
    }
    // The 3' end: its last five bases tinted, green with one to three G/C.
    if (t.size() >= 5) {
        const bool good = check.clampGC >= 1 && check.clampGC <= 3;
        lines.push_back("Last 5 bases …" + upper(t.substr(t.size() - 5)) + ": " + std::to_string(check.clampGC)
                        + " G/C" + (good ? " ✓" : ""));
        tintThreePrime(good);
    } else {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(m_buffer, &a, &b);
        gtk_text_buffer_remove_tag(m_buffer, m_tintGood, &a, &b);
        gtk_text_buffer_remove_tag(m_buffer, m_tintBad, &a, &b);
    }
    m_annealing = m_site ? m_site->binding.length() : 0;
    gtk_label_set_text(GTK_LABEL(m_info), joinLines(lines).c_str());
    gtk_widget_set_visible(m_info, !lines.empty());
    if (t.empty())
        gtk_widget_add_css_class(m_info, "dim-label");
    else
        gtk_widget_remove_css_class(m_info, "dim-label");
    gtk_label_set_text(GTK_LABEL(m_warnings), joinLines(warn).c_str());
    gtk_widget_set_visible(m_warnings, !warn.empty());
    gtk_widget_set_sensitive(m_add, m_site.has_value());
    gtk_widget_queue_draw(m_marks);
}

// Where else the primer binds, and whether it binds the other strand facing
// itself, so it can make a product on its own.
std::string PrimerEditor::otherSites(const std::vector<PrimerSite> &sites, const PrimerSite &best, int tail) const
{
    const int n = m_doc->sequence().length();
    auto place = [n](const PrimerSite &o) {
        return view::siteText(o.binding, n)
             + (o.strand == Strand::Reverse ? " ←" : " →")
             + (o.mismatches > 0 ? " (" + std::to_string(o.mismatches)
                                       + (o.mismatches == 1 ? " mismatch)" : " mismatches)")
                                 : "");
    };
    const int total = int(sites.size()) - 1;
    std::string text = "Also binds ";
    for (int i = 1; i <= std::min(3, total); ++i)
        text += (i > 1 ? ", " : "") + place(sites[size_t(i)]);
    if (total > 3)
        text += " and " + std::to_string(total - 3) + " more";
    text += "; the best site is shown.";
    for (size_t i = 1; i < sites.size(); ++i)
        if (sites[i].strand != best.strand) {
            const int product = pcrProductLength(m_doc->sequence(), best, tail, sites[i], tail);
            if (product > 0) {
                text += " It binds both strands facing itself, so on its own it can make a "
                      + view::withThousands(product) + " bp product.";
                break;
            }
        }
    return text;
}

// For each codon or tag in the tail: whether it lands in frame with the CDS
// the primer joins, and how to fix it when it doesn't.
void PrimerEditor::checkFrames(const PrimerSite &site, int tail, std::vector<std::string> &lines,
                               std::vector<std::string> &warnings) const
{
    for (const Mark &m : marks()) {
        if (m.part.kind == PrimerPart::Kind::EnzymeSite || m.start >= tail)
            continue;
        const FrameCheck f = checkPartFrame(m_doc->sequence(), site, tail, m.start, m.length, m.forward);
        if (!f.applies)
            continue;
        if (f.againstCds)
            warnings.push_back(m.part.name + " reads against " + f.cds
                               + "; turn it round with the switch before inserting.");
        else if (f.inFrame)
            lines.push_back(m.part.name + " in frame with " + f.cds + " ✓");
        else
            warnings.push_back(m.part.name + " is out of frame with " + f.cds + ": add " + std::to_string(f.fix)
                               + (f.fix == 1 ? " base" : " bases") + " between it and the annealing part.");
    }
}

// With a partner chosen: the product, the gap between the annealing Tms and
// any dimer. Returns the product's span on the template, to select.
std::optional<Span> PrimerEditor::checkPartner(const std::string &t, const PrimerSite &s,
                                               std::vector<std::string> &lines,
                                               std::vector<std::string> &warnings) const
{
    const int pick = selectedIndex(m_partner) - 1;
    const auto &primers = m_doc->sequence().primers;
    if (pick < 0 || pick >= int(m_partnerIndex.size()) || m_partnerIndex[size_t(pick)] >= int(primers.size()))
        return std::nullopt;
    const Primer &partner = primers[size_t(m_partnerIndex[size_t(pick)])];
    if (partner.binding.length() <= 0)
        return std::nullopt;
    PrimerSite theirs;
    theirs.binding = partner.binding;
    theirs.strand = partner.strand;
    const int partnerLength = partner.binding.length();
    const int product = pcrProductLength(m_doc->sequence(), s, int(t.size()) - s.binding.length(), theirs,
                                         int(partner.bases.size()) - partnerLength);
    const double a = q5Tm(t.substr(t.size() - size_t(s.binding.length())));
    const double b = q5Tm(partner.bases.substr(partner.bases.size() - size_t(partnerLength)));
    std::string line = "With " + partner.name + ": ";
    line += product > 0 ? view::withThousands(product) + " bp product" : "no product (they don't face each other)";
    if (!std::isnan(a) && !std::isnan(b)) {
        const double gap = std::abs(std::round(a) - std::round(b));
        line += " · Q5 Tm gap " + fmt("%.0f", gap) + " °C";
        if (gap > 5)
            warnings.push_back("The Tms differ by more than 5 °C; fit this one to " + fmt("%.0f", b) + " °C.");
    }
    lines.push_back(line);
    // The Tm gap above is the annealing parts'; the pair check's own is the full primers'.
    for (const std::string &w : checkPrimerPair(t, partner.bases).warnings)
        if (!startsWith(w, "Tm differs"))
            warnings.push_back(w);
    if (product <= 0)
        return std::nullopt;
    // The template stretch the product covers (tails aside).
    const bool mineForward = s.strand == Strand::Forward;
    const Range fwd = mineForward ? s.binding : partner.binding;
    const Range rev = mineForward ? partner.binding : s.binding;
    const int n = m_doc->sequence().length();
    int length = rev.end - fwd.start;
    if (length <= 0)
        length += n;
    else if (length > n)
        length -= n;   // the reverse site runs across the origin, past the forward one
    return Span{fwd.start, length};
}

// Grows or shrinks the annealing part along the template to the target Tm.
void PrimerEditor::fitToTm()
{
    if (!m_site) {
        gtk_widget_error_bell(m_root);
        return;
    }
    const std::string t = typed();
    // Only the bases after the last insert anneal here, even where an
    // insert's own bases happen to match the template: Fit never cuts into it.
    PrimerSite site = *m_site;
    int insertsEnd = 0;
    for (const Mark &m : marks())
        insertsEnd = std::max(insertsEnd, m.start + m.length);
    const int over = site.binding.length() - (int(t.size()) - insertsEnd);
    if (over > 0) {
        if (site.strand == Strand::Reverse)
            site.binding.end -= over;
        else
            site.binding.start += over;
    }
    if (site.binding.length() <= 0) {
        gtk_widget_error_bell(m_root);
        return;
    }
    PrimerSite fitted;
    const std::string result = fitAnnealingToTm(m_doc->sequence(), t, site,
                                                gtk_spin_button_get_value(GTK_SPIN_BUTTON(m_tm)), &fitted, q5Tm);
    const int len = fitted.binding.length();
    if (len <= 0 || int(result.size()) < len)
        return;
    const int old = site.binding.length();
    // Stepping the Tm fits again each step: one undo step for the run.
    if (!m_fitRun)
        noteUndo(false);
    replaceBases(int(t.size()) - old, old, result.substr(result.size() - size_t(len)));
    m_fitRun = true;
}

// ---- editing the box ---------------------------------------------------------------

// Puts a part in at the cursor, marked, as one undo step. With the switch on
// it goes in reverse complemented, so it still reads on the strand typed.
void PrimerEditor::insertPart(const PrimerPart &part)
{
    gtk_widget_grab_focus(m_text);
    const bool forward = !gtk_switch_get_active(GTK_SWITCH(m_flip));
    const std::string bases = upper(forward ? part.bases : reverseComplement(part.bases));
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(m_buffer, &a, &b);
    const int at = gtk_text_iter_get_offset(&a);
    const bool pad = part.kind == PrimerPart::Kind::EnzymeSite && at == 0
                  && gtk_check_button_get_active(GTK_CHECK_BUTTON(m_pad));
    const std::string padding = pad ? kSitePadding : "";
    noteUndo(false);
    m_restoring = true;
    gtk_text_buffer_delete(m_buffer, &a, &b);
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, at);
    gtk_text_buffer_insert(m_buffer, &a, (padding + bases).c_str(), -1);
    tagMark({at + int(padding.size()), int(bases.size()), part, forward});
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, at + int(padding.size() + bases.size()));
    gtk_text_buffer_place_cursor(m_buffer, &a);
    m_restoring = false;
    refresh();
}

// Reverse complements the bases, keeping each mark on its bases with its arrow
// turned round, and the cursor on the same base.
void PrimerEditor::flip()
{
    const std::string text = typed();
    const int n = int(text.size());
    std::vector<Mark> flipped;
    for (Mark m : marks()) {
        m.start = n - m.start - m.length;
        m.forward = !m.forward;
        flipped.push_back(m);
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(m_buffer, &a, &b);
    const int caret = gtk_text_iter_get_offset(&a), length = gtk_text_iter_get_offset(&b) - caret;
    m_restoring = true;
    setText(reverseComplement(text), flipped);
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, std::max(0, n - caret - length));
    gtk_text_buffer_get_iter_at_offset(m_buffer, &b, std::max(0, n - caret));
    gtk_text_buffer_select_range(m_buffer, &a, &b);
    m_restoring = false;
    refresh();
}

std::vector<PrimerEditor::Mark> PrimerEditor::marks() const
{
    std::vector<Mark> out;
    struct Walk { GtkTextBuffer *buffer; std::vector<Mark> *out; };
    Walk walk{m_buffer, &out};
    gtk_text_tag_table_foreach(gtk_text_buffer_get_tag_table(m_buffer), +[](GtkTextTag *tag, gpointer p) {
        auto *w = static_cast<Walk *>(p);
        auto *m = static_cast<Mark *>(g_object_get_data(G_OBJECT(tag), "oc-mark"));
        if (!m)
            return;
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(w->buffer, &it);
        // Each run of the tag is a mark (typing inside a part splits it).
        for (;;) {
            if (!gtk_text_iter_has_tag(&it, tag) && !gtk_text_iter_forward_to_tag_toggle(&it, tag))
                break;
            GtkTextIter end = it;
            gtk_text_iter_forward_to_tag_toggle(&end, tag);
            Mark mark = *m;
            mark.start = gtk_text_iter_get_offset(&it);
            mark.length = gtk_text_iter_get_offset(&end) - mark.start;
            if (mark.length > 0)
                w->out->push_back(mark);
            if (gtk_text_iter_is_end(&end))
                break;
            it = end;
        }
    }, &walk);
    std::sort(out.begin(), out.end(), [](const Mark &a, const Mark &b) { return a.start < b.start; });
    return out;
}

void PrimerEditor::tagMark(const Mark &m)
{
    GtkTextTag *tag = gtk_text_buffer_create_tag(m_buffer, nullptr, nullptr);
    g_object_set_data_full(G_OBJECT(tag), "oc-mark", new Mark(m), [](gpointer p) { delete static_cast<Mark *>(p); });
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, m.start);
    gtk_text_buffer_get_iter_at_offset(m_buffer, &b, m.start + m.length);
    gtk_text_buffer_apply_tag(m_buffer, tag, &a, &b);
}

void PrimerEditor::setText(const std::string &text, const std::vector<Mark> &marks)
{
    // Old marks' tags go with the old text.
    std::vector<GtkTextTag *> old;
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(m_buffer);
    gtk_text_tag_table_foreach(table, +[](GtkTextTag *tag, gpointer p) {
        if (g_object_get_data(G_OBJECT(tag), "oc-mark"))
            static_cast<std::vector<GtkTextTag *> *>(p)->push_back(tag);
    }, &old);
    for (GtkTextTag *tag : old)
        gtk_text_tag_table_remove(table, tag);
    const bool wasRestoring = m_restoring;
    m_restoring = true;
    gtk_text_buffer_set_text(m_buffer, text.c_str(), -1);
    for (const Mark &m : marks)
        tagMark(m);
    m_restoring = wasRestoring;
}

// Replaces some bases with unmarked ones.
void PrimerEditor::replaceBases(int start, int length, const std::string &bases)
{
    m_restoring = true;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, start);
    gtk_text_buffer_get_iter_at_offset(m_buffer, &b, start + length);
    gtk_text_buffer_delete(m_buffer, &a, &b);
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, start);
    gtk_text_buffer_insert(m_buffer, &a, bases.c_str(), -1);
    gtk_text_buffer_get_start_iter(m_buffer, &a);
    gtk_text_buffer_place_cursor(m_buffer, &a);
    m_restoring = false;
    refresh();
}

PrimerEditor::Snapshot PrimerEditor::snapshot() const
{
    GtkTextIter caret;
    gtk_text_buffer_get_iter_at_mark(m_buffer, &caret, gtk_text_buffer_get_insert(m_buffer));
    return {typed(), marks(), bool(gtk_switch_get_active(GTK_SWITCH(m_flip))), gtk_text_iter_get_offset(&caret)};
}

void PrimerEditor::restore(const Snapshot &s)
{
    m_restoring = true;
    setText(s.text, s.marks);
    gtk_switch_set_active(GTK_SWITCH(m_flip), s.flipped);
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_offset(m_buffer, &at, s.caret);
    gtk_text_buffer_place_cursor(m_buffer, &at);
    m_restoring = false;
    refresh();
}

// Keeps the box as it is before a change, for undo. Typing runs join into one
// step while the keys keep coming.
void PrimerEditor::noteUndo(bool typing)
{
    const gint64 now = g_get_monotonic_time();
    const bool joins = typing && m_lastTyping > 0 && now - m_lastTyping < 1000000;
    m_lastTyping = typing ? now : 0;
    m_fitRun = false;
    if (joins)
        return;
    m_undo.push_back(snapshot());
    m_redo.clear();
}

void PrimerEditor::undo()
{
    if (m_undo.empty()) {
        gtk_widget_error_bell(m_text);
        return;
    }
    m_redo.push_back(snapshot());
    restore(m_undo.back());
    m_undo.pop_back();
    m_lastTyping = 0;
    m_fitRun = false;
}

void PrimerEditor::redo()
{
    if (m_redo.empty()) {
        gtk_widget_error_bell(m_text);
        return;
    }
    m_undo.push_back(snapshot());
    restore(m_redo.back());
    m_redo.pop_back();
    m_lastTyping = 0;
    m_fitRun = false;
}

void PrimerEditor::tintThreePrime(bool good)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(m_buffer, &a, &b);
    gtk_text_buffer_remove_tag(m_buffer, m_tintGood, &a, &b);
    gtk_text_buffer_remove_tag(m_buffer, m_tintBad, &a, &b);
    const int n = gtk_text_buffer_get_char_count(m_buffer);
    gtk_text_buffer_get_iter_at_offset(m_buffer, &a, std::max(0, n - 5));
    gtk_text_buffer_apply_tag(m_buffer, good ? m_tintGood : m_tintBad, &a, &b);
}

// Base letters (IUPAC codes included); anything else typed or pasted, such as
// GenBank base numbers, is left out.
void PrimerEditor::insertText(GtkTextBuffer *buffer, GtkTextIter *at, const char *text, int len, gpointer p)
{
    auto *self = static_cast<PrimerEditor *>(p);
    if (self->m_restoring || self->m_filtering)
        return;
    const std::string raw(text, len < 0 ? std::strlen(text) : size_t(len));
    std::string clean;
    for (char c : raw)
        if (isBaseLetter(c))
            clean += c;
    if (!clean.empty())
        self->noteUndo(clean.size() == 1 && raw.size() == 1);
    if (clean.size() == raw.size())
        return;
    g_signal_stop_emission_by_name(buffer, "insert-text");
    if (clean.empty()) {
        gtk_widget_error_bell(self->m_text);
        return;
    }
    self->m_filtering = true;
    gtk_text_buffer_insert(buffer, at, clean.c_str(), int(clean.size()));
    self->m_filtering = false;
}

void PrimerEditor::deleteRange(GtkTextBuffer *, GtkTextIter *start, GtkTextIter *end, gpointer p)
{
    auto *self = static_cast<PrimerEditor *>(p);
    if (self->m_restoring || gtk_text_iter_equal(start, end))
        return;
    // A single base deleted (Backspace) joins the typing run.
    self->noteUndo(gtk_text_iter_get_offset(end) - gtk_text_iter_get_offset(start) == 1);
}

// Enter adds the primer; the box's own undo takes Ctrl+Z, never the document.
gboolean PrimerEditor::boxKey(GtkEventControllerKey *, guint key, guint, GdkModifierType state, gpointer p)
{
    auto *self = static_cast<PrimerEditor *>(p);
    const bool ctrl = state & GDK_CONTROL_MASK, shift = state & GDK_SHIFT_MASK;
    if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !ctrl) {
        self->accept();
        return TRUE;
    }
    if (ctrl && (key == GDK_KEY_z || key == GDK_KEY_Z)) {
        if (shift)
            self->redo();
        else
            self->undo();
        return TRUE;
    }
    if (ctrl && (key == GDK_KEY_y || key == GDK_KEY_Y)) {
        self->redo();
        return TRUE;
    }
    return FALSE;
}

// A line under each marked part: its colour says what kind it is, its arrow
// which way it reads, and its name sits under it. The annealing part gets a
// grey one, named at its 3' end, drawn first so a part's line shows over it.
void PrimerEditor::drawMarks(GtkDrawingArea *, cairo_t *cr, int, int, gpointer p)
{
    auto *self = static_cast<PrimerEditor *>(p);
    GtkTextView *view = GTK_TEXT_VIEW(self->m_text);
    const int n = gtk_text_buffer_get_char_count(self->m_buffer);
    GdkRGBA fg;
    gtk_widget_get_color(self->m_text, &fg);
    PangoLayout *label = gtk_widget_create_pango_layout(self->m_marks, nullptr);
    PangoFontDescription *font = pango_font_description_from_string("Sans Bold 7");
    pango_layout_set_font_description(label, font);
    pango_font_description_free(font);

    auto draw = [&](int start, int length, GdkRGBA color, const std::string &name, bool forward, bool nameAtEnd) {
        if (length <= 0 || start < 0 || start + length > n)
            return;
        // The characters' boxes, grouped by display line.
        struct Run { double x0, x1, y; bool first, last; };
        std::vector<Run> runs;
        for (int i = start; i < start + length; ++i) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_offset(self->m_buffer, &it, i);
            GdkRectangle r;
            gtk_text_view_get_iter_location(view, &it, &r);
            int x, y;
            gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &x, &y);
            const double bottom = y + r.height;
            if (runs.empty() || std::abs(runs.back().y - bottom) > 1)
                runs.push_back({double(x), double(x + r.width), bottom, runs.empty(), false});
            else
                runs.back().x1 = x + r.width;
        }
        if (runs.empty())
            return;
        runs.back().last = true;
        gdk_cairo_set_source_rgba(cr, &color);
        for (const Run &run : runs) {
            const double y = run.y + 3, x0 = run.x0 + 1, x1 = run.x1 - 1;
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, x0, y);
            cairo_line_to(cr, x1, y);
            cairo_stroke(cr);
            // The arrowhead where the part ends, the way it reads.
            if (forward ? run.last : run.first) {
                const double tip = forward ? x1 : x0, back = forward ? -5 : 5;
                cairo_move_to(cr, tip, y);
                cairo_line_to(cr, tip + back, y - 3);
                cairo_line_to(cr, tip + back, y + 3);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
            if (nameAtEnd ? run.last : run.first) {
                pango_layout_set_text(label, name.c_str(), -1);
                int lw = 0, lh = 0;
                pango_layout_get_pixel_size(label, &lw, &lh);
                cairo_move_to(cr, nameAtEnd ? std::max(x0, x1 - lw) : x0, y + 2);
                pango_cairo_show_layout(cr, label);
            }
        }
    };
    if (self->m_annealing > 0) {
        GdkRGBA grey = fg;
        grey.alpha = 0.35f;
        draw(n - self->m_annealing, self->m_annealing, grey, "anneals", true, true);
    }
    for (const Mark &m : self->marks())
        draw(m.start, m.length, partColor(m.part.kind), m.part.name, m.forward, false);
    g_object_unref(label);
}

// ---- finishing ---------------------------------------------------------------------

void PrimerEditor::accept()
{
    if (m_finished)
        return;
    if (!m_site) {
        gtk_widget_error_bell(m_root);
        return;
    }
    m_finished = true;
    m_doc->clearPendingPrimer();
    std::string description = gtk_editable_get_text(GTK_EDITABLE(m_description));
    description.erase(0, description.find_first_not_of(" \t\n"));
    description.erase(description.find_last_not_of(" \t\n") + 1);
    Primer p;
    p.name = primerName();
    p.bases = cleanPrimerBases(typed());
    p.binding = m_site->binding;
    p.strand = m_site->strand;
    p.color = hexColor();
    p.description = description;
    m_doc->addPrimers({p}, "Add Primer");
    m_doc->setSelection({p.binding.start, p.binding.length()});
    close();
}

void PrimerEditor::cancel()
{
    if (m_finished)
        return;
    m_finished = true;
    m_doc->clearPendingPrimer();
    // Put back the selection the preview moved, unless it was changed since.
    if (m_selectionShown && m_doc->hasSelection() && m_doc->selection().start == m_selectionShown->start
        && m_doc->selection().length == m_selectionShown->length) {
        if (m_hadSelection && m_selectionBefore.length > 0)
            m_doc->setSelection(m_selectionBefore);
        else
            m_doc->clearSelection();
    }
    close();
}

void PrimerEditor::close()
{
    if (onClose)
        onClose();
}

} // namespace oc::gtk
