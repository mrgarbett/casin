// SPDX-License-Identifier: GPL-3.0-or-later
#include "gelwizard.h"

#include "core/digest.h"
#include "core/enzymes.h"
#include "view/gellayout.h"
#include "view/maplayout.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace oc::gtk {

namespace {

const int kMaxLanes = 10;
// Kept while the app runs: the gel opens as it was last left.
int g_ladder = 0;
GelStain g_stain = GelStain::UV;

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string joined(const std::vector<std::string> &v, const char *sep)
{
    std::string out;
    for (const std::string &s : v)
        out += (out.empty() ? "" : sep) + s;
    return out;
}


std::vector<Enzyme> named(const std::vector<std::string> &names)
{
    std::vector<Enzyme> out;
    for (const std::string &n : names)
        if (const Enzyme *e = findEnzyme(n))
            out.push_back(*e);
    return out;
}

} // namespace

std::string GelWizard::Lane::key() const
{
    return std::to_string(reinterpret_cast<uintptr_t>(doc)) + "|" + std::to_string(seq.length()) + "|"
         + joined(enzymes, ",");
}

GelWizard::GelWizard(Document *templ, GtkWidget *parent, const std::string &name,
                     std::function<std::vector<GelSource>()> sources)
    : m_template(templ), m_sources(std::move(sources)), m_parent(parent)
{
    const std::string laneName = templ->sequence().name.empty() ? name : templ->sequence().name;
    m_lanes.push_back({templ, laneName, templ->sequence(), {}});
    if (!templ->visibleSites().empty() && templ->visibleSites().front().enzyme)
        m_lanes.push_back({templ, laneName, templ->sequence(), {templ->visibleSites().front().enzyme->name}});
    m_selected = int(m_lanes.size());
    m_gel = std::make_unique<GelView>();
    m_dialog = GTK_WIDGET(adw_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(m_dialog), ("Gel · " + name).c_str());
    adw_dialog_set_child(ADW_DIALOG(m_dialog), buildView());
    const int w = parent ? gtk_widget_get_width(parent) : 1300, h = parent ? gtk_widget_get_height(parent) : 820;
    adw_dialog_set_content_width(ADW_DIALOG(m_dialog), std::max(780, int(w * 0.8)));
    adw_dialog_set_content_height(ADW_DIALOG(m_dialog), std::max(520, int(h * 0.8)));
    g_signal_connect_swapped(m_dialog, "destroy", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 if (self->onClosed)
                                     self->onClosed();
                                 delete self;
                             }),
                             this);
    refresh();
}

GelWizard::~GelWizard()
{
    if (m_copiedTimer)
        g_source_remove(m_copiedTimer);
    if (m_refreshIdle)
        g_source_remove(m_refreshIdle);
}

void GelWizard::present()
{
    if (gtk_widget_get_parent(m_dialog))
        gtk_widget_grab_focus(m_dialog);
    else
        adw_dialog_present(ADW_DIALOG(m_dialog), m_parent);
}

void GelWizard::close()
{
    adw_dialog_force_close(ADW_DIALOG(m_dialog));
}

void GelWizard::templateChanged()
{
    refresh();
}

