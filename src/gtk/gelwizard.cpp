// SPDX-License-Identifier: GPL-3.0-or-later
#include "gelwizard.h"
#include "window.h"

#include "core/digest.h"
#include "core/enzymes.h"
#include "view/gellayout.h"
#include "view/maplayout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace oc::gtk {

namespace {

// Kept while the app runs: the gel opens as it was last left.
int g_ladder = 0;
int g_comb = 1;
double g_percent = 1.0;
GelStain g_stain = GelStain::EthidiumUV;
Gel3DScene g_scene3D = Gel3DScene::PinkOnBlue;
// The 3D picture's knobs, kept while the app runs (experimental: for
// settling on the shipped values by eye).
Gel3DLook g_look3D;
struct Knob {
    const char *name;
    float Gel3DLook::*value;
    double lo, hi, step;
};
const Knob kKnobs[] = {
    {"Exposure", &Gel3DLook::exposure, 0.2, 2.0, 0.05},   {"Contrast", &Gel3DLook::contrast, 0.6, 2.5, 0.05},
    {"Stain glow", &Gel3DLook::stain, 0.0, 3.0, 0.05},    {"Band glow", &Gel3DLook::band, 0.0, 3.0, 0.05},
    {"Scatter", &Gel3DLook::scatter, 0.0, 60.0, 1.0},     {"Lamp", &Gel3DLook::lamp, 0.0, 30.0, 0.5},
    {"Lamp size", &Gel3DLook::lampSize, 0.01, 0.2, 0.005}, {"Spot cone", &Gel3DLook::spot, 0.1, 1.2, 0.02},
    {"Bloom", &Gel3DLook::bloom, 0.0, 1.0, 0.02},         {"Vignette", &Gel3DLook::vignette, 0.0, 1.0, 0.05},
    {"Grain", &Gel3DLook::grain, 0.0, 0.1, 0.005},
};
// Each sequence's lanes as its gel was last closed, so reopening it keeps them.
struct Remembered {
    Document *doc;
    std::vector<GelWizard::Lane> lanes;
    int selected;
};
std::vector<Remembered> g_remembered;

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

const Ladder &ladderAt(int i)
{
    return commonLadders()[size_t(std::clamp(i, 0, int(commonLadders().size()) - 1))];
}

// "0.5% agarose", "1% agarose".
std::string percentTitle(double percent)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, std::fabs(percent - std::round(percent)) < 0.01 ? "%.0f%% agarose" : "%.1f%% agarose",
                  percent);
    return buf;
}

GtkStringList *stringList(const std::vector<std::string> &items)
{
    std::vector<const char *> strings;
    for (const std::string &t : items)
        strings.push_back(t.c_str());
    strings.push_back(nullptr);
    return gtk_string_list_new(strings.data());
}

} // namespace

std::string GelWizard::Lane::key() const
{
    if (empty)
        return "empty";
    if (isLadder())
        return "ladder " + std::to_string(ladder);
    return std::to_string(reinterpret_cast<uintptr_t>(doc)) + "|" + std::to_string(seq.length()) + "|"
         + joined(enzymes, ",");
}

GelWizard::GelWizard(Document *templ, GtkWidget *parent, const std::string &name,
                     std::function<std::vector<GelSource>()> sources)
    : m_template(templ), m_sources(std::move(sources)), m_parent(parent)
{
    const auto saved = std::find_if(g_remembered.begin(), g_remembered.end(),
                                    [&](const Remembered &r) { return r.doc == templ; });
    if (saved != g_remembered.end()) {
        m_lanes = saved->lanes;
        m_selected = saved->selected;
    } else {
        // The ladder last used, the sequence uncut, and cut with its first enzyme.
        const std::string laneName = templ->sequence().name.empty() ? name : templ->sequence().name;
        m_lanes.push_back({templ, ladderAt(g_ladder).name, {}, {}, std::clamp(g_ladder, 0, int(commonLadders().size()) - 1)});
        m_lanes.push_back({templ, laneName, templ->sequence(), {}});
        if (!templ->visibleSites().empty() && templ->visibleSites().front().enzyme)
            m_lanes.push_back({templ, laneName, templ->sequence(), {templ->visibleSites().front().enzyme->name}});
        m_selected = int(m_lanes.size()) - 1;
    }
    m_gel = std::make_unique<GelView>();
    m_dialog = GTK_WIDGET(adw_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(m_dialog), ("Gel · " + name).c_str());
    adw_dialog_set_child(ADW_DIALOG(m_dialog), buildView());
    const int w = parent ? gtk_widget_get_width(parent) : 1300, h = parent ? gtk_widget_get_height(parent) : 820;
    adw_dialog_set_content_width(ADW_DIALOG(m_dialog), std::max(780, int(w * 0.8)));
    adw_dialog_set_content_height(ADW_DIALOG(m_dialog), std::max(520, int(h * 0.8)));
    // Esc while typing an enzyme only leaves the box; otherwise it closes the gel.
    adw_dialog_set_can_close(ADW_DIALOG(m_dialog), FALSE);
    g_signal_connect_swapped(m_dialog, "close-attempt", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 GtkRoot *root = gtk_widget_get_root(self->m_dialog);
                                 GtkWidget *focus = root ? gtk_root_get_focus(root) : nullptr;
                                 if (focus && (focus == self->m_enzymeEntry
                                               || gtk_widget_is_ancestor(focus, self->m_enzymeEntry))) {
                                     // To the Done button: focusing the lane list scrolled
                                     // the sidebar to the selected lane, under the pointer.
                                     gtk_widget_grab_focus(self->m_done);
                                     return;
                                 }
                                 self->close();
                             }),
                             this);
    g_signal_connect_swapped(m_dialog, "destroy", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 // The dialog's children are disposed after this, and their
                                 // signals (a list emptied, a menu dropped) must not reach
                                 // what's about to be deleted.
                                 self->m_closing = true;
                                 disconnectTree(self->m_dialog, self);
                                 if (self->onClosed)
                                     self->onClosed();
                                 // Deleted once the dialog is fully gone: a child's
                                 // last signal still finds a live object that does
                                 // nothing.
                                 g_idle_add_once([](gpointer q) { delete static_cast<GelWizard *>(q); }, self);
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
    g_remembered.erase(std::remove_if(g_remembered.begin(), g_remembered.end(),
                                      [&](const Remembered &r) { return r.doc == m_template; }),
                       g_remembered.end());
    g_remembered.push_back({m_template, m_lanes, m_selected});
    adw_dialog_force_close(ADW_DIALOG(m_dialog));
}

