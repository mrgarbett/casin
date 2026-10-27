// SPDX-License-Identifier: GPL-3.0-or-later
// Digest and Gel: pick enzymes, see the fragments and a virtual gel.
#include "window.h"

#include "core/digest.h"
#include "view/cairo_render.h"
#include "view/gellayout.h"
#include "view/maplayout.h"   // formatBp, withThousands

#include <algorithm>
#include <set>

namespace oc::gtk {

namespace {

struct DigestState {
    Sequence seq;                      // a copy: the dialog doesn't follow later edits
    std::vector<Enzyme> enzymes;       // the ones that cut, sorted by name
    std::set<std::string> chosen;
    int ladder = 0;
    DigestResult result;
    GtkWidget *gel = nullptr;
    GtkWidget *fragments = nullptr;
    GtkWidget *summary = nullptr;
    bool dark = false;
};

std::string endText(const std::string &enzyme, int overhang)
{
    if (enzyme.empty())
        return "end";
    if (overhang == 0)
        return enzyme + " (blunt)";
    return enzyme + (overhang > 0 ? " (5′ " : " (3′ ") + std::to_string(std::abs(overhang)) + ")";
}

void recompute(DigestState *st)
{
    std::vector<Enzyme> use;
    for (const Enzyme &e : st->enzymes)
        if (st->chosen.count(e.name))
            use.push_back(e);
    st->result = digest(st->seq, use);

    std::string names;
    for (const Enzyme &e : use)
        names += (names.empty() ? "" : " + ") + e.name;
    std::string summary;
    if (use.empty())
        summary = "Pick one or more enzymes";
    else if (st->result.uncut)
        summary = names + " doesn't cut";
    else
        summary = names + ": " + std::to_string(st->result.fragments.size())
                + (st->result.fragments.size() == 1 ? " fragment" : " fragments");
    gtk_label_set_text(GTK_LABEL(st->summary), summary.c_str());

    gtk_list_box_remove_all(GTK_LIST_BOX(st->fragments));
    if (!use.empty() && !st->result.uncut) {
        std::vector<DigestFragment> sorted = st->result.fragments;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const DigestFragment &a, const DigestFragment &b) { return a.length > b.length; });
        const int n = st->seq.length();
        for (const DigestFragment &f : sorted) {
            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), view::formatBp(f.length).c_str());
            int end = f.start + f.length;
            if (end > n)
                end -= n;
            const std::string sub = view::withThousands(f.start + 1) + "–" + view::withThousands(end) + " · "
                                  + endText(f.leftEnzyme, f.leftOverhang) + " to "
                                  + endText(f.rightEnzyme, f.rightOverhang);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub.c_str());
            gtk_list_box_append(GTK_LIST_BOX(st->fragments), row);
        }
    }
    gtk_widget_queue_draw(st->gel);
}

void drawGel(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data)
{
    auto *st = static_cast<DigestState *>(data);
    const Ladder &ladder = commonLadders()[size_t(st->ladder)];
    std::vector<view::GelLane> lanes;
    lanes.push_back({"Ladder", ladder.bands, ladder.bright, true, false});
    lanes.push_back({"Uncut", {st->seq.length()}, {}, false, st->seq.circular});
    view::GelLane cut{"Digest", {}, {}, false, false};
    if (!st->chosen.empty()) {
        if (st->result.uncut)
            cut.uncutCircular = st->seq.circular;
        for (const DigestFragment &f : st->result.fragments)
            cut.bands.push_back(f.length);
    }
    lanes.push_back(cut);
    view::PangoTextMeasurer tm;
    view::GelLayout layout(lanes, st->dark ? view::Theme::darkTheme() : view::Theme::light(), tm, w, h);
    view::drawScene(cr, layout.render());
}

} // namespace

