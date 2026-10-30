// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.h"
#include "maplayout.h"   // withThousands
#include "../core/edit.h"
#include "../core/featurelib.h"
#include "../core/tm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace oc {

Document::Document() : m_enzymes(commonEnzymes()) {}

void Document::emit(const std::vector<Listener> &ls)
{
    for (const Listener &l : ls)
        l();
}

void Document::setPendingPrimer(const Primer &primer)
{
    m_display = m_seq;
    m_display.primers.push_back(primer);
    m_hasPending = true;
    emit(m_pendingListeners);
}

void Document::clearPendingPrimer()
{
    if (!m_hasPending)
        return;
    m_hasPending = false;   // m_display stays until replaced: layouts may still hold it
    emit(m_pendingListeners);
}

void Document::setSequence(Sequence seq, std::string path)
{
    m_hasPending = false;
    m_seq = std::move(seq);
    m_path = std::move(path);
    m_hasSelection = false;
    m_selection = {};
    m_activeFeature = -1;
    m_activeSite = -1;
    m_undo.clear();
    m_redo.clear();
    m_typing = false;
    m_reads.clear();
    m_state = m_savedState = ++m_nextState;
    recomputeSites();
    emit(m_seqListeners);
    emit(m_enzListeners);
    emit(m_selListeners);
    emit(m_histListeners);
}

void Document::recomputeSites()
{
    m_allSites = findSites(m_seq, m_enzymes);
    m_countByEnzyme.assign(m_enzymes.size(), 0);
    for (const CutSite &s : m_allSites)
        ++m_countByEnzyme[size_t(s.enzyme - m_enzymes.data())];
    refilter();
}

int Document::cutCount(const std::string &enzyme) const
{
    for (size_t i = 0; i < m_enzymes.size(); ++i)
        if (m_enzymes[i].name == enzyme)
            return i < m_countByEnzyme.size() ? m_countByEnzyme[i] : 0;
    return 0;
}

bool Document::enzymePassesFilter(const std::string &name) const
{
    const int c = cutCount(name);
    return c > 0 && (m_maxCuts <= 0 || c <= m_maxCuts);
}

void Document::refilter()
{
    m_visibleSites.clear();
    for (const CutSite &s : m_allSites) {
        const int c = m_countByEnzyme[size_t(s.enzyme - m_enzymes.data())];
        if ((m_maxCuts > 0 && c > m_maxCuts) || m_hidden.count(s.enzyme->name))
            continue;
        m_visibleSites.push_back(s);
    }
    m_activeSite = -1;
}

void Document::setMaxCuts(int maxCuts)
{
    if (maxCuts == m_maxCuts)
        return;
    m_maxCuts = maxCuts;
    refilter();
    emit(m_enzListeners);
}

void Document::setEnzymeHidden(const std::string &name, bool hidden)
{
    if (hidden == isEnzymeHidden(name))
        return;
    if (hidden)
        m_hidden.insert(name);
    else
        m_hidden.erase(name);
    refilter();
    emit(m_enzListeners);
}

void Document::showOnlyEnzymes(const std::set<std::string> &names, int maxCuts)
{
    m_maxCuts = maxCuts;
    m_hidden.clear();
    for (const Enzyme &e : m_enzymes)
        if (!names.count(e.name))
            m_hidden.insert(e.name);
    refilter();
    emit(m_enzListeners);
}

void Document::setSelection(Span s)
{
    m_typing = false;
    const int n = m_seq.length();
    if (n == 0)
        return;
    s.length = std::max(0, std::min(s.length, n));
    s.start = m_seq.circular ? ((s.start % n) + n) % n : std::max(0, std::min(s.start, n));
    if (!m_seq.circular)
        s.length = std::min(s.length, n - s.start);
    m_selection = s;
    m_hasSelection = true;
    m_activeFeature = -1;
    m_activeSite = -1;
    emit(m_selListeners);
}

void Document::clearSelection()
{
    m_typing = false;
    m_hasSelection = false;
    m_selection = {};
    m_activeFeature = -1;
    m_activeSite = -1;
    emit(m_selListeners);
}

std::string Document::selectedBases() const
{
    if (!m_hasSelection || m_selection.length <= 0)
        return {};
    return m_seq.slice(m_selection);
}