void GelWizard::templateChanged()
{
    if (m_closing)
        return;
    refresh();
}

GtkWidget *GelWizard::buildView()
{
    // The gel, always dark as an imager is, with its controls floating over it.
    m_gel->setStain(g_stain);
    m_gel->setComb(gelCombs()[size_t(std::clamp(g_comb, 0, int(gelCombs().size()) - 1))]);
    m_gel->setBottomInset(72);
    m_gel->onSelectLane = [this](int lane) { selectLane(lane); };
    m_gel->onPickBand = [this](int lane, int band) { pickFragment(lane, band); };

    // How the gel was stained and photographed.
    m_stain = gtk_drop_down_new(nullptr, nullptr);
    fillStains();
    gtk_widget_set_tooltip_text(m_stain, "How the gel was stained and photographed");
    g_signal_connect_swapped(m_stain, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GelWizard *>(p)->stainChanged();
                             }),
                             this);
    // How long the gel ran: drag it back to see the gel part way through.
    GtkWidget *runIcon = gtk_image_new_from_icon_name("restricto-run-symbolic");
    gtk_widget_set_tooltip_text(runIcon, "How long the gel ran");
    m_run = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.1, 1, 0.01);
    gtk_range_set_value(GTK_RANGE(m_run), 1);
    gtk_scale_set_draw_value(GTK_SCALE(m_run), FALSE);
    gtk_widget_set_size_request(m_run, 110, -1);
    gtk_widget_set_tooltip_text(m_run, "How long the gel ran");
    g_signal_connect_swapped(m_run, "value-changed", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GelWizard *>(p)->runChanged();
                             }),
                             this);
    m_runLabel = gtk_label_new("Full run");
    gtk_widget_add_css_class(m_runLabel, "numeric");
    gtk_widget_add_css_class(m_runLabel, "dim-label");
    gtk_widget_set_size_request(m_runLabel, 58, -1);
    gtk_label_set_xalign(GTK_LABEL(m_runLabel), 0);
    // Icon only, so the pill fits over the gel at the dialog's smallest.
    m_copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
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
    gtk_box_append(GTK_BOX(pill), m_stain);
    gtk_widget_set_margin_end(m_stain, 8);
    gtk_box_append(GTK_BOX(pill), runIcon);
    gtk_box_append(GTK_BOX(pill), m_run);
    gtk_box_append(GTK_BOX(pill), m_runLabel);
    gtk_box_append(GTK_BOX(pill), m_copy);
    if (Window::experimentalTools()) {
        m_tune = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_tune), "restricto-tune-symbolic");
        gtk_widget_set_tooltip_text(m_tune, "Tune the 3D picture");
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(m_tune), buildTuner());
        gtk_widget_set_visible(m_tune, FALSE);
        gtk_box_append(GTK_BOX(pill), m_tune);
    }
    // The Imager's print and, with Experimental Tools on, the gel in 3D.
    m_stage = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(m_stage), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(m_stage), m_gel->widget(), "imager");
    GtkWidget *stage = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(stage), m_stage);
    gtk_overlay_add_overlay(GTK_OVERLAY(stage), pill);
    gtk_widget_set_hexpand(stage, TRUE);
    // Experimental: the Imager or 3D, switched in the header bar as GNOME apps do.
    GtkWidget *tabs = nullptr;
    if (Window::experimentalTools()) {
        tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(tabs, "linked");
        const char *titles[] = {"Imager", "3D"};
        const char *tips[] = {"The gel as an imager prints it",
                              "The gel in 3D (experimental): drag to turn it, scroll to come closer"};
        for (int i = 0; i < 2; ++i) {
            m_lookTabs[i] = gtk_toggle_button_new_with_label(titles[i]);
            gtk_widget_set_tooltip_text(m_lookTabs[i], tips[i]);
            if (i)
                gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(m_lookTabs[i]), GTK_TOGGLE_BUTTON(m_lookTabs[0]));
            g_object_set_data(G_OBJECT(m_lookTabs[i]), "oc-look", GINT_TO_POINTER(i));
            g_signal_connect(m_lookTabs[i], "toggled", G_CALLBACK(+[](GtkToggleButton *b, gpointer p) {
                                 if (gtk_toggle_button_get_active(b))
                                     static_cast<GelWizard *>(p)->lookChanged(
                                         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "oc-look")));
                             }),
                             this);
            gtk_box_append(GTK_BOX(tabs), m_lookTabs[i]);
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_lookTabs[0]), TRUE);
    }

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(body), buildSidebar());
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(body), stage);

    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    GtkWidget *done = m_done = gtk_button_new_with_label("Done");
    gtk_widget_add_css_class(done, "suggested-action");
    g_signal_connect_swapped(done, "clicked", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GelWizard *>(p)->close();
                             }),
                             this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), done);
    if (tabs)
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), tabs);
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
    GtkWidget *intro = gtk_label_new("Digests run side by side on an agarose gel.");
    gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_widget_add_css_class(intro, "dim-label");

    // The gel: how dense it was cast, and the comb that cut its wells.
    GtkWidget *gelGroup = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(gelGroup), "Gel");
    m_agarose = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_agarose), "Agarose");
    std::vector<std::string> percents;
    guint pickPercent = 0;
    for (int i = 0; i < view::gelPercentCount(); ++i) {
        percents.push_back(percentTitle(view::gelPercent(i)));
        if (std::fabs(view::gelPercent(i) - g_percent) < 0.01)
            pickPercent = guint(i);
    }
    GtkStringList *percentModel = stringList(percents);
    adw_combo_row_set_model(ADW_COMBO_ROW(m_agarose), G_LIST_MODEL(percentModel));
    g_object_unref(percentModel);
    adw_combo_row_set_selected(ADW_COMBO_ROW(m_agarose), pickPercent);
    gtk_widget_set_tooltip_text(m_agarose, "Denser gels separate small fragments; thinner ones, large ones");
    g_signal_connect_swapped(m_agarose, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 g_percent = self->percent();
                                 self->refresh();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gelGroup), m_agarose);
    m_comb = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_comb), "Comb");
    std::vector<std::string> combs;
    for (const GelComb &c : gelCombs())
        combs.push_back(c.name);
    GtkStringList *combModel = stringList(combs);
    adw_combo_row_set_model(ADW_COMBO_ROW(m_comb), G_LIST_MODEL(combModel));
    g_object_unref(combModel);
    adw_combo_row_set_selected(ADW_COMBO_ROW(m_comb), guint(std::clamp(g_comb, 0, int(gelCombs().size()) - 1)));
    gtk_widget_set_tooltip_text(m_comb, "The comb that cast the wells: a thin one gives sharper bands");
    g_signal_connect_swapped(m_comb, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 g_comb = int(adw_combo_row_get_selected(ADW_COMBO_ROW(self->m_comb)));
                                 self->m_gel->setComb(gelCombs()[size_t(g_comb)]);
                                 if (self->m_gel3D)
                                     self->m_gel3D->setComb(gelCombs()[size_t(g_comb)]);
                                 self->fillSidebar();
                             }),
                             this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(gelGroup), m_comb);

    // The lanes, named as on the gel; the buttons move, add and remove them.
    GtkWidget *lanes = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(lanes), "Lanes");
    auto button = [this](const char *icon, const char *tip, void (*clicked)(GelWizard *)) {
        GtkWidget *b = gtk_button_new_from_icon_name(icon);
        gtk_widget_set_tooltip_text(b, tip);
        g_object_set_data(G_OBJECT(b), "oc-clicked", reinterpret_cast<gpointer>(clicked));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton *b, gpointer p) {
                             reinterpret_cast<void (*)(GelWizard *)>(g_object_get_data(G_OBJECT(b), "oc-clicked"))(
                                 static_cast<GelWizard *>(p));
                         }),
                         this);
        return b;
    };
    m_left = button("go-previous-symbolic", "Move this lane left", [](GelWizard *w) { w->moveLane(-1); });
    m_right = button("go-next-symbolic", "Move this lane right", [](GelWizard *w) { w->moveLane(1); });
    m_add = button("list-add-symbolic", "Add a lane after this one", [](GelWizard *w) { w->addLane(); });
    m_remove = button("list-remove-symbolic", "Remove this lane", [](GelWizard *w) { w->removeLane(); });
    GtkWidget *laneButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(laneButtons, GTK_ALIGN_CENTER);
    for (auto pair : {std::make_pair(m_left, m_right), std::make_pair(m_add, m_remove)}) {
        GtkWidget *linked = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(linked, "linked");
        gtk_box_append(GTK_BOX(linked), pair.first);
        gtk_box_append(GTK_BOX(linked), pair.second);
        gtk_box_append(GTK_BOX(laneButtons), linked);
    }
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(lanes), laneButtons);
    m_laneList = gtk_list_box_new();
    gtk_widget_add_css_class(m_laneList, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_laneList), GTK_SELECTION_SINGLE);
    g_signal_connect(m_laneList, "row-selected", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *row, gpointer p) {
                         auto *self = static_cast<GelWizard *>(p);
                         if (!self->m_filling && row)
                             self->selectLane(gtk_list_box_row_get_index(row));
                     }),
                     this);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(lanes), m_laneList);

    // The selected lane: what's loaded, and a sample's enzymes.
    m_laneGroup = adw_preferences_group_new();
    m_sequence = adw_combo_row_new();
    showWholeNames(ADW_COMBO_ROW(m_sequence));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_sequence), "Loaded");
    g_signal_connect_swapped(m_sequence, "notify::selected", G_CALLBACK(+[](gpointer p) {
                                 auto *self = static_cast<GelWizard *>(p);
                                 if (!self->m_filling)
                                     self->sequenceChosen();
                             }),
                             this);
    // Refilled as it opens, so a sequence opened after the gel is there too.
    GtkGesture *opening = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(opening), GTK_PHASE_CAPTURE);
    g_signal_connect_swapped(opening, "pressed", G_CALLBACK(+[](gpointer p) {
                                 static_cast<GelWizard *>(p)->fillSequences();
                             }),
                             this);
    gtk_widget_add_controller(m_sequence, GTK_EVENT_CONTROLLER(opening));
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
    for (GtkWidget *w : {intro, gelGroup, lanes, m_laneGroup, m_fragmentGroup})
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

