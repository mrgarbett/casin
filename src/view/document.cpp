// SPDX-License-Identifier: GPL-3.0-or-later
#include "document.h"
#include "maplayout.h"   // withThousands
#include "../core/tm.h"

#include <cmath>
#include <cstdio>

namespace oc {

Document::Document() : m_enzymes(commonEnzymes()) {}

void Document::emit(const std::vector<Listener> &ls)
{
    for (const Listener &l : ls)
        l();
}

void Document::setSequence(Sequence seq, std::string path)
{
    m_seq = std::move(seq);
    m_path = std::move(path);
    m_hasSelection = false;
    m_selection = {};
    m_activeFeature = -1;
    m_activeSite = -1;
    recomputeSites();
    emit(m_seqListeners);
    emit(m_enzListeners);
    emit(m_selListeners);
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

void Document::setSelection(Span s)
{
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

std::string Document::statusText() const
{
    if (m_seq.empty())
        return "No sequence";
    const int n = m_seq.length();
    std::string s = view::formatBp(n) + "  ·  " + (m_seq.circular ? "circular" : "linear");
    if (!m_hasSelection)
        return s;
    if (m_selection.length == 0) {
        s += "  ·  position " + view::withThousands(m_selection.start);
        return s;
    }
    int end = m_selection.end();
    if (end > n)
        end -= n;
    s += "  ·  selected " + view::withThousands(m_selection.start + 1) + "–" + view::withThousands(end) + " ("
       + view::formatBp(m_selection.length) + ")";
    const std::string bases = selectedBases();
    char buf[64];
    std::snprintf(buf, sizeof buf, "  ·  GC %.1f%%", gcFraction(bases) * 100.0);
    s += buf;
    if (m_selection.length >= 8 && m_selection.length <= 100) {
        const double tm = meltingTemperature(bases);
        if (!std::isnan(tm)) {
            std::snprintf(buf, sizeof buf, "  ·  Tm %.1f °C", tm);
            s += buf;
        }
    }
    return s;
}

} // namespace oc
