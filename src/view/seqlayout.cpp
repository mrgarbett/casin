// SPDX-License-Identifier: GPL-3.0-or-later
#include "seqlayout.h"
#include "maplayout.h"   // withThousands

#include <algorithm>
#include <cmath>

namespace oc::view {

namespace {
const double kLinePadTop = 8;
const double kLinePadBottom = 10;
const int kMaxEnzymeRows = 6;
}

SequenceLayout::SequenceLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
                               TextMeasurer &tm, double width, bool showComplement)
    : m_seq(seq), m_sites(sites), m_theme(theme), m_complement(showComplement),
      m_n(seq.length()), m_width(width)
{
    m_charW = tm.width("ACGTACGTAC", m_baseFont) / 10.0;
    m_gap = std::round(m_charW * 0.8);
    m_baseH = std::ceil(tm.height(m_baseFont) + 4);
    m_labelH = std::ceil(tm.height(m_smallFont) + 2);
    m_aaH = std::ceil(tm.height(m_baseFont) + 1);
    const int digits = int(withThousands(std::max(1, m_n)).size());
    m_left = 10 + digits * tm.width("0", m_numFont) + 10;
    const double groupW = 10 * m_charW + m_gap;
    m_bpl = std::max(10, int((width - m_left - 12 + m_gap) / groupW) * 10);

    std::map<const Enzyme *, int> counts;
    for (const CutSite &s : m_sites)
        ++counts[s.enzyme];
    for (const CutSite &s : m_sites)
        m_cutCount.push_back(counts[s.enzyme]);

    for (size_t i = 0; i < m_seq.features.size(); ++i) {
        const Feature &f = m_seq.features[i];
        if (!FeatureStyle::showsTranslation(f))
            continue;
        std::vector<CodonMark> marks = codonMarks(m_seq, f);
        std::sort(marks.begin(), marks.end(),
                  [](const CodonMark &a, const CodonMark &b) { return a.position < b.position; });
        m_marks[int(i)] = std::move(marks);
    }

    double y = 0;
    for (int start = 0; start < m_n; start += m_bpl) {
        Line line;
        line.start = start;
        line.len = std::min(m_bpl, m_n - start);
        line.y = y;
        m_lines.push_back(line);
        y += 1;   // placeholder; real heights below
    }

    // Distribute cut sites and feature pieces to lines.
    std::vector<std::vector<int>> sitesByLine(m_lines.size());
    for (size_t i = 0; i < m_sites.size(); ++i) {
        const int c = m_sites[i].cutTop;
        if (c >= 0 && c < m_n)
            sitesByLine[size_t(c / m_bpl)].push_back(int(i));
    }
    for (size_t li = 0; li < m_lines.size(); ++li) {
        Line &line = m_lines[li];
        // Enzyme labels: one per cut position, stacked upward from the strand.
        std::vector<SiteLabel> labels;
        for (int si : sitesByLine[li]) {
            const CutSite &cs = m_sites[size_t(si)];
            if (!labels.empty() && m_sites[size_t(labels.back().sites.front())].cutTop == cs.cutTop) {
                labels.back().sites.push_back(si);
                labels.back().text += ", " + cs.enzyme->name;
            } else {
                SiteLabel l;
                l.sites = {si};
                l.text = cs.enzyme->name;
                labels.push_back(l);
            }
            labels.back().unique = labels.back().unique || m_cutCount[size_t(si)] == 1;
            labels.back().typeIIS = labels.back().typeIIS || cs.enzyme->cutsOutsideSite();
        }
        std::vector<double> rowEnd;
        for (SiteLabel &l : labels) {
            const int c = m_sites[size_t(l.sites.front())].cutTop - line.start;
            Font f = m_smallFont;
            f.bold = l.unique;
            l.w = tm.width(l.text, f);
            l.x = boundaryX(c);
            l.row = -1;
            for (size_t r = 0; r < rowEnd.size(); ++r)
                if (l.x > rowEnd[r] + 4) {
                    l.row = int(r);
                    break;
                }
            if (l.row < 0 && int(rowEnd.size()) < kMaxEnzymeRows) {
                rowEnd.push_back(-1e9);
                l.row = int(rowEnd.size()) - 1;
            }
            if (l.row < 0)
                continue;   // too crowded; still shown on the map and in the list
            rowEnd[size_t(l.row)] = l.x + l.w;
            line.labels.push_back(l);
        }
        line.enzymeRows = int(rowEnd.size());
    }

    // Feature pieces per line, lanes assigned greedily within each line.
    std::vector<std::vector<LanePiece>> piecesByLine(m_lines.size());
    for (size_t fi = 0; fi < m_seq.features.size(); ++fi) {
        const Feature &f = m_seq.features[fi];
        if (!FeatureStyle::isDrawn(f) || f.ranges.empty())
            continue;
        const Range &first = f.ranges.front(), &last = f.ranges.back();
        for (const Range &r : f.ranges) {
            if (r.length() <= 0)
                continue;
            for (int li = r.start / m_bpl; li < int(m_lines.size()) && m_lines[size_t(li)].start < r.end; ++li) {
                const Line &line = m_lines[size_t(li)];
                const int a = std::max(r.start, line.start) - line.start;
                const int b = std::min(r.end, line.start + line.len) - line.start;
                if (b <= a)
                    continue;
                bool head = false;
                if (f.strand == Strand::Forward && &r == &last && r.end == line.start + b)
                    head = true;
                if (f.strand == Strand::Reverse && &r == &first && r.start == line.start + a)
                    head = true;
                piecesByLine[size_t(li)].push_back({int(fi), 0, a, b, head});
            }
        }
    }
    y = 0;
    for (size_t li = 0; li < m_lines.size(); ++li) {
        Line &line = m_lines[li];
        auto &pieces = piecesByLine[li];
        std::stable_sort(pieces.begin(), pieces.end(), [](const LanePiece &x, const LanePiece &z) {
            return x.a < z.a || (x.a == z.a && x.b > z.b);
        });
        std::vector<int> laneEnd;
        std::vector<std::pair<int, int>> featureLane;   // a feature keeps one lane within a line
        for (LanePiece &p : pieces) {
            int lane = -1;
            for (auto &fl : featureLane)
                if (fl.first == p.feature)
                    lane = fl.second;
            if (lane < 0) {
                for (size_t l = 0; l < laneEnd.size(); ++l)
                    if (p.a >= laneEnd[l]) {
                        lane = int(l);
                        break;
                    }
                if (lane < 0) {
                    laneEnd.push_back(0);
                    lane = int(laneEnd.size()) - 1;
                }
                featureLane.push_back({p.feature, lane});
            }
            p.lane = lane;
            laneEnd[size_t(lane)] = std::max(laneEnd[size_t(lane)], p.b);
        }
        line.pieces = pieces;
        line.laneHasAA.assign(laneEnd.size(), false);
        for (const LanePiece &p : pieces)
            if (FeatureStyle::showsTranslation(m_seq.features[size_t(p.feature)]))
                line.laneHasAA[size_t(p.lane)] = true;

        line.y = y;
        double off = kLinePadTop + line.enzymeRows * m_labelH + (line.enzymeRows ? 3 : 0);
        line.yTop = off;
        off += m_baseH;
        line.yBottom = off;
        if (m_complement)
            off += m_baseH;
        line.yRuler = off;
        off += m_rulerH;
        line.yLanes = off + 2;
        off += 2;
        for (bool aa : line.laneHasAA)
            off += m_laneH + 2 + (aa ? m_aaH : 0);
        line.height = off + kLinePadBottom;
        y += line.height;
    }
    m_totalHeight = y;
}

