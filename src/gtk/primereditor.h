// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/primercheck.h"
#include "view/document.h"

#include <gtk/gtk.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace oc::gtk {

// Something inserted into a primer: a codon, an enzyme site, a tag or a
// protease site.
struct PrimerPart {
    enum class Kind { Codon, EnzymeSite, Tag, ProteaseSite };
    Kind kind = Kind::Codon;
    std::string name;    // shown on its mark: "Met", "EcoRI", "His6", "TEV"
    std::string bases;   // as it reads, 5'->3'
};

// Adding a primer, in the right panel's Primers pane (as on the Mac): its
// name, description, colour and bases, with codons, enzyme sites and tags to
// insert into it, each marked under the bases with a line showing which way
// it reads. While it binds, the window's map and sequence draw it where it
// would go; Add puts it on the document as one undo step.
class PrimerEditor {
public:
    // An editor for a new primer on `doc`, filled in with `bases` (5'->3') if given.
    PrimerEditor(Document *doc, const std::string &bases);
    ~PrimerEditor();
    GtkWidget *widget() const { return m_root; }
    // Replaces the bases, as when a new selection is sent here.
    void setBases(const std::string &bases);
    // Puts the cursor in the sequence box.
    void focus();
    // The document's sequence changed: find the site again, leaving the
    // selection where the edit put it.
    void sequenceChanged();
    // Called once the primer was added or the editor cancelled.
    std::function<void()> onClose;

    void accept();
    void cancel();

private:
    struct Mark {
        int start = 0, length = 0;   // in bases from the 5' end
        PrimerPart part;
        bool forward = true;
    };
    struct Snapshot {
        std::string text;
        std::vector<Mark> marks;
        bool flipped = false;
        int caret = 0;
    };

    GtkWidget *buildView();
    GtkWidget *insertRow(GtkWidget *popup, GtkWidget *second, void (*insert)(GtkButton *, gpointer));
    void refresh(bool select = true);
    std::string typed() const;
    std::string primerName() const;
    std::string hexColor() const;
    std::string nextPrimerName() const;
    std::string otherSites(const std::vector<PrimerSite> &sites, const PrimerSite &best, int tail) const;
    void checkFrames(const PrimerSite &site, int tail, std::vector<std::string> &lines,
                     std::vector<std::string> &warnings) const;
    std::optional<Span> checkPartner(const std::string &typed, const PrimerSite &site,
                                     std::vector<std::string> &lines, std::vector<std::string> &warnings) const;
    void fitToTm();
    void fillCodons();
    void insertPart(const PrimerPart &part);
    void flip();
    void close();

    // The sequence box: marks ride on text tags, so they move with edits.
    std::vector<Mark> marks() const;
    void setText(const std::string &text, const std::vector<Mark> &marks);
    void tagMark(const Mark &m);
    void replaceBases(int start, int length, const std::string &bases);
    Snapshot snapshot() const;
    void restore(const Snapshot &s);
    void noteUndo(bool typing);
    void undo();
    void redo();
    void tintThreePrime(bool good);
    static void drawMarks(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self);
    static void insertText(GtkTextBuffer *, GtkTextIter *at, const char *text, int len, gpointer self);
    static void deleteRange(GtkTextBuffer *, GtkTextIter *start, GtkTextIter *end, gpointer self);
    static gboolean boxKey(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer self);

    Document *m_doc;
    std::optional<PrimerSite> m_site;
    bool m_finished = false;
    Span m_selectionBefore{0, 0};
    bool m_hadSelection = false;
    std::optional<Span> m_selectionShown;

    GtkWidget *m_root = nullptr;
    GtkWidget *m_name, *m_description, *m_color, *m_text, *m_marks, *m_flip;
    GtkWidget *m_info, *m_warnings, *m_add, *m_tm, *m_partner, *m_aminoAcid, *m_codon, *m_enzyme, *m_tag, *m_pad;
    GtkTextBuffer *m_buffer;
    GtkTextTag *m_tintGood, *m_tintBad;
    bool m_colorChosen = false;
    int m_annealing = 0;
    std::vector<int> m_partnerIndex;      // the document primer behind each partner entry (after "None")
    std::vector<PrimerPart> m_enzymeParts, m_tagParts, m_codonParts;

    // The box's own undo, apart from the document's.
    std::vector<Snapshot> m_undo, m_redo;
    bool m_restoring = false;
    bool m_filtering = false;
    gint64 m_lastTyping = 0;
    bool m_fitRun = false;   // the last change was Fit, so the next one joins it
};

} // namespace oc::gtk