GtkWidget *GelWizard::buildView()
{
    // The gel, always dark as an imager is, with its controls floating over it.
    m_gel->setStain(g_stain);
    m_gel->setBottomInset(72);
    m_gel->onSelectLane = [this](int lane) { selectLane(lane); };
    m_gel->onPickBand = [this](int lane, int band) { pickFragment(lane, band); };

    m_uv = gtk_toggle_button_new_with_label("UV");
    gtk_widget_set_tooltip_text(m_uv, "Ethidium bromide under UV");
    m_blue = gtk_toggle_button_new_with_label("Blue Light");
    gtk_widget_set_tooltip_text(m_blue, "SYBR Safe under blue light");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(m_blue), GTK_TOGGLE_BUTTON(m_uv));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_stain == GelStain::UV ? m_uv : m_blue), TRUE);
    g_signal_connect_swapped(m_uv, "toggled", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 g_stain = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self->m_uv))
                                               ? GelStain::UV
                                               : GelStain::BlueLight;
                                 self->m_gel->setStain(g_stain);
                             }),
                             this);
    GtkWidget *stain = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(stain, "linked");
    gtk_box_append(GTK_BOX(stain), m_uv);
    gtk_box_append(GTK_BOX(stain), m_blue);
    m_copy = gtk_button_new();
    GtkWidget *copyContent = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(copyContent), "edit-copy-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(copyContent), "Copy Image");
    gtk_button_set_child(GTK_BUTTON(m_copy), copyContent);
    gtk_widget_add_css_class(m_copy, "flat");
    gtk_widget_set_tooltip_text(m_copy, "Copy the gel as a picture");
    g_signal_connect_swapped(m_copy, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GelWizard *>(p)->copyImage();
                             }),
                             this);
    GtkWidget *pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(pill, "restricto-gel-pill");
    gtk_widget_set_halign(pill, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(pill, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(pill, 16);
    gtk_box_append(GTK_BOX(pill), stain);
    gtk_box_append(GTK_BOX(pill), m_copy);
    GtkWidget *stage = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(stage), m_gel->widget());
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), pill);
    gtk_widget_set_hexpand(stage, TRUE);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(body), buildSidebar());
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(body), stage);

    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    GtkWidget *done = gtk_button_new_with_label("Done");
    gtk_widget_add_css_class(done, "suggested-action");
    g_signal_connect_swapped(done, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GelWizard *>(p)->close();
                             }),
                             this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), done);
    adw_dialog_set_default_widget(ADW_DIALOG(m_dialog), done);

    // Enzymes picked from the menu come here, by name.
    GSimpleActionGroup *actions = g_simple_action_group_new();
    GSimpleAction *add = g_simple_action_new("add-enzyme", G_VARIANT_TYPE_STRING);
    g_signal_connect(add, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *name, gpointer p) {
                         static_cast<GelWizard *>(p)->addEnzyme(g_variant_get_string(name, nullptr));
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(add));
    g_object_unref(add);
    gtk_widget_insert_action_group(m_dialog, "gel", G_ACTION_GROUP(actions));
    g_object_unref(actions);

    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), body);
    return view;
}

