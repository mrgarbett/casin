// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gel3dview.h"
#include "gelview.h"
#include "view/document.h"

#include <adwaita.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oc::gtk {

// A sequence open in a tab, as a lane's source.
struct GelSource {
    Document *doc;
    std::string name;
};

// The gel wizard (as on the Mac, #34 and #35): a dialog over the template's
// window. Lanes on the left (ladders and any open sequence, cut with any
// enzymes or left uncut, in any order), and the gel on the right as an
// imager shows it. Changing or moving a lane runs it again. Hovering a band
// names it; clicking a fragment selects it in its sequence. Copy Image puts
// the gel on the clipboard for a notebook.
class GelWizard {
public:
    // A gel for `templ`: as it was last left, or a ladder, the sequence
    // uncut, and cut with the first enzyme on its map. `sources` lists the
    // sequences open now. It deletes itself when it closes, calling onClosed first.
    GelWizard(Document *templ, GtkWidget *parent, const std::string &name,
              std::function<std::vector<GelSource>()> sources);
    void present();
    void close();
    // The template's sequence changed.
    void templateChanged();

    std::function<void()> onClosed;

    // What a lane holds; kept per sequence so a gel reopens as it was left.
    struct Lane {
        Document *doc;      // compared with the open sequences, never used once it closed
        std::string name;
        Sequence seq;       // as last seen
        std::vector<std::string> enzymes;
        int ladder = -1;    // which of the common ladders, or -1 for a sample
        bool empty = false; // nothing loaded: a well left free
        bool isLadder() const { return ladder >= 0; }
        bool isSample() const { return !isLadder() && !empty; }
        // What decides the bands: when it changes, the lane runs again.
        std::string key() const;
    };

private:
    struct Fragment {
        int start = 0, length = 0;
        std::string caption;
    };

    ~GelWizard();
    GtkWidget *buildView();
    GtkWidget *buildSidebar();
    void refresh();
    void fillSidebar();
    void fillSequences();
    std::vector<std::string> laneNames() const;
    std::vector<Fragment> fragmentsOf(const Lane &lane) const;
    double percent() const;
    void selectLane(int lane);
    void pickFragment(int lane, int index);
    void addLane();
    void removeLane();
    void moveLane(int step);
    void addEnzyme(const std::string &typed);
    void removeEnzyme(const std::string &name);
    void sequenceChosen();
    void runChanged();
    void stainChanged();
    void fillStains();
    void lookChanged(int look);
    void show3D();
    void copyImage();
    Document *live(const Lane &lane) const;

    Document *m_template;
    std::function<std::vector<GelSource>()> m_sources;
    std::vector<GelSource> m_listed;   // in the sequence row, in its order, before the ladders
    std::vector<Lane> m_lanes;         // as on the gel, ladders among them
    int m_selected = 0;                // the lane being edited
    std::vector<std::vector<Fragment>> m_fragments;
    std::vector<std::string> m_drawnKeys;
    bool m_filling = false;
    guint m_copiedTimer = 0, m_refreshIdle = 0;
    std::unique_ptr<GelView> m_gel;
    std::unique_ptr<Gel3DView> m_gel3D;   // made the first time the 3D tab opens
    int m_look = 0;                       // 0 the Imager, 1 3D (experimental)
    GtkWidget *m_parent, *m_dialog, *m_agarose, *m_comb, *m_laneList, *m_add, *m_remove, *m_left, *m_right;
    GtkWidget *m_laneGroup, *m_sequence, *m_enzymeEntry, *m_enzymeMenu, *m_chips, *m_enzymeNote;
    GtkWidget *m_fragmentGroup, *m_fragmentList, *m_stain, *m_run, *m_runLabel, *m_copy;
    GtkWidget *m_stage, *m_lookTabs[2] = {nullptr, nullptr};
};

} // namespace oc::gtk
