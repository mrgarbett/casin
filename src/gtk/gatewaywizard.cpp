// SPDX-License-Identifier: GPL-3.0-or-later
#include "gatewaywizard.h"
#include "window.h"

#include "core/edit.h"
#include "view/maplayout.h"

#include <algorithm>
#include <cctype>

namespace oc::gtk {

namespace {

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

// "att1 → 231–245", with the arrow saying which strand the core reads on.
std::string siteLine(const AttSite &site, int n)
{
    return "att" + std::to_string(site.number) + "  " + (site.reversed ? "← " : "→ ")
           + view::siteText(Range{site.span.start, site.span.end()}, n);
}

// What one molecule's sites come to: the numbers it crosses at, or why it
// can't be used.
std::string sitesOf(const Sequence &seq, const std::string &what)
{
    const std::vector<AttSite> sites = findAttSites(seq);
    if (sites.empty())
        return what + ": no att sites";
    std::string out = what + ":";
    for (const AttSite &site : sites)
        out += "\n    " + siteLine(site, seq.length());
    return out;
}

// Whether a feature named att... already covers base `pos`.
bool annotated(const Sequence &seq, int pos)
{
    for (const Feature &f : seq.features) {
        if (f.name.rfind("att", 0) != 0 && f.name.rfind("ATT", 0) != 0)
            continue;
        for (const Range &r : f.ranges)
            if (r.contains(pos))
                return true;
    }
    return false;
}

// A copy of `seq` with its att cores marked, so the maps here show where the
// strands will cross; a site already annotated is left to its own label. The
// tab's own sequence is left as it is.
Sequence withSites(const Sequence &seq)
{
    Sequence out = seq;
    const int n = seq.length();
    for (const AttSite &site : findAttSites(seq)) {
        if (annotated(seq, (site.span.start + site.span.length / 2) % n))
            continue;
        Feature f;
        f.type = "misc_recomb";
        f.name = "att" + std::to_string(site.number) + " core";
        if (site.span.end() > n)
            f.ranges = {{site.span.start, n}, {0, site.span.end() - n}};
        else
            f.ranges = {{site.span.start, site.span.end()}};
        f.strand = site.reversed ? Strand::Reverse : Strand::Forward;
        out.features.push_back(f);
    }
    return out;
}

} // namespace

GatewayWizard::GatewayWizard(Document *insert, GtkWidget *parent, const std::string &name,
                             std::function<std::vector<GatewaySource>()> sources)
    : m_insert(insert), m_sources(std::move(sources)), m_parent(parent)
{
    m_insertView.setSequence(withSites(insert->sequence()));
    m_insertMap = std::make_unique<MapView>(&m_insertView);
    m_vectorMap = std::make_unique<MapView>(&m_vector);
    m_map = std::make_unique<MapView>(&m_preview);
    m_seq = std::make_unique<SequenceView>(&m_preview);
    m_dialog = GTK_WIDGET(adw_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(m_dialog), ("Gateway · " + name).c_str());
    adw_dialog_set_child(ADW_DIALOG(m_dialog), buildView());
    adw_dialog_set_default_widget(ADW_DIALOG(m_dialog), m_accept);
    const int w = parent ? gtk_widget_get_width(parent) : 1300, h = parent ? gtk_widget_get_height(parent) : 820;
    adw_dialog_set_content_width(ADW_DIALOG(m_dialog), std::max(780, int(w * 0.8)));
    adw_dialog_set_content_height(ADW_DIALOG(m_dialog), std::max(500, int(h * 0.8)));
    g_signal_connect_swapped(m_dialog, "destroy", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GatewayWizard *>(p);
                                 self->m_closing = true;
                                 disconnectTree(self->m_dialog, self);
                                 if (self->onClosed)
                                     self->onClosed();
                                 g_idle_add_once([](gpointer q) { delete static_cast<GatewayWizard *>(q); }, self);
                             }),
                             this);
    // An entry clone is a circle, an attB fragment usually isn't: a good
    // first guess at which reaction this is, until the reaction is picked.
    if (m_insert->sequence().circular)
        adw_combo_row_set_selected(ADW_COMBO_ROW(m_reaction), 1);
    fillVectors();
    refresh();
}

GatewayWizard::~GatewayWizard() = default;

void GatewayWizard::present()
{
    if (gtk_widget_get_parent(m_dialog))
        gtk_widget_grab_focus(m_dialog);
    else
        adw_dialog_present(ADW_DIALOG(m_dialog), m_parent);
}

void GatewayWizard::close()
{
    adw_dialog_force_close(ADW_DIALOG(m_dialog));
}