GtkWidget *GelWizard::buildSidebar()
{
    GtkWidget *intro = gtk_label_new("Digests run side by side, as on a 1% agarose gel.");
    gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_widget_add_css_class(intro, "dim-label");

    GtkWidget *ladderGroup = adw_preferences_group_new();
    m_ladder = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_ladder), "Ladder");
    std::vector<const char *> names;
    for (const Ladder &l : commonLadders())
        names.push_back(l.name.c_str());
    names.push_back(nullptr);
    GtkStringList *ladders = gtk_string_list_new(names.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(m_ladder), G_LIST_MODEL(ladders));
    g_object_unref(ladders);
    adw_combo_row_set_selected(ADW_COMBO_ROW(m_ladder), guint(std::clamp(g_ladder, 0, int(commonLadders().size()) - 1)));
    g_signal_connect_swapped(m_ladder, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 g_ladder = int(adw_combo_row_get_selected(ADW_COMBO_ROW(self->m_ladder)));
                                 self->refresh();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(ladderGroup), m_ladder);

    // The lanes, numbered as on the gel.
    GtkWidget *lanes = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(lanes), "Lanes");
    m_add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(m_add, "Add a lane");
    m_remove = gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_set_tooltip_text(m_remove, "Remove the selected lane");
    GtkWidget *laneButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(laneButtons, "linked");
    gtk_widget_set_valign(laneButtons, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(laneButtons), m_add);
    gtk_box_append(GTK_BOX(laneButtons), m_remove);
    g_signal_connect_swapped(m_add, "clicked", G_CALLBACK(+[](gpointer p) { static_cast<GelWizard *>(p)->addLane(); }),
                             this);
    g_signal_connect_swapped(m_remove, "clicked",
                             G_CALLBACK(+[](gpointer p) { static_cast<GelWizard *>(p)->removeLane(); }), this);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(lanes), laneButtons);
    m_laneList = gtk_list_box_new();
    gtk_widget_add_css_class(m_laneList, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_laneList), GTK_SELECTION_SINGLE);
    g_signal_connect(m_laneList, "row-selected", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *self = static_cast<GelWizard *>(p);
                         if (!self->m_filling && row)
                             self->selectLane(gtk_list_box_row_get_index(row) + 1);
                     }),
                     this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(lanes), m_laneList);

    // The selected lane: its sequence and enzymes.
    m_laneGroup = adw_preferences_group_new();
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(m_laneGroup), "Enzymes in one lane cut together.");
    m_sequence = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_sequence), "Sequence");
    g_signal_connect_swapped(m_sequence, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 if (!self->m_filling)
                                     self->sequenceChosen();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_laneGroup), m_sequence);
    m_enzymeEntry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_enzymeEntry), "Add an enzyme");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(m_enzymeEntry), TRUE);
    auto typed = +[](gpointer p) {
        auto *self = static_cast<GelWizard *>(p);
        self->addEnzyme(gtk_editable_get_text(GTK_EDITABLE(self->m_enzymeEntry)));
    };
    g_signal_connect_swapped(m_enzymeEntry, "apply", G_CALLBACK(typed), this);
    g_signal_connect_swapped(m_enzymeEntry, "entry-activated", G_CALLBACK(typed), this);
    m_enzymeMenu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_enzymeMenu), "view-more-symbolic");
    gtk_widget_add_css_class(m_enzymeMenu, "flat");
    gtk_widget_set_valign(m_enzymeMenu, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(m_enzymeMenu, "Add an enzyme that cuts this sequence");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(m_enzymeEntry), m_enzymeMenu);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_laneGroup), m_enzymeEntry);
    // The lane's enzymes as chips; clicking one takes it out.
    m_chips = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(m_chips), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(m_chips), 8);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(m_chips), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(m_chips), 6);
    gtk_widget_set_margin_top(m_chips, 8);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_laneGroup), m_chips);
    m_enzymeNote = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(m_enzymeNote), TRUE);
    gtk_label_set_xalign(GTK_LABEL(m_enzymeNote), 0);
    gtk_widget_add_css_class(m_enzymeNote, "caption");
    gtk_widget_add_css_class(m_enzymeNote, "warning");
    gtk_widget_set_margin_top(m_enzymeNote, 6);
    gtk_widget_set_visible(m_enzymeNote, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_laneGroup), m_enzymeNote);

    // Its fragments; clicking one selects it in its sequence.
    m_fragmentGroup = adw_preferences_group_new();
    m_fragmentList = gtk_list_box_new();
    gtk_widget_add_css_class(m_fragmentList, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_fragmentList), GTK_SELECTION_NONE);
    g_signal_connect(m_fragmentList, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *self = static_cast<GelWizard *>(p);
                         self->pickFragment(self->m_selected, gtk_list_box_row_get_index(row));
                     }),
                     this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(m_fragmentGroup), m_fragmentList);

    GtkWidget *inputs = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(inputs, 16);
    gtk_widget_set_margin_end(inputs, 16);
    gtk_widget_set_margin_top(inputs, 12);
    gtk_widget_set_margin_bottom(inputs, 18);
    for (GtkWidget *w : {intro, ladderGroup, lanes, m_laneGroup, m_fragmentGroup})
        gtk_box_append(GTK_BOX(inputs), w);
    GtkWidget *sidebar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar), inputs);
    gtk_widget_set_size_request(sidebar, 340, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);
    return sidebar;
}

Document *GelWizard::live(const Lane &lane) const
{
    for (const GelSource &s : m_sources ? m_sources() : std::vector<GelSource>{})
        if (s.doc == lane.doc)
            return s.doc;
    return nullptr;
}