double GelWizard::percent() const
{
    return view::gelPercent(int(adw_combo_row_get_selected(ADW_COMBO_ROW(m_agarose))));
}

// What each lane is called on the gel: "L" for a ladder, the samples
// numbered among themselves.
std::vector<std::string> GelWizard::laneNames() const
{
    std::vector<std::string> out;
    int n = 0;
    for (const Lane &lane : m_lanes)
        out.push_back(lane.isLadder() ? "L" : lane.empty ? "–" : std::to_string(++n));
    return out;
}

// Rebuilds the gel from the lanes, running the ones that changed.
void GelWizard::refresh()
{
    if (m_closing)
        return;
    // Lanes follow their sequences while those are open.
    for (Lane &lane : m_lanes)
        if (Document *d = lane.isSample() ? live(lane) : nullptr)
            lane.seq = d->sequence();
    const double pc = percent();
    // Bromophenol blue runs with ~300 bp fragments, xylene cyanol with ~4 kb.
    m_gel->setDyeRuns({view::gelRunFraction(300, false, pc), view::gelRunFraction(4000, false, pc)});
    const std::vector<std::string> names = laneNames();
    m_fragments.clear();
    std::vector<GelLaneModel> models;
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const Lane &lane = m_lanes[i];
        view::GelLane l{names[i], {}, {}, lane.isLadder(), false, {}};
        if (lane.empty) {
            // A free well: nothing runs.
        } else if (lane.isLadder()) {
            l.bands = ladderAt(lane.ladder).bands;
            l.bright = ladderAt(lane.ladder).bright;
        } else {
            const std::vector<Enzyme> use = named(lane.enzymes);
            const DigestResult r = use.empty() ? DigestResult{} : digest(lane.seq, use);
            if (use.empty() || r.uncut) {
                l.bands = {lane.seq.length()};
                if (lane.seq.circular) {
                    // The nicked open circle trails the supercoiled plasmid.
                    l.uncutCircular = true;
                    l.bands = {lane.seq.length(), lane.seq.length()};
                    l.forms = {view::GelForm::OpenCircular, view::GelForm::Supercoiled};
                }
            } else {
                for (const DigestFragment &f : r.fragments)
                    l.bands.push_back(f.length);
                std::sort(l.bands.rbegin(), l.bands.rend());
            }
        }
        m_fragments.push_back(lane.isSample() ? fragmentsOf(lane) : std::vector<Fragment>{});
        GelLaneModel m{lane.empty ? std::string() : names[i], {}, lane.isLadder()};
        for (size_t j = 0; j < l.bands.size(); ++j) {
            const int bp = l.bands[j];
            const bool reference = std::find(l.bright.begin(), l.bright.end(), bp) != l.bright.end();
            m.bands.push_back({bp, view::gelRunFraction(bp, view::gelBandForm(l, j), pc), view::gelBandIntensity(l, j),
                               reference, j < m_fragments[i].size() ? m_fragments[i][j].caption : std::string()});
        }
        models.push_back(std::move(m));
    }
    // A new percentage runs every lane again; so does a lane that moved.
    std::vector<std::string> keys;
    for (const Lane &lane : m_lanes)
        keys.push_back(std::to_string(pc) + " " + lane.key());
    std::set<int> running;
    for (size_t i = 0; i < keys.size(); ++i)
        if (i >= m_drawnKeys.size() || m_drawnKeys[i] != keys[i])
            running.insert(int(i));
    m_drawnKeys = keys;
    m_selected = std::clamp(m_selected, 0, int(m_lanes.size()) - 1);
    m_gel->setSelectedLane(m_selected);
    m_gel->setLanes(std::move(models), running);
    show3D();
    fillSidebar();
}