void GatewayWizard::templateChanged()
{
    if (m_closing)
        return;
    fillVectors();
    refresh();
}

GtkWidget *GatewayWizard::buildView()
{
    view::MapOptions vectorOptions = m_vectorMap->options();
    vectorOptions.enzymes = false;
    m_vectorMap->setOptions(vectorOptions);
    m_insertMap->setOptions(vectorOptions);

    m_split = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(m_split), m_map->widget());
    gtk_paned_set_end_child(GTK_PANED(m_split), m_seq->widget());
    gtk_paned_set_shrink_start_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(m_split), FALSE);
    gtk_paned_set_position(GTK_PANED(m_split), 330);
    const bool dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    m_insertMap->setDark(dark);
    m_vectorMap->setDark(dark);
    m_map->setDark(dark);
    m_seq->setDark(dark);

    m_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(m_stack, TRUE);
    gtk_stack_add_titled(GTK_STACK(m_stack), m_insertMap->widget(), "insert", "Insert");
    gtk_stack_add_titled(GTK_STACK(m_stack), m_vectorMap->widget(), "vector", "Vector");
    gtk_stack_add_titled(GTK_STACK(m_stack), m_split, "clone", "Clone");
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
                                 static_cast<GatewayWizard *>(p)->close();
                             }),
                             this);
    m_accept = gtk_button_new_with_label("Open Clone");
    gtk_widget_add_css_class(m_accept, "suggested-action");
    gtk_widget_set_tooltip_text(m_accept, "Open the clone as a new, unsaved sequence");
    g_signal_connect_swapped(m_accept, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GatewayWizard *>(p)->accept();
                             }),
                             this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_accept);

    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), body);
    return view;
}

GtkWidget *GatewayWizard::buildSidebar()
{
    auto changed = +[](gpointer p) {
        auto *self = static_cast<GatewayWizard *>(p);
        if (!self->m_filling)
            self->refresh();
    };

    m_cue = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(m_cue, "restricto-cue");
    m_cueIcon = gtk_image_new();
    gtk_widget_set_valign(m_cueIcon, GTK_ALIGN_START);
    m_cueText = label(nullptr);
    gtk_widget_set_hexpand(m_cueText, TRUE);
    gtk_box_append(GTK_BOX(m_cue), m_cueIcon);
    gtk_box_append(GTK_BOX(m_cue), m_cueText);

    // The reaction, and what this tab recombines with. Which reaction it is
    // can't be read off the sequence: both families share the same cores, so
    // the choice only decides what the new sites are called.
    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Reaction");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          "This tab is the insert. What lies between its two att sites moves "
                                          "into the vector. BP joins attB to attP; LR joins attL to attR.");
    m_reaction = adw_combo_row_new();
    showWholeNames(ADW_COMBO_ROW(m_reaction));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_reaction), "Reaction");
    setModel(m_reaction, {"BP → entry clone", "LR → expression clone"});
    g_signal_connect_swapped(m_reaction, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GatewayWizard *>(p);
                                 if (self->m_filling)
                                     return;
                                 self->m_chosenReaction = true;
                                 self->refresh();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), m_reaction);
    m_vectorRow = adw_combo_row_new();
    showWholeNames(ADW_COMBO_ROW(m_vectorRow));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_vectorRow), "Vector");
    adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(m_vectorRow), FALSE);
    g_signal_connect_swapped(m_vectorRow, "notify::selected", G_CALLBACK(changed), this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), m_vectorRow);
    // An insert whose sites face the other way only fits read from its
    // other strand. It's turned over by itself when that's what it needs;
    // the switch shows it, and lets the other way be tried by hand.
    m_flip = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_flip), "Flip the insert");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(m_flip), "Read it from its other strand, so its sites face the vector's");
    g_signal_connect_swapped(m_flip, "notify::active", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GatewayWizard *>(p);
                                 if (self->m_filling)
                                     return;
                                 self->m_chosenFlip = true;
                                 self->refresh();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), m_flip);
    m_alsoByproduct = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_alsoByproduct), "Also open the byproduct");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(m_alsoByproduct),
                                "The other circle; only a circular insert leaves one");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), m_alsoByproduct);

    // What was found, so a plasmid with no sites says so plainly.
    m_sitesGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_sitesGroup), "att Sites");
    m_sites = label("caption");
    gtk_label_set_selectable(GTK_LABEL(m_sites), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_sitesGroup), m_sites);

    m_resultGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_resultGroup), "Clone");
    GtkWidget *result = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    m_size = label("title-1", false);
    gtk_widget_add_css_class(m_size, "numeric");
    m_info = label("caption");
    gtk_label_set_selectable(GTK_LABEL(m_info), TRUE);
    gtk_box_append(GTK_BOX(result), m_size);
    gtk_box_append(GTK_BOX(result), m_info);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_resultGroup), result);
    m_warnings = label("caption");
    gtk_widget_add_css_class(m_warnings, "warning");

    GtkWidget *inputs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(inputs, 16);
    gtk_widget_set_margin_end(inputs, 16);
    gtk_widget_set_margin_top(inputs, 14);
    gtk_widget_set_margin_bottom(inputs, 18);
    for (GtkWidget *w : {m_cue, group, m_sitesGroup, m_resultGroup, m_warnings})
        gtk_box_append(GTK_BOX(inputs), w);
    GtkWidget *sidebar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar), inputs);
    gtk_widget_set_size_request(sidebar, 340, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);
    gtk_widget_add_css_class(sidebar, "restricto-wizard-sidebar");
    return sidebar;
}