// Rebuilds the gel from the lanes, running the ones that changed.
void GelWizard::refresh()
{
    // Lanes follow their sequences while those are open.
    for (Lane &lane : m_lanes)
        if (Document *d = live(lane))
            lane.seq = d->sequence();
    const Ladder &ladder = commonLadders()[size_t(std::clamp(g_ladder, 0, int(commonLadders().size()) - 1))];
    std::vector<view::GelLane> gel{{"L", ladder.bands, ladder.bright, true, false}};
    m_fragments = {{}};
    for (const Lane &lane : m_lanes) {
        view::GelLane l{std::to_string(gel.size()), {}, {}, false, false};
        std::vector<Fragment> frags = fragmentsOf(lane);
        if (lane.enzymes.empty()) {
            l.bands = {lane.seq.length()};
            l.uncutCircular = lane.seq.circular;
        } else {
            const DigestResult r = digest(lane.seq, named(lane.enzymes));
            l.uncutCircular = r.uncut && lane.seq.circular;
            for (const DigestFragment &f : r.fragments)
                l.bands.push_back(f.length);
            std::sort(l.bands.rbegin(), l.bands.rend());
        }
        gel.push_back(l);
        m_fragments.push_back(std::move(frags));
    }
    std::vector<GelLaneModel> models;
    for (size_t i = 0; i < gel.size(); ++i) {
        GelLaneModel m{i == 0 ? "L" : std::to_string(i), {}, i == 0};
        for (size_t j = 0; j < gel[i].bands.size(); ++j) {
            const int bp = gel[i].bands[j];
            const bool reference = std::find(gel[i].bright.begin(), gel[i].bright.end(), bp) != gel[i].bright.end();
            m.bands.push_back({bp, view::gelRunFraction(bp, gel[i].uncutCircular), view::gelBandIntensity(gel[i], bp),
                               reference, i < m_fragments.size() && j < m_fragments[i].size()
                                              ? m_fragments[i][j].caption
                                              : std::string()});
        }
        models.push_back(std::move(m));
    }
    std::vector<std::string> keys{"ladder " + std::to_string(g_ladder)};
    for (const Lane &lane : m_lanes)
        keys.push_back(lane.key());
    std::set<int> running;
    for (size_t i = 0; i < keys.size(); ++i)
        if (i >= m_drawnKeys.size() || m_drawnKeys[i] != keys[i])
            running.insert(int(i));
    m_drawnKeys = keys;
    m_selected = std::clamp(m_selected, 1, int(m_lanes.size()));
    m_gel->setSelectedLane(m_selected);
    m_gel->setLanes(std::move(models), running);
    fillSidebar();
}

// The lane's fragments, largest first as its bands are, with their ends.
std::vector<GelWizard::Fragment> GelWizard::fragmentsOf(const Lane &lane) const
{
    if (lane.enzymes.empty())
        return {{0, lane.seq.length(), lane.seq.circular ? "uncut, supercoiled" : "uncut, linear"}};
    std::vector<Fragment> out;
    for (const DigestFragment &f : digest(lane.seq, named(lane.enzymes)).fragments) {
        const std::string ends = f.leftEnzyme.empty() && f.rightEnzyme.empty()
                                     ? "uncut"
                                     : (f.leftEnzyme.empty() ? "end" : f.leftEnzyme) + " – "
                                           + (f.rightEnzyme.empty() ? "end" : f.rightEnzyme);
        out.push_back({f.start, f.length, ends});
    }
    // Nothing cut a circle: it runs supercoiled, as the gel draws it.
    if (out.size() == 1 && lane.seq.circular && out[0].caption == "uncut")
        return {{0, lane.seq.length(), "not cut, supercoiled"}};
    std::stable_sort(out.begin(), out.end(), [](const Fragment &a, const Fragment &b) { return a.length > b.length; });
    return out;
}