// The lane's fragments, largest first as its bands are, with their ends.
std::vector<GelWizard::Fragment> GelWizard::fragmentsOf(const Lane &lane) const
{
    // An uncut plasmid runs twice: the nicked open circle, then the
    // supercoiled form, as the gel draws it.
    const int n = lane.seq.length();
    auto uncutCircle = [n](const char *uncut) {
        return std::vector<Fragment>{{0, n, std::string(uncut) + ", nicked (open circle)"},
                                     {0, n, std::string(uncut) + ", supercoiled"}};
    };
    if (lane.enzymes.empty())
        return lane.seq.circular ? uncutCircle("uncut") : std::vector<Fragment>{{0, n, "uncut, linear"}};
    std::vector<Fragment> out;
    for (const DigestFragment &f : digest(lane.seq, named(lane.enzymes)).fragments) {
        const std::string ends = f.leftEnzyme.empty() && f.rightEnzyme.empty()
                                     ? "uncut"
                                     : (f.leftEnzyme.empty() ? "end" : f.leftEnzyme) + " – "
                                           + (f.rightEnzyme.empty() ? "end" : f.rightEnzyme);
        out.push_back({f.start, f.length, ends});
    }
    // Nothing cut a circle: it runs as an uncut plasmid does.
    if (out.size() == 1 && lane.seq.circular && out[0].caption == "uncut")
        return uncutCircle("not cut");
    std::stable_sort(out.begin(), out.end(), [](const Fragment &a, const Fragment &b) { return a.length > b.length; });
    return out;
}

