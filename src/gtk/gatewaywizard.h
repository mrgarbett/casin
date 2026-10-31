// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mapview.h"
#include "seqview.h"
#include "core/gateway.h"
#include "view/document.h"

#include <adwaita.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oc::gtk {

// What the wizard can recombine with: every sequence open now.
struct GatewaySource {
    Document *doc;
    std::string name;
};

// The Gateway wizard: a dialog over this tab's window, which dims behind it.
// This tab is the insert (the attB fragment for BP, the entry clone for LR);
// the vector is picked from the sequences open now. The sidebar says which
// att sites were found in each and what the reaction gives. The right side
// shows the insert's and the vector's maps, with their att cores marked, and
// the clone once there is one. Accept opens the clone as a new, unsaved
// sequence, with the byproduct beside it when asked.
class GatewayWizard {
public:
    GatewayWizard(Document *insert, GtkWidget *parent, const std::string &name,
                  std::function<std::vector<GatewaySource>()> sources);
    void present();
    void close();
    // The insert's sequence changed.
    void templateChanged();

    std::function<void(const Sequence &)> onOpenProduct;
    std::function<void()> onClosed;

private:
    ~GatewayWizard();
    GtkWidget *buildView();
    GtkWidget *buildSidebar();
    void fillVectors();
    void refresh();
    void setCue(const std::string &text, const char *icon, const char *css);
    void accept();
    Document *vectorDoc() const;
    GatewayReaction reaction() const;

    Document *m_insert;
    Document m_insertView;   // this tab, mirrored for the map
    Document m_vector;    // the picked vector, mirrored for the map
    Document m_preview;   // the clone as drawn
    std::unique_ptr<MapView> m_insertMap, m_vectorMap, m_map;
    std::unique_ptr<SequenceView> m_seq;
    std::function<std::vector<GatewaySource>()> m_sources;
    std::vector<Document *> m_vectors;   // the document behind each row entry
    Sequence m_clone, m_byproduct;
    bool m_hasClone = false;
    bool m_filling = false;
    bool m_closing = false;
    bool m_chosenReaction = false;   // the reaction was picked by hand, so leave it alone
    GtkWidget *m_parent;
    GtkWidget *m_dialog, *m_reaction, *m_vectorRow, *m_alsoByproduct;
    GtkWidget *m_cue, *m_cueIcon, *m_cueText;
    GtkWidget *m_sitesGroup, *m_sites, *m_resultGroup, *m_size, *m_info, *m_warnings;
    GtkWidget *m_accept, *m_stack, *m_split;
};

} // namespace oc::gtk