double SequenceLayout::colX(int col) const
{
    return m_left + col * m_charW + (col / 10) * m_gap;
}

double SequenceLayout::boundaryX(int b) const
{
    if (b > 0 && b % 10 == 0)
        return m_left + b * m_charW + (b / 10 - 0.5) * m_gap;
    return colX(b);
}

double SequenceLayout::laneY(const Line &l, int lane) const
{
    double y = l.y + l.yLanes;
    for (int i = 0; i < lane; ++i)
        y += m_laneH + 2 + (l.laneHasAA[size_t(i)] ? m_aaH : 0);
    return y;
}

int SequenceLayout::lineIndexAt(double y) const
{
    if (m_lines.empty())
        return -1;
    auto it = std::upper_bound(m_lines.begin(), m_lines.end(), y,
                               [](double v, const Line &l) { return v < l.y; });
    if (it == m_lines.begin())
        return 0;
    return int(it - m_lines.begin()) - 1;
}

double SequenceLayout::lineTopFor(int pos) const
{
    if (m_lines.empty())
        return 0;
    const int li = std::clamp(pos / m_bpl, 0, int(m_lines.size()) - 1);
    return m_lines[size_t(li)].y;
}

double SequenceLayout::lineHeightFor(int pos) const
{
    if (m_lines.empty())
        return 0;
    const int li = std::clamp(pos / m_bpl, 0, int(m_lines.size()) - 1);
    return m_lines[size_t(li)].height;
}