// Every open sequence, with the lane's own first if it has since been
// closed, then the ladders.
void GelWizard::fillSequences()
{
    const Lane &lane = m_lanes[size_t(m_selected)];
    m_listed = m_sources ? m_sources() : std::vector<GelSource>{};
    m_listed.erase(std::remove_if(m_listed.begin(), m_listed.end(),
                                  [](const GelSource &s) { return s.doc->isEmpty(); }),
                   m_listed.end());
    if (lane.isSample() && !live(lane))
        m_listed.insert(m_listed.begin(), {lane.doc, lane.name});
    std::vector<std::string> titles;
    guint pick = 0;
    for (size_t i = 0; i < m_listed.size(); ++i) {
        const GelSource &s = m_listed[i];
        const int length = s.doc == lane.doc && lane.isSample() ? lane.seq.length() : s.doc->sequence().length();
        titles.push_back((s.name.empty() ? std::string("Untitled") : s.name) + "  ·  " + view::withThousands(length)
                         + " bp");
        if (s.doc == lane.doc && lane.isSample())
            pick = guint(i);
    }
    for (size_t i = 0; i < commonLadders().size(); ++i)
        titles.push_back("Ladder  ·  " + commonLadders()[i].name);
    // Last, a well left free (a gap between samples, as gels are often loaded).
    titles.push_back("Nothing  ·  an empty well");
    if (lane.isLadder())
        pick = guint(m_listed.size() + size_t(lane.ladder));
    if (lane.empty)
        pick = guint(titles.size() - 1);
    const bool filling = m_filling;
    m_filling = true;
    GtkStringList *model = stringList(titles);
    adw_combo_row_set_model(ADW_COMBO_ROW(m_sequence), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(m_sequence), pick);
    m_filling = filling;
}

