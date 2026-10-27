// SPDX-License-Identifier: GPL-3.0-or-later
// The open document shared by both frontends: the sequence, its cut sites,
// the enzyme filter, the selection and the active feature/site. Frontends
// subscribe to the change notifications (a plain-C++ stand-in for signals).
#pragma once

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

    // Text for a status bar: length, topology, selection, GC%, Tm.
    std::string statusText() const;

    void onSequenceChanged(Listener l) { m_seqListeners.push_back(std::move(l)); }
    void onSelectionChanged(Listener l) { m_selListeners.push_back(std::move(l)); }
    void onEnzymesChanged(Listener l) { m_enzListeners.push_back(std::move(l)); }

private:
    void recomputeSites();
    void refilter();
    static void emit(const std::vector<Listener> &ls);

    Sequence m_seq;
    std::string m_path;
    std::vector<Enzyme> m_enzymes;
    std::vector<CutSite> m_allSites;
    std::vector<CutSite> m_visibleSites;
    std::vector<int> m_countByEnzyme;
    int m_maxCuts = 1;
    std::set<std::string> m_hidden;
    Span m_selection;
    bool m_hasSelection = false;
    int m_activeFeature = -1;
    int m_activeSite = -1;
    std::vector<Listener> m_seqListeners, m_selListeners, m_enzListeners;
};

} // namespace oc