int SequenceLayout::positionAt(Point p) const
{
    const int li = lineIndexAt(p.y);
    if (li < 0)
        return 0;
    const Line &l = m_lines[size_t(li)];
    const double groupW = 10 * m_charW + m_gap;
    const double x = p.x - m_left;
    if (x <= 0)
        return l.start;
    const int group = int(x / groupW);
    const double within = x - group * groupW;
    const int col = group * 10 + std::clamp(int(std::lround(within / m_charW)), 0, 10);
    return l.start + std::clamp(col, 0, l.len);
}

int SequenceLayout::featureAt(Point p) const
{
    const int li = lineIndexAt(p.y);
    if (li < 0)
        return -1;
    const Line &l = m_lines[size_t(li)];
    for (const LanePiece &pc : l.pieces) {
        const double y0 = laneY(l, pc.lane);
        const Rect r{boundaryX(pc.a), y0, boundaryX(pc.b) - boundaryX(pc.a), m_laneH};
        if (r.contains(p))
            return pc.feature;
    }
    return -1;
}

int SequenceLayout::siteAt(Point p) const
{
    const int li = lineIndexAt(p.y);
    if (li < 0)
        return -1;
    const Line &l = m_lines[size_t(li)];
    for (const SiteLabel &s : l.labels) {
        const double yRow = l.y + l.yTop - 3 - (s.row + 1) * m_labelH;
        if (Rect{s.x, yRow, s.w, m_labelH}.contains(p))
            return s.sites.front();
    }
    return -1;
}

std::string SequenceLayout::tooltipAt(Point p) const
{
    const int si = siteAt(p);
    if (si >= 0) {
        const CutSite &c = m_sites[size_t(si)];
        return c.enzyme->name + "  " + c.enzyme->site + "  cuts after " + withThousands(c.cutTop);
    }
    const int fi = featureAt(p);
    if (fi >= 0) {
        const Feature &f = m_seq.features[size_t(fi)];
        return f.name + " (" + f.type + "), " + formatBp(f.totalLength());
    }
    return {};
}