void GelWizard::fillSidebar()
{
    m_filling = true;
    const std::vector<std::string> names = laneNames();
    gtk_list_box_remove_all(GTK_LIST_BOX(m_laneList));
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const Lane &lane = m_lanes[i];
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), lane.empty          ? "Empty"
                                                                : lane.isLadder()     ? ladderAt(lane.ladder).name.c_str()
                                                                : lane.name.empty() ? "Untitled"
                                                                                    : lane.name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), lane.empty             ? "Nothing loaded"
                                                         : lane.isLadder()         ? "Ladder"
                                                         : lane.enzymes.empty() ? "Uncut"
                                                                                : joined(lane.enzymes, " + ").c_str());
        GtkWidget *badge = gtk_label_new(names[i].c_str());
        gtk_widget_add_css_class(badge, "restricto-lane-badge");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), badge);
        gtk_list_box_append(GTK_LIST_BOX(m_laneList), row);
        if (int(i) == m_selected)
            gtk_list_box_select_row(GTK_LIST_BOX(m_laneList), GTK_LIST_BOX_ROW(row));
    }
    const int teeth = gelCombs()[size_t(std::clamp(g_comb, 0, int(gelCombs().size()) - 1))].teeth;
    gtk_widget_set_sensitive(m_add, int(m_lanes.size()) < teeth);
    gtk_widget_set_sensitive(m_remove, m_lanes.size() > 1);
    gtk_widget_set_sensitive(m_left, m_selected > 0);
    gtk_widget_set_sensitive(m_right, m_selected < int(m_lanes.size()) - 1);
    // A button that just went grey drops the focus, and with nothing in the
    // dialog focused Esc (and Enter for Done) no longer reach it.
    if (GtkRoot *root = gtk_widget_get_root(m_dialog)) {
        GtkWidget *focus = gtk_root_get_focus(root);
        if (!focus || !gtk_widget_is_ancestor(focus, m_dialog))
            gtk_widget_grab_focus(m_done);
    }

    // The selected lane: what's loaded, and a sample's enzymes.
    const Lane &lane = m_lanes[size_t(m_selected)];
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(m_laneGroup),
                                    lane.empty      ? "Empty Well"
                                    : lane.isLadder() ? "Ladder"
                                                      : ("Lane " + names[size_t(m_selected)]).c_str());
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(m_laneGroup),
                                          lane.isSample() ? "Enzymes in one lane cut together." : nullptr);
    fillSequences();
    gtk_widget_set_visible(m_enzymeEntry, lane.isSample());
    if (!lane.isSample())
        gtk_widget_set_visible(m_enzymeNote, FALSE);

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
    gtk_widget_set_visible(m_chips, lane.isSample() && !lane.enzymes.empty());

    // Enzymes that cut this sequence, the fewest cuts first.
    std::map<std::string, int> cuts;
    if (lane.isSample())
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
    gtk_widget_set_visible(m_fragmentGroup, lane.isSample());
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
    if (m_closing)
        return;
    if (lane < 0 || lane >= int(m_lanes.size()) || lane == m_selected)
        return;
    m_selected = lane;
    m_gel->setSelectedLane(lane);
    gtk_widget_set_visible(m_enzymeNote, FALSE);
    fillSidebar();
}

// Selects fragment `index` of `lane` in its sequence's tab.
void GelWizard::pickFragment(int lane, int index)
{
    if (m_closing)
        return;
    if (lane < 0 || lane >= int(m_lanes.size()) || size_t(lane) >= m_fragments.size() || index < 0
        || size_t(index) >= m_fragments[size_t(lane)].size())
        return;
    const Fragment &f = m_fragments[size_t(lane)][size_t(index)];
    if (Document *d = live(m_lanes[size_t(lane)]))
        d->setSelection({f.start, f.length});
}

void GelWizard::addLane()
{
    if (m_closing)
        return;
    const int teeth = gelCombs()[size_t(std::clamp(g_comb, 0, int(gelCombs().size()) - 1))].teeth;
    if (int(m_lanes.size()) >= teeth)
        return;
    // After the lane being edited: the same sequence, uncut, ready for an
    // enzyme (a ladder's neighbour takes the first sample's sequence).
    Lane copy = m_lanes[size_t(m_selected)];
    if (!copy.isSample()) {
        const auto sample = std::find_if(m_lanes.begin(), m_lanes.end(), [](const Lane &l) { return l.isSample(); });
        if (sample != m_lanes.end()) {
            copy = *sample;
        } else {
            copy = {m_template, m_template->sequence().name, m_template->sequence(), {}};
        }
    }
    copy.enzymes.clear();
    copy.ladder = -1;
    copy.empty = false;
    m_lanes.insert(m_lanes.begin() + m_selected + 1, copy);
    ++m_selected;
    // Focus stays on +: moving it to the enzyme box would scroll the sidebar
    // and slide + away from under the pointer.
    refresh();
}

void GelWizard::removeLane()
{
    if (m_closing)
        return;
    if (m_lanes.size() <= 1)
        return;
    m_lanes.erase(m_lanes.begin() + m_selected);
    m_selected = std::min(m_selected, int(m_lanes.size()) - 1);
    // Lanes after it moved over: they run again in their new places.
    refresh();
}

// Swaps the lane with its neighbour; both run again where they now are.
void GelWizard::moveLane(int step)
{
    if (m_closing)
        return;
    const int to = m_selected + step;
    if (to < 0 || to >= int(m_lanes.size()))
        return;
    std::swap(m_lanes[size_t(m_selected)], m_lanes[size_t(to)]);
    m_selected = to;
    refresh();
}