void Document::activateFeature(int index)
{
    m_typing = false;
    if (index < 0 || index >= int(m_seq.features.size()))
        return;
    m_selection = m_seq.features[size_t(index)].span(m_seq.length(), m_seq.circular);
    m_hasSelection = true;
    m_activeFeature = index;
    m_activeSite = -1;
    emit(m_selListeners);
}

void Document::activateSite(int visibleIndex)
{
    m_typing = false;
    if (visibleIndex < 0 || visibleIndex >= int(m_visibleSites.size()))
        return;
    const CutSite &s = m_visibleSites[size_t(visibleIndex)];
    m_selection = {s.matchStart, s.enzyme->siteLength()};
    m_hasSelection = true;
    m_activeFeature = -1;
    m_activeSite = visibleIndex;
    emit(m_selListeners);
}

int Document::findNext(const std::string &raw, int *count, Strand *strand)
{
    std::string query;
    for (char c : raw)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            query.push_back(c);
    if (count)
        *count = 0;
    if (query.empty() || cleanBases(query).size() != query.size())
        return -1;
    const std::vector<PatternMatch> matches = findPattern(m_seq, query);
    if (count)
        *count = int(matches.size());
    if (matches.empty())
        return 0;
    const int from = m_hasSelection ? m_selection.start : -1;
    size_t hit = 0;
    for (size_t i = 0; i < matches.size(); ++i)
        if (matches[i].start > from) {
            hit = i;
            break;
        }
    if (strand)
        *strand = matches[hit].strand;
    setSelection({matches[hit].start, matches[hit].length});
    return int(hit) + 1;
}

// ---- editing ---------------------------------------------------------------

namespace {
constexpr size_t kMaxUndo = 500;
}

Document::Step Document::snapshot(const std::string &label) const
{
    return {label, m_seq, m_selection, m_hasSelection, m_activeFeature, m_state};
}

void Document::restore(Step &step)
{
    m_seq = std::move(step.seq);
    m_selection = step.selection;
    m_hasSelection = step.hasSelection;
    m_state = step.state;
    sequenceEdited();
    m_activeFeature = step.activeFeature < int(m_seq.features.size()) ? step.activeFeature : -1;
    emit(m_selListeners);
    emit(m_histListeners);
}

void Document::addRead(ReadTrack read)
{
    // Aligning the same read again replaces it.
    for (ReadTrack &r : m_reads)
        if (r.name == read.name) {
            r = std::move(read);
            emit(m_readListeners);
            return;
        }
    m_reads.push_back(std::move(read));
    emit(m_readListeners);
}

void Document::clearReads()
{
    if (m_reads.empty())
        return;
    m_reads.clear();
    emit(m_readListeners);
}

void Document::sequenceEdited()
{
    m_hasPending = false;
    m_reads.clear();
    m_activeFeature = -1;
    m_activeSite = -1;
    recomputeSites();
    m_inEdit = true;
    emit(m_seqListeners);
    emit(m_enzListeners);
    m_inEdit = false;
}

void Document::edit(const std::string &label, const std::function<bool(Sequence &)> &change,
                    const std::function<Span()> &after, bool merge)
{
    merge = merge && m_typing && !m_undo.empty();
    m_typing = false;
    Step before = merge ? Step{} : snapshot(label);
    if (!change(m_seq))
        return;
    if (!merge) {
        m_undo.push_back(std::move(before));
        if (m_undo.size() > kMaxUndo)
            m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_state = ++m_nextState;
    const int n = m_seq.length();
    Span sel = after ? after() : Span{};
    if (n == 0) {
        m_hasSelection = false;
        m_selection = {};
    } else {
        sel.length = std::clamp(sel.length, 0, n);
        sel.start = m_seq.circular ? ((sel.start % n) + n) % n : std::clamp(sel.start, 0, n);
        if (!m_seq.circular)
            sel.length = std::min(sel.length, n - sel.start);
        m_selection = sel;
        m_hasSelection = true;
    }
    sequenceEdited();
    emit(m_selListeners);
    emit(m_histListeners);
}

void Document::undo()
{
    m_typing = false;
    if (m_undo.empty())
        return;
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(snapshot(step.label));
    restore(step);
}

void Document::redo()
{
    if (m_redo.empty())
        return;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back(snapshot(step.label));
    restore(step);
}

void Document::markSaved()
{
    m_savedState = m_state;
    emit(m_histListeners);
}

void Document::markUnsaved()
{
    m_savedState = 0;   // states start at 1
    emit(m_histListeners);
}

bool Document::typeBases(const std::string &text)
{
    std::string bases;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u) || std::isdigit(u))
            continue;
        if (!std::strchr("ACGTacgt", c))
            return false;
        bases.push_back(c);
    }
    if (bases.empty())
        return false;
    const Span sel = m_hasSelection ? m_selection : Span{m_seq.length(), 0};
    int at = 0;
    edit(sel.length > 0 ? "Replace Bases" : "Typing",
         [&](Sequence &s) {
             at = replaceBases(s, sel, bases);
             return true;
         },
         [&] { return Span{at + int(bases.size()), 0}; }, sel.length == 0);
    m_typing = true;
    return true;
}