Scene SequenceLayout::render(double scrollY, double viewHeight, const SeqViewState &st) const
{
    Scene s;
    s.width = m_width;
    s.height = viewHeight;
    s.background = m_theme.background;
    if (m_lines.empty()) {
        s.label("No sequence", {m_width / 2, viewHeight / 2}, {13, false, false}, m_theme.mutedText,
                HAlign::Center, VAlign::Middle);
        return s;
    }

    // Selection as up to two linear pieces.
    std::vector<Range> sel;
    if (st.hasSelection && st.selection.length > 0) {
        const int a = st.selection.start, e = st.selection.end();
        if (e > m_n) {
            sel.push_back({a, m_n});
            sel.push_back({0, std::min(e - m_n, m_n)});
        } else {
            sel.push_back({a, e});
        }
    }
    const CutSite *active = (st.activeSite >= 0 && st.activeSite < int(m_sites.size()))
                              ? &m_sites[size_t(st.activeSite)] : nullptr;

    const double asc = m_baseFont.size * 0.78;
    for (int li = lineIndexAt(scrollY); li >= 0 && li < int(m_lines.size()); ++li) {
        const Line &l = m_lines[size_t(li)];
        if (l.y > scrollY + viewHeight)
            break;
        const double y0 = l.y - scrollY;
        const double top = y0 + l.yTop, bottom = y0 + l.yBottom;
        const double strandsEnd = y0 + l.yRuler;

        for (const Range &r : sel) {
            const int a = std::max(r.start, l.start) - l.start;
            const int b = std::min(r.end, l.start + l.len) - l.start;
            if (b > a)
                s.rect({boundaryX(a), top, boundaryX(b) - boundaryX(a), strandsEnd - top}, m_theme.selection);
        }

        // Position number.
        s.label(withThousands(l.start + 1), {m_left - 10, top + (m_baseH + asc) / 2 - 1}, m_numFont,
                m_theme.mutedText, HAlign::Right);

        // Bases, one text run per group of 10.
        const std::string bases = m_seq.bases.substr(size_t(l.start), size_t(l.len));
        const std::string comp = m_complement ? complement(bases) : std::string();
        for (int g = 0; g < l.len; g += 10) {
            const std::string chunk = bases.substr(size_t(g), 10);
            s.label(chunk, {colX(g), top + (m_baseH + asc) / 2 - 1}, m_baseFont, m_theme.text);
            if (m_complement)
                s.label(comp.substr(size_t(g), 10), {colX(g), bottom + (m_baseH + asc) / 2 - 1}, m_baseFont,
                        m_theme.mutedText);
        }

        // Ruler: a tick and number under every 10th base.
        const double ry = y0 + l.yRuler;
        s.line({colX(0), ry + 2}, {boundaryX(l.len), ry + 2}, m_theme.tick.withAlpha(0.6), 0.8);
        for (int c = 9; c < l.len; c += 10) {
            const double x = colX(c) + m_charW / 2;
            s.line({x, ry + 2}, {x, ry + 5}, m_theme.tick, 1);
            s.label(withThousands(l.start + c + 1), {x, ry + 6}, m_smallFont, m_theme.mutedText, HAlign::Center,
                    VAlign::Top);
        }

        // Enzyme labels with a short tick down to the strand.
        for (const SiteLabel &lab : l.labels) {
            const bool isActive = active && std::find(lab.sites.begin(), lab.sites.end(), st.activeSite) != lab.sites.end();
            const double yRow = top - 3 - (lab.row + 1) * m_labelH;
            Font f = m_smallFont;
            f.bold = lab.unique;
            const Color c = isActive ? m_theme.cutMarker
                          : lab.unique ? m_theme.enzymeUnique
                          : lab.typeIIS ? m_theme.enzymeTypeIIS
                                        : m_theme.enzymeLabel;
            s.line({lab.x, yRow + m_labelH - 1}, {lab.x, top}, m_theme.leader, 0.8);
            s.label(lab.text, {lab.x + 2, yRow + m_labelH / 2}, f, c, HAlign::Left, VAlign::Middle);
        }

        // Cut marker for the active site.
        if (active) {
            const int ct = active->cutTop - l.start, cb = active->cutBottom - l.start;
            const double mid = m_complement ? bottom : bottom - 2;
            const double low = m_complement ? bottom + m_baseH : bottom;
            const bool topIn = ct >= 0 && ct <= l.len, botIn = cb >= 0 && cb <= l.len;
            if (topIn)
                s.line({boundaryX(ct), top}, {boundaryX(ct), mid}, m_theme.cutMarker, 2);
            if (botIn)
                s.line({boundaryX(cb), mid}, {boundaryX(cb), low}, m_theme.cutMarker, 2);
            if (topIn || botIn) {
                const double xa = topIn ? boundaryX(ct) : (ct < 0 ? boundaryX(0) : boundaryX(l.len));
                const double xb = botIn ? boundaryX(cb) : (cb < 0 ? boundaryX(0) : boundaryX(l.len));
                if (std::fabs(xa - xb) > 0.5)
                    s.line({xa, mid}, {xb, mid}, m_theme.cutMarker, 2);
            }
        }

        // Feature lanes and translations.
        for (const LanePiece &pc : l.pieces) {
            const Feature &f = m_seq.features[size_t(pc.feature)];
            const Color fill = FeatureStyle::fill(f.type, f.color);
            const double ly = laneY(l, pc.lane) - scrollY;
            const double x0 = boundaryX(pc.a) + 0.5, x1 = boundaryX(pc.b) - 0.5;
            const double ym = ly + m_laneH / 2;
            const double head = pc.head ? std::min(m_laneH * 0.6, (x1 - x0) * 0.5) : 0;
            std::vector<Point> poly;
            if (pc.head && f.strand == Strand::Forward)
                poly = {{x0, ly}, {x1 - head, ly}, {x1, ym}, {x1 - head, ly + m_laneH}, {x0, ly + m_laneH}};
            else if (pc.head && f.strand == Strand::Reverse)
                poly = {{x0, ym}, {x0 + head, ly}, {x1, ly}, {x1, ly + m_laneH}, {x0 + head, ly + m_laneH}};
            else
                poly = {{x0, ly}, {x1, ly}, {x1, ly + m_laneH}, {x0, ly + m_laneH}};
            const bool isActive = pc.feature == st.activeFeature;
            s.polygon(poly, fill, isActive ? m_theme.accent : m_theme.featureOutline, isActive ? 2 : 0.6);
            Font nf{10, true, false};
            const double avail = x1 - x0 - head - 6;
            if (avail > 20) {
                std::string name = f.name;
                ApproxTextMeasurer approx;
                while (!name.empty() && approx.width(name, nf) > avail)
                    name.pop_back();
                if (name.size() < f.name.size() && name.size() > 2)
                    name = name.substr(0, name.size() - 1) + "…";
                if (!name.empty() && name.size() >= 2) {
                    const double nx = (f.strand == Strand::Reverse && pc.head) ? x0 + head + 3 : x0 + 3;
                    s.label(name, {nx, ym}, nf, FeatureStyle::textOn(fill), HAlign::Left, VAlign::Middle);
                }
            }

            auto mi = m_marks.find(pc.feature);
            if (mi != m_marks.end()) {
                const auto &marks = mi->second;
                const double ay = ly + m_laneH + 1;
                auto it = std::lower_bound(marks.begin(), marks.end(), l.start + pc.a,
                                           [](const CodonMark &m, int v) { return m.position < v; });
                int k = int(it - marks.begin());
                for (; it != marks.end() && it->position < l.start + pc.b; ++it, ++k) {
                    const int c = it->position - l.start;
                    const double cx = colX(c) + m_charW / 2;
                    if (k % 2 == 0)
                        s.rect({cx - m_charW * 1.5, ay, m_charW * 3, m_aaH - 1}, fill.withAlpha(0.18));
                    const Color ac = it->aminoAcid == '*' ? m_theme.cutMarker : m_theme.text;
                    s.label(std::string(1, it->aminoAcid), {cx, ay + (m_aaH - 1) / 2}, m_baseFont, ac,
                            HAlign::Center, VAlign::Middle);
                }
            }
        }

        // Caret.
        if (st.hasSelection && st.selection.length == 0) {
            const int c = st.selection.start - l.start;
            if (c >= 0 && (c < l.len || (c == l.len && li + 1 == int(m_lines.size()))))
                s.line({boundaryX(c), top - 1}, {boundaryX(c), strandsEnd}, m_theme.selectionEdge, 1.5);
        }
    }
    return s;
}

} // namespace oc::view