void GelWizard::addEnzyme(const std::string &typed)
{
    if (m_closing)
        return;
    Lane &lane = m_lanes[size_t(m_selected)];
    if (!lane.isSample())
        return;
    // Several at once, by commas or spaces; names are matched ignoring case.
    // Only known enzymes are added; anything else is named in the note.
    std::vector<std::string> unknown;
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
    // Only what wasn't added stays in the box, to be corrected.
    gtk_editable_set_text(GTK_EDITABLE(m_enzymeEntry), joined(unknown, ", ").c_str());
    gtk_editable_set_position(GTK_EDITABLE(m_enzymeEntry), -1);
    if (changed)
        refresh();
}

void GelWizard::removeEnzyme(const std::string &name)
{
    if (m_closing)
        return;
    std::vector<std::string> &e = m_lanes[size_t(m_selected)].enzymes;
    e.erase(std::remove(e.begin(), e.end(), name), e.end());
    refresh();
}

// The row picked a sequence, or a ladder, for the lane.
void GelWizard::sequenceChosen()
{
    if (m_closing)
        return;
    const guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(m_sequence));
    Lane &lane = m_lanes[size_t(m_selected)];
    if (i == GTK_INVALID_LIST_POSITION)
        return;
    if (i >= m_listed.size() + commonLadders().size()) {
        // The last item: leave the well empty.
        if (lane.empty)
            return;
        lane.empty = true;
        lane.ladder = -1;
        lane.enzymes.clear();
    } else if (i >= m_listed.size()) {
        const int ladder = int(i - m_listed.size());
        if (lane.ladder == ladder)
            return;
        lane.ladder = g_ladder = ladder;
        lane.empty = false;
        lane.name = ladderAt(ladder).name;
        lane.enzymes.clear();
    } else {
        const GelSource &s = m_listed[i];
        if (s.doc == lane.doc && lane.isSample())
            return;
        // The enzymes stay: the same digest of another sequence (one that
        // doesn't cut it leaves it uncut).
        lane.doc = s.doc;
        lane.name = s.name;
        lane.seq = s.doc->sequence();
        lane.ladder = -1;
        lane.empty = false;
    }
    // Choosing from the list mustn't refill it while it's still open.
    if (!m_refreshIdle)
        m_refreshIdle = g_idle_add_once([](gpointer p) {
            auto *self = static_cast<GelWizard *>(p);
            self->m_refreshIdle = 0;
            self->refresh();
        }, this);
}

void GelWizard::runChanged()
{
    if (m_closing)
        return;
    const double level = gtk_range_get_value(GTK_RANGE(m_run));
    m_gel->setRunLevel(level);
    show3D();
    gtk_label_set_text(GTK_LABEL(m_runLabel),
                       level >= 0.995 ? "Full run" : (std::to_string(int(std::round(level * 100))) + "% run").c_str());
}

// The stain menu lists the Imager's stains on its tab, the 3D scenes on the other.
void GelWizard::fillStains()
{
    const bool filling = m_filling;
    m_filling = true;
    GtkStringList *model = m_look == 1
                               ? stringList({gel3DSceneTitle(Gel3DScene::PinkOnBlue), gel3DSceneTitle(Gel3DScene::GreenOnBlue),
                                             gel3DSceneTitle(Gel3DScene::BlackAndWhite)})
                               : stringList({gelStainTitle(GelStain::EthidiumUV), gelStainTitle(GelStain::SybrSafe),
                                             gelStainTitle(GelStain::BlackAndWhite)});
    gtk_drop_down_set_model(GTK_DROP_DOWN(m_stain), G_LIST_MODEL(model));
    g_object_unref(model);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(m_stain), m_look == 1 ? guint(g_scene3D) : guint(g_stain));
    m_filling = filling;
}

void GelWizard::stainChanged()
{
    if (m_closing)
        return;
    if (m_filling)
        return;
    const guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(m_stain));
    if (i == GTK_INVALID_LIST_POSITION)
        return;
    if (m_look == 1) {
        g_scene3D = Gel3DScene(i);
        if (m_gel3D)
            m_gel3D->setScene(g_scene3D);
        return;
    }
    g_stain = GelStain(i);
    m_gel->setStain(g_stain);
}

// Imager: the flat imager print, in the chosen stain. 3D: the gel on the
// light box, to turn around, in the chosen scene.
void GelWizard::lookChanged(int look)
{
    if (m_closing)
        return;
    if (look == m_look)
        return;
    m_look = look;
    fillStains();
    // Copy Image takes the Imager's print; the 3D scene isn't copied yet.
    gtk_widget_set_sensitive(m_copy, look != 1);
    if (m_tune)
        gtk_widget_set_visible(m_tune, look == 1);
    if (look == 1 && !m_gel3D) {
        m_gel3D = std::make_unique<Gel3DView>();
        m_gel3D->setScene(g_scene3D);
        m_gel3D->setLook(g_look3D);
        m_gel3D->setComb(gelCombs()[size_t(std::clamp(g_comb, 0, int(gelCombs().size()) - 1))]);
        gtk_stack_add_named(GTK_STACK(m_stage), m_gel3D->widget(), "3d");
    }
    show3D();
    gtk_stack_set_visible_child_name(GTK_STACK(m_stage), look == 1 ? "3d" : "imager");
}

