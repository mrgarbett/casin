// SPDX-License-Identifier: GPL-3.0-or-later
// The open document shared by both frontends: the sequence, its cut sites,
// the enzyme filter, the selection and the active feature/site. Frontends
// subscribe to the change notifications (a plain-C++ stand-in for signals).
#pragma once

#include "../core/align.h"
#include "../core/enzymes.h"
#include "../core/sequence.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace oc {

class Document {
public:
    using Listener = std::function<void()>;

    Document();
    Document(const Document &) = delete;   // cut sites point into m_enzymes
    Document &operator=(const Document &) = delete;

    bool isEmpty() const { return m_seq.empty(); }
    const Sequence &sequence() const { return m_seq; }
    const std::string &path() const { return m_path; }
    void setSequence(Sequence seq, std::string path = {});
    void setPath(std::string path) { m_path = std::move(path); }

    // ---- enzymes ----
    const std::vector<Enzyme> &enzymes() const { return m_enzymes; }
    const std::vector<CutSite> &allSites() const { return m_allSites; }
    // Sites that pass the filter and are not hidden. Indices into this list
    // are what activeSite() and the layouts use.
    const std::vector<CutSite> &visibleSites() const { return m_visibleSites; }
    int cutCount(const std::string &enzyme) const;
    // 1 = unique cutters, 2 = <= 2 cuts, 3 = <= 3 cuts, 0 = all cutters.
    int maxCuts() const { return m_maxCuts; }
    void setMaxCuts(int maxCuts);
    bool isEnzymeHidden(const std::string &name) const { return m_hidden.count(name) > 0; }
    void setEnzymeHidden(const std::string &name, bool hidden);
    bool enzymePassesFilter(const std::string &name) const;
    // Shows just these enzymes, with the cut filter set to `maxCuts`.
    void showOnlyEnzymes(const std::set<std::string> &names, int maxCuts);

    // ---- aligned reads ----
    // Reads shown under the sequence. They are a view, not part of the file,
    // and are dropped when the sequence changes.
    const std::vector<ReadTrack> &reads() const { return m_reads; }
    void addRead(ReadTrack read);
    void clearReads();

    // ---- selection ----
    bool hasSelection() const { return m_hasSelection; }
    Span selection() const { return m_selection; }   // may wrap; length 0 = caret
    void setSelection(Span s);
    void clearSelection();
    std::string selectedBases() const;
    int activeFeature() const { return m_activeFeature; }
    int activeSite() const { return m_activeSite; }
    // Selects the feature's footprint and makes it active.
    void activateFeature(int index);
    // Selects the site's recognition sequence and makes it active.
    void activateSite(int visibleIndex);

    // Searches both strands for `query` (IUPAC) and selects the first match
    // starting after the current selection start, wrapping around. Returns the
    // 1-based match number, 0 if nothing matched, -1 if the query isn't DNA.
    int findNext(const std::string &query, int *count = nullptr, Strand *strand = nullptr);

    // ---- editing ----
    // Every change to the sequence goes through these. Each one is a step on
    // the undo stack; the whole sequence is kept per step, which is cheap at
    // plasmid sizes.
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    std::string undoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
    std::string redoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }
    void undo();
    void redo();
    // True when the sequence differs from the last load or save.
    bool isModified() const { return m_state != m_savedState; }
    void markSaved();
    // For a new sequence that has never been saved.
    void markUnsaved();

    // Replaces the selection (or inserts at the caret) with `text`: A, C, G and
    // T in either case (lowercase stays lowercase), skipping spaces and digits.
    // Returns false, changing nothing, for any other letter or no bases at all.
    bool typeBases(const std::string &text);
    // The selection with the features and primers inside it.
    Sequence selectedFragment() const;
    // Like typeBases, but brings the fragment's features and primers along.
    bool pasteFragment(const Sequence &fragment);
    void deleteSelection();
    void deleteBackward();   // Backspace: the selection, or the base before the caret
    void deleteForward();    // Delete: the selection, or the base after the caret
    void reverseComplementAll();
    // Circular only: makes the selection start (or caret) base 1.
    void setOriginAtSelection();
    void setCircular(bool circular);
    void rename(const std::string &name);
    // Feature edits. addFeature returns the new feature's index.
    int addFeature(const Feature &f);
    void replaceFeature(int index, const Feature &f);
    void removeFeature(int index);
    // Adds features for the common elements found (see findCommonFeatures) as
    // one undo step. Returns how many were added.
    int annotateCommonFeatures();
    // Adds a primer that binds the selection on the given strand.
    int addPrimerFromSelection(const std::string &name, Strand strand);
    // Adds primers as one undo step, e.g. a designed pair.
    void addPrimers(const std::vector<Primer> &primers, const std::string &label);
    void removePrimer(int index);

    // Text for a status bar: length, topology, selection, GC%, Tm.
    // Length and topology, then the caret or selection (positions, length, GC, Tm).
    // Without `withLength`, only the caret or selection part (empty if none).
    std::string statusText(bool withLength = true) const;

    void onSequenceChanged(Listener l) { m_seqListeners.push_back(std::move(l)); }
    void onSelectionChanged(Listener l) { m_selListeners.push_back(std::move(l)); }
    void onEnzymesChanged(Listener l) { m_enzListeners.push_back(std::move(l)); }
    // Undo/redo availability or the modified flag changed.
    void onHistoryChanged(Listener l) { m_histListeners.push_back(std::move(l)); }
    // Reads were added or dropped.
    void onReadsChanged(Listener l) { m_readListeners.push_back(std::move(l)); }
    // True while the current sequence change came from an edit, not a load.
    bool isEditing() const { return m_inEdit; }

private:
    struct Step {
        std::string label;
        Sequence seq;
        Span selection;
        bool hasSelection;
        int activeFeature;
        unsigned state;
    };
    // Snapshots for undo, runs `change`, then selects `after` (a caret when
    // its length is 0) and notifies. `change` returns false to cancel.
    // With `merge`, the change joins the previous undo step instead (typing).
    void edit(const std::string &label, const std::function<bool(Sequence &)> &change,
              const std::function<Span()> &after, bool merge = false);
    Step snapshot(const std::string &label) const;
    void restore(Step &step);
    void sequenceEdited();
    void recomputeSites();
    void refilter();
    static void emit(const std::vector<Listener> &ls);

    Sequence m_seq;
    std::string m_path;
    std::vector<Enzyme> m_enzymes;
    std::vector<CutSite> m_allSites;
    std::vector<CutSite> m_visibleSites;
    std::vector<int> m_countByEnzyme;
    std::vector<ReadTrack> m_reads;
    int m_maxCuts = 1;
    std::set<std::string> m_hidden;
    Span m_selection;
    bool m_hasSelection = false;
    int m_activeFeature = -1;
    int m_activeSite = -1;
    std::vector<Step> m_undo, m_redo;
    unsigned m_state = 0, m_savedState = 0, m_nextState = 0;
    bool m_inEdit = false;
    bool m_typing = false;   // the last step was typed at the caret, so more typing joins it
    std::vector<Listener> m_seqListeners, m_selListeners, m_enzListeners, m_histListeners, m_readListeners;
};

} // namespace oc