// Every other open sequence, after a "Choose" entry, keeping what was picked
// when it's still open. A lone candidate is picked at once.
void GatewayWizard::fillVectors()
{
    Document *keep = vectorDoc();
    m_filling = true;
    m_vectors.clear();
    std::vector<std::string> items{"Choose…"};
    for (const GatewaySource &s : m_sources ? m_sources() : std::vector<GatewaySource>{}) {
        if (!s.doc || s.doc == m_insert || s.doc->isEmpty())
            continue;
        m_vectors.push_back(s.doc);
        items.push_back(s.name);
    }
    setModel(m_vectorRow, items);
    gtk_widget_set_sensitive(m_vectorRow, !m_vectors.empty());
    guint select = m_vectors.size() == 1 ? 1 : 0;
    for (size_t i = 0; i < m_vectors.size(); ++i)
        if (m_vectors[i] == keep)
            select = guint(i + 1);
    adw_combo_row_set_selected(ADW_COMBO_ROW(m_vectorRow), select);
    m_filling = false;
}

// The picked vector, while its tab is still open: a tab in another window
// can close while this dialog is up.
Document *GatewayWizard::vectorDoc() const
{
    const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(m_vectorRow));
    Document *doc = i >= 1 && i <= m_vectors.size() ? m_vectors[i - 1] : nullptr;
    if (!doc || !m_sources)
        return nullptr;
    for (const GatewaySource &s : m_sources())
        if (s.doc == doc)
            return doc;
    return nullptr;
}

GatewayReaction GatewayWizard::reaction() const
{
    return adw_combo_row_get_selected(ADW_COMBO_ROW(m_reaction)) == 0 ? GatewayReaction::BP : GatewayReaction::LR;
}

void GatewayWizard::setCue(const std::string &text, const char *icon, const char *css)
{
    gtk_label_set_text(GTK_LABEL(m_cueText), text.c_str());
    gtk_image_set_from_icon_name(GTK_IMAGE(m_cueIcon), icon);
    for (const char *c : {"ready", "problem"})
        gtk_widget_remove_css_class(m_cue, c);
    if (css)
        gtk_widget_add_css_class(m_cue, css);
}