Sequence Document::selectedFragment() const
{
    if (!m_hasSelection || m_selection.length <= 0)
        return {};
    return extractFragment(m_seq, m_selection);
}

bool Document::pasteFragment(const Sequence &fragment)
{
    if (fragment.bases.empty())
        return false;
    const Span sel = m_hasSelection ? m_selection : Span{m_seq.length(), 0};
    int at = 0;
    edit("Paste",
         [&](Sequence &s) {
             at = insertFragment(s, sel, fragment);
             return true;
         },
         [&] { return Span{at, int(fragment.bases.size())}; });
    return true;
}

void Document::deleteSelection()
{
    if (!m_hasSelection || m_selection.length <= 0)
        return;
    const Span sel = m_selection;
    int at = 0;
    edit("Delete Bases",
         [&](Sequence &s) {
             at = replaceBases(s, sel, {});
             return true;
         },
         [&] { return Span{at, 0}; });
}

void Document::deleteBackward()
{
    if (m_hasSelection && m_selection.length > 0)
        return deleteSelection();
    if (!m_hasSelection || m_seq.empty())
        return;
    const int pos = m_selection.start;
    if (pos == 0 && !m_seq.circular)
        return;
    const int n = m_seq.length();
    const Span del{(pos - 1 + n) % n, 1};
    edit("Delete Bases",
         [&](Sequence &s) {
             deleteBases(s, del);
             return true;
         },
         [&] { return Span{del.start, 0}; });
}

void Document::deleteForward()
{
    if (m_hasSelection && m_selection.length > 0)
        return deleteSelection();
    if (!m_hasSelection || m_seq.empty())
        return;
    const int n = m_seq.length();
    int pos = m_selection.start;
    if (pos >= n) {
        if (!m_seq.circular)
            return;
        pos = 0;
    }
    edit("Delete Bases",
         [&](Sequence &s) {
             deleteBases(s, {pos, 1});
             return true;
         },
         [&] { return Span{pos, 0}; });
}

void Document::reverseComplementAll()
{
    const int n = m_seq.length();
    const Span sel = m_selection;
    const bool had = m_hasSelection;
    edit("Reverse Complement",
         [](Sequence &s) {
             reverseComplementSequence(s);
             return !s.empty();
         },
         [&] { return had ? Span{((n - sel.start - sel.length) % n + n) % n, sel.length} : Span{0, 0}; });
}

void Document::setOriginAtSelection()
{
    if (!m_seq.circular || !m_hasSelection)
        return;
    const int origin = m_selection.start;
    const int len = m_selection.length;
    edit("Set Origin",
         [&](Sequence &s) {
             rotateOrigin(s, origin);
             return origin % std::max(1, s.length()) != 0;
         },
         [&] { return Span{0, len}; });
}

void Document::setCircular(bool circular)
{
    if (circular == m_seq.circular || m_seq.empty())
        return;
    const Span sel = m_selection;
    edit(circular ? "Make Circular" : "Make Linear",
         [&](Sequence &s) {
             s.circular = circular;
             return true;
         },
         [&] { return sel; });
}

void Document::rename(const std::string &name)
{
    const std::string clean = trim(name);
    if (clean.empty() || clean == m_seq.name)
        return;
    const Span sel = m_selection;
    edit("Rename",
         [&](Sequence &s) {
             s.name = clean;
             return true;
         },
         [&] { return sel; });
}

