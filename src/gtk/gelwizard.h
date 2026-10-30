// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// The gel wizard (as on the Mac, #34): a dialog over the template's window.
// Lanes on the left (a ladder, then any open sequence, cut with any enzymes
// or left uncut), and the gel on the right as an imager shows it. Changing a
// lane runs it again. Hovering a band names it; clicking a fragment selects
// it in its sequence. Copy Image puts the gel on the clipboard.
class GelWizard {
public:
    // A gel for `templ`: uncut, and cut with the first enzyme on its map.
    // `sources` lists the sequences open now. It deletes itself when it
    // closes, calling onClosed first.
    GelWizard(Document *templ, GtkWidget *parent, const std::string &name,
              std::function<std::vector<GelSource>()> sources);
    void present();
    void close();
    // The template's sequence changed.
    void templateChanged();

    std::function<void()> onClosed;

private:
    struct Lane {
        Document *doc;      // compared with the open sequences, never used once it closed
        std::string name;
        Sequence seq;       // as last seen
        std::vector<std::string> enzymes;
        // What decides the bands: when it changes, the lane runs again.
        std::string key() const;
    };
    struct Fragment {
        int start = 0, length = 0;
        std::string caption;
    };

    ~GelWizard();
    GtkWidget *buildView();
    GtkWidget *buildSidebar();
    void refresh();
    void fillSidebar();
    std::vector<Fragment> fragmentsOf(const Lane &lane) const;
    void selectLane(int lane);
    void pickFragment(int lane, int index);
    void addLane();
    void removeLane();
    void addEnzyme(const std::string &typed);
    void removeEnzyme(const std::string &name);
    void sequenceChosen();
    void copyImage();
    Document *live(const Lane &lane) const;

    Document *m_template;
    std::function<std::vector<GelSource>()> m_sources;
    std::vector<GelSource> m_listed;   // in the sequence row, in its order
    std::vector<Lane> m_lanes;
    int m_selected = 1;                 // the lane being edited, as on the gel (0 is the ladder)
    std::vector<std::vector<Fragment>> m_fragments;
    std::vector<std::string> m_drawnKeys;
    bool m_filling = false;
    guint m_copiedTimer = 0, m_refreshIdle = 0;
    std::unique_ptr<GelView> m_gel;
    GtkWidget *m_parent, *m_dialog, *m_ladder, *m_laneList, *m_add, *m_remove, *m_laneGroup, *m_sequence;
    GtkWidget *m_enzymeEntry, *m_enzymeMenu, *m_chips, *m_enzymeNote, *m_fragmentGroup, *m_fragmentList;
    GtkWidget *m_uv, *m_blue, *m_copy;
};

} // namespace oc::gtk
