// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mapview.h"
#include "seqview.h"
#include "view/document.h"

#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oc::gtk {

// The PCR wizard (as on the Mac, #31): a window with the reaction's inputs on
// the left (the two primers and the polymerase) and the product on the
// right, drawn as it will be: its map and sequence, tails and all. The
// template's window selects the stretch that gets amplified. Open Product
// opens the product as a new, unsaved sequence.
class PcrWizard {
public:
    // A wizard for `templ`, over `parent`, titled after `name`. It deletes
    // itself when its window closes, calling onClosed first.
    PcrWizard(Document *templ, GtkWindow *parent, const std::string &name);
    void present();
    void close();
    // New, changed or deleted primers on the template.
    void templateChanged();

    std::function<void()> onAddPrimer;
    std::function<void(const Sequence &)> onOpenProduct;
    std::function<void()> onClosed;

private:
    ~PcrWizard();
    struct Choice { int index; Range binding; };
    GtkWidget *buildView();
    void fillPrimers();
    void refresh();
    std::vector<Choice> m_forward, m_reverse;   // what each dropdown lists

    Document *m_template;
    Document m_preview;   // the product as drawn; refilled as the inputs change
    std::unique_ptr<MapView> m_map;
    std::unique_ptr<SequenceView> m_seq;
    bool m_hasProduct = false;
    bool m_filling = false;
    GtkWidget *m_window, *m_fwd, *m_rev, *m_polymerase, *m_info, *m_warnings, *m_program, *m_programRow;
    GtkWidget *m_productHeading, *m_open, *m_addPrimer, *m_stack, *m_empty, *m_split;
};

} // namespace oc::gtk