int Document::addFeature(const Feature &f)
{
    if (f.ranges.empty())
        return -1;
    const Span sel = m_selection;
    edit("Add Feature",
         [&](Sequence &s) {
             s.features.push_back(f);
             return true;
         },
         [&] { return sel; });
    const int index = int(m_seq.features.size()) - 1;
    activateFeature(index);
    return index;
}

void Document::replaceFeature(int index, const Feature &f)
{
    if (index < 0 || index >= int(m_seq.features.size()) || f.ranges.empty())
        return;
    const Span sel = m_selection;
    edit("Edit Feature",
         [&](Sequence &s) {
             s.features[size_t(index)] = f;
             return true;
         },
         [&] { return sel; });
    activateFeature(index);
}

void Document::removeFeature(int index)
{
    if (index < 0 || index >= int(m_seq.features.size()))
        return;
    const Span sel = m_selection;
    edit("Delete Feature",
         [&](Sequence &s) {
             s.features.erase(s.features.begin() + index);
             return true;
         },
         [&] { return sel; });
}

int Document::annotateCommonFeatures()
{
    const std::vector<Feature> found = findCommonFeatures(m_seq);
    if (found.empty())
        return 0;
    const Span sel = m_selection;
    edit("Annotate Common Features",
         [&](Sequence &s) {
             s.features.insert(s.features.end(), found.begin(), found.end());
             return true;
         },
         [&] { return sel; });
    return int(found.size());
}

int Document::addPrimerFromSelection(const std::string &name, Strand strand)
{
    if (!m_hasSelection || m_selection.length <= 0 || m_selection.wraps(m_seq.length()))
        return -1;
    Primer p;
    p.name = trim(name).empty() ? "Primer " + std::to_string(m_seq.primers.size() + 1) : trim(name);
    p.binding = {m_selection.start, m_selection.end()};
    p.strand = strand == Strand::Reverse ? Strand::Reverse : Strand::Forward;
    const std::string top = m_seq.bases.substr(size_t(p.binding.start), size_t(p.binding.length()));
    p.bases = p.strand == Strand::Reverse ? reverseComplement(top) : top;
    const Span sel = m_selection;
    edit("Add Primer",
         [&](Sequence &s) {
             s.primers.push_back(p);
             return true;
         },
         [&] { return sel; });
    return int(m_seq.primers.size()) - 1;
}

void Document::addPrimers(const std::vector<Primer> &primers, const std::string &label)
{
    if (primers.empty())
        return;
    const Span sel = m_selection;
    edit(label,
         [&](Sequence &s) {
             s.primers.insert(s.primers.end(), primers.begin(), primers.end());
             return true;
         },
         [&] { return sel; });
}

void Document::removePrimer(int index)
{
    if (index < 0 || index >= int(m_seq.primers.size()))
        return;
    const Span sel = m_selection;
    edit("Delete Primer",
         [&](Sequence &s) {
             s.primers.erase(s.primers.begin() + index);
             return true;
         },
         [&] { return sel; });
}

std::string Document::statusText(bool withLength) const
{
    if (m_seq.empty())
        return withLength ? "No sequence" : "";
    const int n = m_seq.length();
    std::string s = withLength ? view::formatBp(n) + "  ·  " + (m_seq.circular ? "circular" : "linear") : "";
    const std::string sep = s.empty() ? "" : "  ·  ";
    if (!m_hasSelection)
        return s;
    if (m_selection.length == 0) {
        s += sep + "position " + view::withThousands(m_selection.start);
        return s;
    }
    int end = m_selection.end();
    if (end > n)
        end -= n;
    s += sep + "selected " + view::withThousands(m_selection.start + 1)
       + (m_selection.length == 1 ? "" : "–" + view::withThousands(end)) + " (" + view::formatBp(m_selection.length)
       + ")";
    const std::string bases = selectedBases();
    char buf[64];
    std::snprintf(buf, sizeof buf, "  ·  GC %.1f%%", gcFraction(bases) * 100.0);
    s += buf;
    // Q5's Tm, as the primer editor and Primers pane show it (one Tm per
    // sequence, the one the PCR wizard uses). Nearest-neighbour Tm is for oligos.
    if (m_selection.length >= 8 && m_selection.length <= 100) {
        const int tm = polymeraseTm(bases, Polymerase::Q5);
        if (tm > 0) {
            std::snprintf(buf, sizeof buf, "  ·  Q5 Tm %d °C", tm);
            s += buf;
        }
    }
    return s;
}

} // namespace oc
