// SPDX-License-Identifier: GPL-3.0-or-later
// Choose Enzymes: find enzymes by where they cut.
#include "window.h"

#include "core/digest.h"
#include "view/maplayout.h"   // formatBp, withThousands

#include <algorithm>
#include <set>

namespace oc::gtk {

namespace {
struct ChooseState {
    Window *window;
    Sequence seq;
    std::vector<CutSite> sites;
    bool hasSelection = false;
    Span selection;
    std::vector<Feature> features;
    GtkWidget *cuts, *where, *spare, *ends, *result, *okButton;
    AdwDialog *dialog;
    std::vector<std::string> found;
    int maxCuts = 1;
};

GtkWidget *comboRowOf(GtkWidget *group, const char *title, const std::vector<std::string> &items)
{
    std::vector<const char *> v;
    for (const std::string &s : items)
        v.push_back(s.c_str());
    v.push_back(nullptr);
    GtkWidget *row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    GtkStringList *list = gtk_string_list_new(v.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(list));
    g_object_unref(list);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}

void updateChoose(ChooseState *st)
{
    static const int maxCuts[] = {1, 2, 3};
    EnzymeCriteria c;
    st->maxCuts = maxCuts[std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(st->cuts)), 2)];
    c.maxCuts = st->maxCuts;
    c.ends = EnzymeCriteria::Ends(std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(st->ends)), 3));
    if (st->hasSelection && adw_combo_row_get_selected(ADW_COMBO_ROW(st->where)) == 1) {
        c.inside = true;
        c.region = st->selection;
    }
    const guint spare = adw_combo_row_get_selected(ADW_COMBO_ROW(st->spare));
    if (spare > 0 && spare <= st->features.size())
        for (const Range &r : st->features[spare - 1].ranges)
            c.spare.push_back({r.start, r.length()});
    st->found = chooseEnzymes(st->seq, commonEnzymes(), st->sites, c);
    std::string text;
    for (const std::string &n : st->found)
        text += (text.empty() ? "" : ", ") + n;
    const std::string title = st->found.empty() ? "No enzyme matches"
                                                : std::to_string(st->found.size())
                                                      + (st->found.size() == 1 ? " enzyme" : " enzymes");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->result), title.c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->result), text.c_str());
    gtk_widget_set_sensitive(st->okButton, !st->found.empty());
}
} // namespace

void Window::chooseEnzymesDialog()
{
    if (m_doc.isEmpty())
        return;
    auto *st = new ChooseState{};
    st->window = this;
    st->seq = m_doc.sequence();
    st->sites = findSites(st->seq, commonEnzymes());
    st->hasSelection = m_doc.hasSelection() && m_doc.selection().length > 0;
    st->selection = m_doc.selection();
    for (const Feature &f : st->seq.features)
        if (f.type != "source")
            st->features.push_back(f);

    AdwDialog *dialog = adw_dialog_new();
    st->dialog = dialog;
    adw_dialog_set_title(dialog, "Choose Enzymes");
    adw_dialog_set_content_width(dialog, 440);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), dialog);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    st->okButton = gtk_button_new_with_label("Show These");
    gtk_widget_add_css_class(st->okButton, "suggested-action");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), st->okButton);
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *g = adw_preferences_group_new();
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));
    st->cuts = comboRowOf(g, "Cuts", {"Once", "Once or twice", "Up to three times"});
    std::vector<std::string> where{"Anywhere"};
    if (st->hasSelection)
        where.push_back("Only inside the selection");
    st->where = comboRowOf(g, "Where", where);
    if (st->hasSelection)
        adw_combo_row_set_selected(ADW_COMBO_ROW(st->where), 1);
    std::vector<std::string> spare{"Nothing"};
    for (const Feature &f : st->features)
        spare.push_back(f.name.empty() ? f.type : f.name);
    st->spare = comboRowOf(g, "Never cut inside", spare);
    st->ends = comboRowOf(g, "Ends", {"Any", "5′ overhang", "3′ overhang", "Blunt"});
    GtkWidget *gr = adw_preferences_group_new();
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(gr));
    st->result = adw_action_row_new();
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(st->result), TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gr), st->result);
    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), page);
    adw_dialog_set_child(dialog, tv);

    auto changed = +[](GObject *, GParamSpec *, gpointer p) { updateChoose(static_cast<ChooseState *>(p)); };
    for (GtkWidget *w : {st->cuts, st->where, st->spare, st->ends})
        g_signal_connect(w, "notify::selected", G_CALLBACK(changed), st);
    updateChoose(st);
    g_signal_connect(st->okButton, "clicked", G_CALLBACK((+[](GtkButton *, gpointer p) {
                         auto *st = static_cast<ChooseState *>(p);
                         st->window->showOnlyEnzymes(std::set<std::string>(st->found.begin(), st->found.end()),
                                                     st->maxCuts);
                         adw_dialog_close(st->dialog);
                     })),
                     st);
    g_object_set_data_full(G_OBJECT(dialog), "state", st, [](gpointer p) { delete static_cast<ChooseState *>(p); });
    adw_dialog_present(dialog, m_window);
}

} // namespace oc::gtk