void GelWizard::fillSidebar()
{
    m_filling = true;
    gtk_list_box_remove_all(GTK_LIST_BOX(m_laneList));
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const Lane &lane = m_lanes[i];
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), lane.name.empty() ? "Untitled" : lane.name.c_str());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    lane.enzymes.empty() ? "Uncut" : joined(lane.enzymes, " + ").c_str());
        GtkWidget *badge = gtk_label_new(std::to_string(i + 1).c_str());
        gtk_widget_add_css_class(badge, "restricto-lane-badge");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), badge);
        gtk_list_box_append(GTK_LIST_BOX(m_laneList), row);
        if (int(i) + 1 == m_selected)
            gtk_list_box_select_row(GTK_LIST_BOX(m_laneList), GTK_LIST_BOX_ROW(row));
    }
    gtk_widget_set_sensitive(m_add, m_lanes.size() < kMaxLanes);
    gtk_widget_set_sensitive(m_remove, m_lanes.size() > 1);

    // The selected lane's sequence and enzymes.
    const Lane &lane = m_lanes[size_t(m_selected - 1)];
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_laneGroup), ("Lane " + std::to_string(m_selected)).c_str());
    m_listed = m_sources ? m_sources() : std::vector<GelSource>{};
    m_listed.erase(std::remove_if(m_listed.begin(), m_listed.end(),
                                  [](const GelSource &s) { return s.doc->isEmpty(); }),
                   m_listed.end());
    if (!live(lane))
        m_listed.insert(m_listed.begin(), {lane.doc, lane.name});
    std::vector<std::string> titles;
    guint pick = 0;
    for (size_t i = 0; i < m_listed.size(); ++i) {
        const GelSource &s = m_listed[i];
        const int length = s.doc == lane.doc ? lane.seq.length() : s.doc->sequence().length();
        titles.push_back((s.name.empty() ? std::string("Untitled") : s.name) + "  ·  " + view::withThousands(length)
                         + " bp");
        if (s.doc == lane.doc)
            pick = guint(i);
    }
    std::vector<const char *> strings;
    for (const std::string &t : titles)
        strings.push_back(t.c_str());
    strings.push_back(nullptr);
    GtkStringList *model = gtk_string_list_new(strings.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(m_sequence), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(m_sequence), pick);

    gtk_flow_box_remove_all(GTK_FLOW_BOX(m_chips));
    for (const std::string &e : lane.enzymes) {
        GtkWidget *chip = gtk_button_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_append(GTK_BOX(content), gtk_label_new(e.c_str()));
        gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name("window-close-symbolic"));
        gtk_button_set_child(GTK_BUTTON(chip), content);
        gtk_widget_add_css_class(chip, "restricto-chip");
        gtk_widget_set_tooltip_text(chip, ("Take " + e + " out of this lane").c_str());
        g_object_set_data_full(G_OBJECT(chip), "oc-enzyme", g_strdup(e.c_str()), g_free);
        g_signal_connect(chip, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer p) {
                             static_cast<GelWizard *>(p)->removeEnzyme(
                                 static_cast<const char *>(g_object_get_data(G_OBJECT(b), "oc-enzyme")));
                         }),
                         this);
        gtk_flow_box_append(GTK_FLOW_BOX(m_chips), chip);
    }
    gtk_widget_set_visible(m_chips, !lane.enzymes.empty());

    // Enzymes that cut this sequence, the fewest cuts first.
    std::map<std::string, int> cuts;
    for (const CutSite &s : findSites(lane.seq, commonEnzymes()))
        if (s.enzyme)
            ++cuts[s.enzyme->name];
    GMenu *menu = g_menu_new();
    const struct {
        const char *title;
        int lo, hi;
    } groups[] = {{"Cut once", 1, 1}, {"Cut twice", 2, 2}, {"Cut 3 to 6 times", 3, 6}};
    for (const auto &group : groups) {
        GMenu *section = g_menu_new();
        for (const auto &[name, n] : cuts)
            if (n >= group.lo && n <= group.hi) {
                GMenuItem *item = g_menu_item_new(name.c_str(), nullptr);
                g_menu_item_set_action_and_target_value(item, "gel.add-enzyme", g_variant_new_string(name.c_str()));
                g_menu_append_item(section, item);
                g_object_unref(item);
            }
        if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0)
            g_menu_append_section(menu, group.title, G_MENU_MODEL(section));
        g_object_unref(section);
    }
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(m_enzymeMenu), G_MENU_MODEL(menu));
    g_object_unref(menu);

    // Its fragments.
    gtk_list_box_remove_all(GTK_LIST_BOX(m_fragmentList));
    const std::vector<Fragment> &frags = size_t(m_selected) < m_fragments.size() ? m_fragments[size_t(m_selected)]
                                                                                 : std::vector<Fragment>{};
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_fragmentGroup),
                                    frags.size() == 1 ? "1 Fragment"
                                                      : (std::to_string(frags.size()) + " Fragments").c_str());
    for (const Fragment &f : frags) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), (view::withThousands(f.length) + " bp").c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), f.caption.c_str());
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        gtk_widget_set_tooltip_text(row, ("Select in " + lane.name).c_str());
        gtk_list_box_append(GTK_LIST_BOX(m_fragmentList), row);
    }
    m_filling = false;
}

void GelWizard::selectLane(int lane)
{
    if (lane < 1 || lane > int(m_lanes.size()) || lane == m_selected)
        return;
    m_selected = lane;
    m_gel->setSelectedLane(lane);
    gtk_widget_set_visible(m_enzymeNote, FALSE);
    fillSidebar();
}

// Selects fragment `index` of `lane` in its sequence's window.
void GelWizard::pickFragment(int lane, int index)
{
    if (lane < 1 || lane > int(m_lanes.size()) || size_t(lane) >= m_fragments.size() || index < 0
        || size_t(index) >= m_fragments[size_t(lane)].size())
        return;
    const Fragment &f = m_fragments[size_t(lane)][size_t(index)];
    if (Document *d = live(m_lanes[size_t(lane - 1)]))
        d->setSelection({f.start, f.length});
}