// Runs the reaction on the picked pair, draws the clone, and fills in what
// was found and what came out.
void GatewayWizard::refresh()
{
    if (m_closing)
        return;
    const Sequence &original = m_insert->sequence();
    Document *vec = vectorDoc();
    // The reaction is only guessed while it hasn't been picked by hand.
    if (!m_chosenReaction) {
        m_filling = true;
        adw_combo_row_set_selected(ADW_COMBO_ROW(m_reaction), original.circular ? 1 : 0);
        m_filling = false;
    }
    // Likewise the flip: on when the insert only fits the other way round.
    if (!m_chosenFlip) {
        GatewayResult probe;
        const bool flip = vec && gatewayRecombine(original, vec->sequence(), reaction(), &probe) && probe.insertFlipped;
        m_filling = true;
        adw_switch_row_set_active(ADW_SWITCH_ROW(m_flip), flip);
        m_filling = false;
    }
    const bool flip = adw_switch_row_get_active(ADW_SWITCH_ROW(m_flip));
    Sequence flipped;
    if (flip) {
        flipped = original;
        reverseComplementSequence(flipped);
    }
    // The insert as it goes in, which is what the Insert view shows.
    const Sequence &insert = flip ? flipped : original;
    m_insertView.setSequence(withSites(insert));
    const std::string insertWhat = reaction() == GatewayReaction::BP ? "attB fragment" : "entry clone";
    const std::string vectorWhat = reaction() == GatewayReaction::BP ? "donor vector" : "destination vector";

    std::vector<std::string> found{sitesOf(insert, "This tab (" + insertWhat + (flip ? ", flipped)" : ")"))};
    if (vec) {
        m_vector.setSequence(withSites(vec->sequence()));
        found.push_back(sitesOf(vec->sequence(), vec->sequence().name + " (" + vectorWhat + ")"));
    }
    gtk_label_set_text(GTK_LABEL(m_sites), joinLines(found).c_str());

    std::vector<std::string> lines, warn;
    const bool hadClone = m_hasClone;
    m_byproduct = Sequence{};
    m_hasClone = false;
    std::string err;
    bool wrongWay = false;   // fits, but only with the flip the other way
    if (vec) {
        GatewayResult result;
        const bool ok = gatewayRecombine(insert, vec->sequence(), reaction(), &result, &err);
        // The core turns a backwards insert over by itself; here the switch
        // decides, so an insert that only fits the other way is refused.
        wrongWay = ok && result.insertFlipped;
        if (ok && !wrongWay) {
            m_clone = result.clone;
            m_byproduct = result.byproduct;
            m_preview.setSequence(m_clone);
            m_preview.markUnsaved();
            m_hasClone = true;
            lines.push_back("att" + std::to_string(result.firstSite) + " × att"
                            + std::to_string(result.secondSite) + " in both molecules");
            lines.push_back("Carries the arc between this tab's sites, in " + m_vector.sequence().name
                            + "'s backbone");
            if (!m_byproduct.bases.empty())
                lines.push_back("Byproduct: " + view::withThousands(m_byproduct.length()) + " bp");
            if (flip)
                lines.push_back("The insert went in flipped, read from its other strand");
        } else if (!wrongWay) {
            warn.push_back(capitalised(err.empty() ? "these two don't recombine" : err) + ".");
        }
    }

    if (m_hasClone) {
        gtk_label_set_text(GTK_LABEL(m_size), (view::withThousands(m_clone.length()) + " bp").c_str());
        setCue("Ready: a " + view::withThousands(m_clone.length()) + " bp "
                   + (reaction() == GatewayReaction::BP ? "entry clone" : "expression clone")
                   + ". See the Clone tab.",
               "object-select-symbolic", "ready");
    } else if (wrongWay) {
        setCue(std::string("The insert's sites face the other way. Turn Flip the insert ") + (flip ? "off" : "on")
                   + " to fit the vector.",
               "dialog-warning-symbolic", "problem");
    } else if (findAttSites(insert).size() != 2) {
        setCue("This tab needs two att sites of different numbers facing each other. None were found, or too many.",
               "dialog-warning-symbolic", "problem");
    } else if (!vec) {
        setCue("Open the " + vectorWhat + " in another tab, then pick it below.", "dialog-information-symbolic",
               nullptr);
    } else {
        setCue("No clone from these two.", "dialog-warning-symbolic", "problem");
    }

    gtk_stack_page_set_visible(gtk_stack_get_page(GTK_STACK(m_stack), m_split), m_hasClone);
    gtk_stack_page_set_visible(gtk_stack_get_page(GTK_STACK(m_stack), m_vectorMap->widget()), vec != nullptr);
    // A clone that has just appeared is what to look at; without one, the
    // vector (or the insert, before there is one) shows where the sites are.
    if (m_hasClone && !hadClone)
        gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "clone");
    else if (!m_hasClone)
        gtk_stack_set_visible_child_name(GTK_STACK(m_stack), vec ? "vector" : "insert");
    // Only a circular insert leaves a second circle to open.
    gtk_widget_set_sensitive(m_alsoByproduct, m_hasClone && !m_byproduct.bases.empty());
    gtk_label_set_text(GTK_LABEL(m_info), joinLines(lines).c_str());
    gtk_widget_set_visible(m_resultGroup, m_hasClone);
    gtk_label_set_text(GTK_LABEL(m_warnings), joinLines(warn).c_str());
    gtk_widget_set_visible(m_warnings, !warn.empty());
    gtk_widget_set_sensitive(m_accept, m_hasClone);
}

void GatewayWizard::accept()
{
    if (m_closing)
        return;
    if (!m_hasClone) {
        gtk_widget_error_bell(m_dialog);
        return;
    }
    const Sequence clone = m_clone;
    const Sequence by = m_byproduct;
    const bool alsoBy = adw_switch_row_get_active(ADW_SWITCH_ROW(m_alsoByproduct)) && !by.bases.empty();
    auto open = onOpenProduct;
    close();
    if (!open)
        return;
    // The byproduct first, so the clone is the tab left in front.
    if (alsoBy)
        open(by);
    open(clone);
}

} // namespace oc::gtk