// Sliders for the 3D picture's knobs, with their values, and a button that
// copies them all as text (to settle on the shipped values).
GtkWidget *GelWizard::buildTuner()
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    int row = 0;
    for (const Knob &k : kKnobs) {
        GtkWidget *name = gtk_label_new(k.name);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, k.lo, k.hi, k.step);
        gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
        gtk_widget_set_size_request(scale, 160, -1);
        gtk_range_set_value(GTK_RANGE(scale), g_look3D.*k.value);
        GtkWidget *value = gtk_label_new("");
        gtk_widget_add_css_class(value, "numeric");
        gtk_widget_add_css_class(value, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(value), 1);
        gtk_widget_set_size_request(value, 48, -1);
        auto show = [](GtkWidget *label, double v) {
            char buf[32];
            std::snprintf(buf, sizeof buf, v >= 10 ? "%.0f" : v >= 1 ? "%.2f" : "%.3f", v);
            gtk_label_set_text(GTK_LABEL(label), buf);
        };
        show(value, g_look3D.*k.value);
        g_object_set_data(G_OBJECT(scale), "oc-knob", const_cast<Knob *>(&k));
        g_object_set_data(G_OBJECT(scale), "oc-value", value);
        g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange *r, gpointer p) {
                             auto *self = static_cast<GelWizard *>(p);
                             auto *k = static_cast<const Knob *>(g_object_get_data(G_OBJECT(r), "oc-knob"));
                             auto *label = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(r), "oc-value"));
                             const double v = gtk_range_get_value(r);
                             g_look3D.*k->value = float(v);
                             char buf[32];
                             std::snprintf(buf, sizeof buf, v >= 10 ? "%.0f" : v >= 1 ? "%.2f" : "%.3f", v);
                             gtk_label_set_text(GTK_LABEL(label), buf);
                             if (self->m_gel3D)
                                 self->m_gel3D->setLook(g_look3D);
                         }),
                         this);
        gtk_grid_attach(GTK_GRID(grid), name, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), scale, 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), value, 2, row, 1, 1);
        ++row;
    }
    GtkWidget *copy = gtk_button_new_with_label("Copy Values");
    gtk_widget_set_tooltip_text(copy, "Put every value on the clipboard, to send");
    gtk_widget_set_halign(copy, GTK_ALIGN_END);
    gtk_widget_set_margin_top(copy, 6);
    g_signal_connect_swapped(copy, "clicked", G_CALLBACK(+[](gpointer p) { static_cast<GelWizard *>(p)->copyLook(); }),
                             this);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), grid);
    gtk_box_append(GTK_BOX(box), copy);
    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    return popover;
}

void GelWizard::copyLook()
{
    if (m_closing)
        return;
    std::string text = "3D gel look (" + std::string(gel3DSceneTitle(g_scene3D)) + ")\n";
    for (const Knob &k : kKnobs) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s: %g\n", k.name, double(g_look3D.*k.value));
        text += buf;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(m_dialog), text.c_str());
}

// The 3D gel follows the lanes and the run while it's showing.
void GelWizard::show3D()
{
    if (m_closing)
        return;
    if (m_gel3D && m_look == 1)
        m_gel3D->show(m_gel->lanes(), gtk_range_get_value(GTK_RANGE(m_run)), m_gel->dyeRuns());
}

void GelWizard::copyImage()
{
    if (m_closing)
        return;
    // What each lane holds, and how the gel was run, under the picture.
    std::vector<std::pair<std::string, std::string>> legend;
    const std::vector<std::string> names = laneNames();
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const Lane &lane = m_lanes[i];
        if (lane.empty)
            continue;
        legend.push_back({names[i], lane.isLadder() ? ladderAt(lane.ladder).name
                                                    : (lane.name.empty() ? std::string("Untitled") : lane.name) + " · "
                                                          + (lane.enzymes.empty() ? "uncut" : joined(lane.enzymes, " + "))});
    }
    std::string run = gtk_label_get_text(GTK_LABEL(m_runLabel));
    run = lower(run);
    const std::string caption = percentTitle(percent()) + " · "
                              + gelCombs()[size_t(std::clamp(g_comb, 0, int(gelCombs().size()) - 1))].name + " comb · "
                              + gelStainTitle(m_gel->stain()) + " · " + run;
    GBytes *png = m_gel->snapshotPng(legend, caption);
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
    gtk_button_set_icon_name(GTK_BUTTON(m_copy), "object-select-symbolic");
    if (m_copiedTimer)
        g_source_remove(m_copiedTimer);
    m_copiedTimer = g_timeout_add(1200, [](gpointer p) -> gboolean {
        auto *self = static_cast<GelWizard *>(p);
        self->m_copiedTimer = 0;
        gtk_button_set_icon_name(GTK_BUTTON(self->m_copy), "edit-copy-symbolic");
        return G_SOURCE_REMOVE;
    }, this);
}

} // namespace oc::gtk
