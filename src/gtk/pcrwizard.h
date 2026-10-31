// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mapview.h"
#include "seqview.h"
#include "core/pcrprogram.h"
#include "view/document.h"

#include <adwaita.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oc::gtk {

// The PCR wizard (as on the Mac, #33): a dialog over the template's window,
// which dims behind it. The reaction's inputs are on the left: the two
// primers (picked from lists, clicked on the map, or typed), the polymerase,
// and the program NEB suggests. On the right are two tabs: Vector, the
// template's map, where primers can be clicked (shift-click for the second);
// and Product, once the pair makes one, drawn as it will be. The template
// selects the stretch that gets amplified. Open Product opens the product as
// a new, unsaved sequence.
class PcrWizard {
public:
    // A wizard for `templ`, over `parent` (a window), titled after `name`.
    // It deletes itself when it closes, calling onClosed first.
    PcrWizard(Document *templ, GtkWidget *parent, const std::string &name);
    void present();
    void close();
    // New, changed or deleted primers on the template.
    void templateChanged();

    std::function<void()> onAddPrimer;
    std::function<void(const Sequence &)> onOpenProduct;
    std::function<void()> onClosed;

private:
    ~PcrWizard();
    GtkWidget *buildView();
    GtkWidget *buildSidebar();
    void fillPrimers();
    void pick(int primer, bool adding);
    void addTyped();
    void refresh();
    void fillProgram(const PcrProgram &program);
    void setCue(const std::string &text, const char *icon, const char *css);
    void openProduct();
    int picked(GtkWidget *row, const std::vector<int> &list) const;

    Document *m_template;
    Document m_vector;    // the template, mirrored: the views never listen to the template itself
    Document m_preview;   // the product as drawn; refilled as the inputs change
    std::unique_ptr<MapView> m_vectorMap, m_map;
    std::unique_ptr<SequenceView> m_seq;
    std::vector<int> m_forward, m_reverse;   // the primer behind each row entry, after "Choose"
    bool m_hasProduct = false;
    bool m_filling = false;
    bool m_closing = false;   // the dialog is being destroyed: nothing more to do
    bool m_filled = false;
    std::string m_markdown;   // what Copy puts on the clipboard
    GtkWidget *m_parent;
    GtkWidget *m_dialog, *m_fwd, *m_rev, *m_typed, *m_typedNote, *m_polymerase, *m_cue, *m_cueIcon, *m_cueText;
    GtkWidget *m_productGroup, *m_size, *m_info, *m_warnings, *m_programGroup, *m_program, *m_cycles, *m_note;
    GtkWidget *m_open, *m_stack, *m_split, *m_toasts;
};

} // namespace oc::gtk