void Window::digestDialog()
{
    if (m_doc.isEmpty())
        return;
    auto *st = new DigestState;
    st->seq = m_doc.sequence();
    st->dark = adw_style_manager_get_dark(adw_style_manager_get_default());
    for (const Enzyme &e : m_doc.enzymes())
        if (m_doc.cutCount(e.name) > 0)
            st->enzymes.push_back(e);
    std::sort(st->enzymes.begin(), st->enzymes.end(),
              [](const Enzyme &a, const Enzyme &b) { return toLower(a.name) < toLower(b.name); });
    // Start from the enzyme picked on the map or in the list, if any.
    const int active = m_doc.activeSite();
    if (active >= 0 && active < int(m_doc.visibleSites().size()))
        st->chosen.insert(m_doc.visibleSites()[size_t(active)].enzyme->name);

    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Digest and Gel");
    adw_dialog_set_content_width(dialog, 860);
    adw_dialog_set_content_height(dialog, 600);
    GtkWidget *header = adw_header_bar_new();

    // Left: ladder and enzymes.
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(left, 230, -1);
    gtk_widget_set_margin_start(left, 12);
    gtk_widget_set_margin_bottom(left, 12);
    std::vector<const char *> ladderNames;
    for (const Ladder &l : commonLadders())
        ladderNames.push_back(l.name.c_str());
    ladderNames.push_back(nullptr);
    GtkWidget *ladder = gtk_drop_down_new_from_strings(ladderNames.data());
    gtk_widget_set_tooltip_text(ladder, "Size standard");
    g_signal_connect(ladder, "notify::selected", G_CALLBACK((+[](GObject *dd, GParamSpec *, gpointer p) {
                         auto *st = static_cast<DigestState *>(p);
                         st->ladder = int(gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)));
                         gtk_widget_queue_draw(st->gel);
                     })),
                     st);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    for (const Enzyme &e : st->enzymes) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), e.name.c_str());
        const int cuts = m_doc.cutCount(e.name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    (e.site + " · " + std::to_string(cuts) + (cuts == 1 ? " cut" : " cuts")).c_str());
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), st->chosen.count(e.name) > 0);
        g_object_set_data_full(G_OBJECT(check), "enzyme", g_strdup(e.name.c_str()), g_free);
        g_signal_connect(check, "toggled", G_CALLBACK((+[](GtkCheckButton *b, gpointer p) {
                             auto *st = static_cast<DigestState *>(p);
                             const std::string name = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "enzyme"));
                             if (gtk_check_button_get_active(b))
                                 st->chosen.insert(name);
                             else
                                 st->chosen.erase(name);
                             recompute(st);
                         })),
                         st);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
        gtk_list_box_append(GTK_LIST_BOX(list), row);
    }
    GtkWidget *lscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lscroll), list);
    gtk_widget_set_vexpand(lscroll, TRUE);
    gtk_box_append(GTK_BOX(left), ladder);
    gtk_box_append(GTK_BOX(left), lscroll);

    // Right: summary, gel and fragments.
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(right, TRUE);
    gtk_widget_set_margin_end(right, 12);
    gtk_widget_set_margin_bottom(right, 12);
    st->summary = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(st->summary), 0);
    gtk_widget_add_css_class(st->summary, "heading");
    st->gel = gtk_drawing_area_new();
    gtk_widget_set_size_request(st->gel, 320, 300);
    gtk_widget_set_vexpand(st->gel, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->gel), drawGel, st, nullptr);
    st->fragments = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(st->fragments), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(st->fragments, "boxed-list");
    GtkWidget *fscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fscroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fscroll), st->fragments);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(fscroll), 150);
    gtk_box_append(GTK_BOX(right), st->summary);
    gtk_box_append(GTK_BOX(right), st->gel);
    gtk_box_append(GTK_BOX(right), fscroll);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(body), left);
    gtk_box_append(GTK_BOX(body), right);
    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), body);
    adw_dialog_set_child(dialog, tv);
    g_object_set_data_full(G_OBJECT(dialog), "state", st, [](gpointer p) { delete static_cast<DigestState *>(p); });
    recompute(st);
    adw_dialog_present(dialog, m_window);
}

} // namespace oc::gtk