void GelWizard::addLane()
{
    if (int(m_lanes.size()) >= kMaxLanes)
        return;
    // Same sequence as the lane being edited, uncut, ready for an enzyme.
    Lane copy = m_lanes[size_t(m_selected - 1)];
    copy.enzymes.clear();
    m_lanes.insert(m_lanes.begin() + m_selected, copy);
    ++m_selected;
    refresh();
    gtk_widget_grab_focus(m_enzymeEntry);
}

void GelWizard::removeLane()
{
    if (m_lanes.size() <= 1)
        return;
    m_lanes.erase(m_lanes.begin() + (m_selected - 1));
    m_selected = std::max(1, m_selected - 1);
    // Lanes after it moved over: they run again in their new places.
    refresh();
}

void GelWizard::addEnzyme(const std::string &typed)
{
    // Several at once, by commas or spaces; names are matched ignoring case.
    std::vector<std::string> unknown;
    Lane &lane = m_lanes[size_t(m_selected - 1)];
    bool changed = false;
    std::string word;
    auto take = [&] {
        if (word.empty())
            return;
        const Enzyme *found = nullptr;
        for (const Enzyme &e : commonEnzymes())
            if (lower(e.name) == lower(word))
                found = &e;
        if (!found)
            unknown.push_back(word);
        else if (std::find(lane.enzymes.begin(), lane.enzymes.end(), found->name) == lane.enzymes.end()) {
            lane.enzymes.push_back(found->name);
            changed = true;
        }
        word.clear();
    };
    for (char c : typed + " ") {
        if (c == ',' || c == ' ' || c == '+')
            take();
        else
            word += c;
    }
    gtk_label_set_text(GTK_LABEL(m_enzymeNote), ("Not a known enzyme: " + joined(unknown, ", ")).c_str());
    gtk_widget_set_visible(m_enzymeNote, !unknown.empty());
    if (unknown.empty())
        gtk_editable_set_text(GTK_EDITABLE(m_enzymeEntry), "");
    if (changed)
        refresh();
}

void GelWizard::removeEnzyme(const std::string &name)
{
    std::vector<std::string> &e = m_lanes[size_t(m_selected - 1)].enzymes;
    e.erase(std::remove(e.begin(), e.end(), name), e.end());
    refresh();
}

void GelWizard::sequenceChosen()
{
    const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(m_sequence));
    Lane &lane = m_lanes[size_t(m_selected - 1)];
    if (i >= m_listed.size() || m_listed[i].doc == lane.doc)
        return;
    const GelSource &s = m_listed[i];
    // Another sequence may not have the enzymes' sites; keep those that cut it.
    std::set<std::string> cutting;
    for (const CutSite &site : findSites(s.doc->sequence(), named(lane.enzymes)))
        if (site.enzyme)
            cutting.insert(site.enzyme->name);
    std::vector<std::string> kept;
    for (const std::string &e : lane.enzymes)
        if (cutting.count(e))
            kept.push_back(e);
    lane = {s.doc, s.name, s.doc->sequence(), kept};
    // Choosing from the list mustn't refill it while it's still open.
    if (!m_refreshIdle)
        m_refreshIdle = g_idle_add_once([](gpointer p) {
            auto *self = static_cast<GelWizard *>(p);
            self->m_refreshIdle = 0;
            self->refresh();
        }, this);
}

void GelWizard::copyImage()
{
    GBytes *png = m_gel->snapshotPng();
    GError *error = nullptr;
    GdkTexture *texture = gdk_texture_new_from_bytes(png, &error);
    g_bytes_unref(png);
    if (!texture) {
        g_clear_error(&error);
        gtk_widget_error_bell(m_dialog);
        return;
    }
    gdk_clipboard_set_texture(gtk_widget_get_clipboard(m_dialog), texture);
    g_object_unref(texture);
    // A moment's tick on the button says it worked.
    GtkWidget *content = gtk_button_get_child(GTK_BUTTON(m_copy));
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "object-select-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "Copied");
    if (m_copiedTimer)
        g_source_remove(m_copiedTimer);
    m_copiedTimer = g_timeout_add(1200, [](gpointer p) -> gboolean {
        auto *self = static_cast<GelWizard *>(p);
        self->m_copiedTimer = 0;
        GtkWidget *c = gtk_button_get_child(GTK_BUTTON(self->m_copy));
        adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(c), "edit-copy-symbolic");
        adw_button_content_set_label(ADW_BUTTON_CONTENT(c), "Copy Image");
        return G_SOURCE_REMOVE;
    }, this);
}

} // namespace oc::gtk
