// SPDX-License-Identifier: GPL-3.0-or-later
#include "seqlayout.h"
#include "maplayout.h"   // withThousands

#include <algorithm>
#include <cctype>
#include <cmath>

namespace oc::view {

namespace {
const double kLinePadTop = 8;
const double kLinePadBottom = 10;
const int kMaxEnzymeRows = 6;
}

SequenceLayout::SequenceLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
                               TextMeasurer &tm, double width, bool showComplement, int frames,
                               std::vector<ReadTrack> reads)
    : m_seq(seq), m_sites(sites), m_theme(theme), m_complement(showComplement),
      m_frames(frames >= 6 ? 6 : frames >= 3 ? 3 : 0), m_reads(std::move(reads)),
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
    // Primer pieces, rows assigned greedily per side within each line.
    for (size_t pi = 0; pi < m_seq.primers.size(); ++pi) {
        const Primer &pr = m_seq.primers[pi];
        const Range r = pr.binding;
        if (r.length() <= 0 || r.end > m_n)
            continue;
        const bool fwd = pr.strand != Strand::Reverse;
        for (int li = r.start / m_bpl; li < int(m_lines.size()) && m_lines[size_t(li)].start < r.end; ++li) {
            Line &line = m_lines[size_t(li)];
            const int a = std::max(r.start, line.start) - line.start;
            const int b = std::min(r.end, line.start + line.len) - line.start;
            if (b <= a)
                continue;
            const bool head = fwd ? r.end == line.start + b : r.start == line.start + a;
            line.primers.push_back({int(pi), 0, a, b, head, fwd});
        }
    }
    for (Line &line : m_lines) {
        std::vector<int> endTop, endBottom;
        for (PrimerPiece &p : line.primers) {
            std::vector<int> &ends = p.top ? endTop : endBottom;
            size_t row = 0;
            while (row < ends.size() && p.a < ends[row] + 1)
                ++row;
            if (row == ends.size())
                ends.push_back(0);
            ends[row] = p.b;
            p.row = int(row);
        }
        line.primerRowsTop = int(endTop.size());
        line.primerRowsBottom = int(endBottom.size());
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
        line.yFramesTop = off;
        if (m_frames)
            off += 3 * m_aaH + 2;
        line.yPrimersTop = off;
        off += line.primerRowsTop * m_primerH;
        line.yTop = off;
        off += m_baseH;
        line.yBottom = off;
        if (m_complement)
            off += m_baseH;
        line.yPrimersBottom = off;
        off += line.primerRowsBottom * m_primerH;
        line.yFramesBottom = off;
        if (m_frames == 6)
            off += 2 + 3 * m_aaH;
        line.yRuler = off;
        off += m_rulerH;
        for (size_t r = 0; r < m_reads.size(); ++r)
            for (const Range &rr : readRanges(m_reads[r]))
                if (rr.start < line.start + line.len && line.start < rr.end) {
                    line.reads.push_back(int(r));
                    break;
                }
        line.yReads = off;
        off += line.reads.size() * (m_aaH + 1) + (line.reads.empty() ? 0 : 3);
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

Rect SequenceLayout::primerRect(const Line &l, const PrimerPiece &p) const
{
    const double y = p.top ? l.y + l.yTop - (p.row + 1) * m_primerH : l.y + l.yPrimersBottom + p.row * m_primerH;
    const double x0 = boundaryX(p.a), x1 = boundaryX(p.b);
    return {x0, y + 2, x1 - x0, m_primerH - 4};
}

int SequenceLayout::primerAt(Point p) const
{
    const int li = lineIndexAt(p.y);
    if (li < 0)
        return -1;
    const Line &l = m_lines[size_t(li)];
    for (const PrimerPiece &pc : l.primers)
        if (primerRect(l, pc).contains(p))
            return pc.primer;
    return -1;
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
        const double yRow = l.y + l.yFramesTop - 3 - (s.row + 1) * m_labelH;
        if (Rect{s.x, yRow, s.w, m_labelH}.contains(p))
            return s.sites.front();
    }
    return -1;
}

std::vector<Range> SequenceLayout::readRanges(const ReadTrack &r) const
{
    const int len = int(r.bases.size());
    const int start = m_n > 0 ? ((r.refSpan.start % m_n) + m_n) % m_n : 0;
    if (start + len <= m_n)
        return {{start, start + len}};
    return {{start, m_n}, {0, start + len - m_n}};
}

int SequenceLayout::readAt(Point p) const
{
    const int li = lineIndexAt(p.y);
    if (li < 0 || li >= int(m_lines.size()))
        return -1;
    const Line &l = m_lines[size_t(li)];
    const double dy = p.y - l.y - l.yReads;
    const int row = dy >= 0 ? int(dy / (m_aaH + 1)) : -1;
    return row >= 0 && row < int(l.reads.size()) ? l.reads[size_t(row)] : -1;
}

std::string SequenceLayout::tooltipAt(Point p) const
{
    const int ri = readAt(p);
    if (ri >= 0) {
        const ReadTrack &r = m_reads[size_t(ri)];
        int diffs = int(r.insertions.size());
        const int start = readRanges(r).front().start;
        for (size_t i = 0; i < r.bases.size(); ++i)
            if (std::toupper(static_cast<unsigned char>(r.bases[i]))
                != std::toupper(static_cast<unsigned char>(m_seq.bases[size_t((start + int(i)) % m_n)])))
                ++diffs;
        return r.name + " (read, " + (r.reverse ? "bottom" : "top") + " strand), " + formatBp(int(r.bases.size()))
             + ", " + std::to_string(diffs) + (diffs == 1 ? " difference" : " differences");
    }
    const int si = siteAt(p);
    if (si >= 0) {
        const CutSite &c = m_sites[size_t(si)];
        return c.enzyme->name + "  " + c.enzyme->site + "  cuts after " + withThousands(c.cutTop);
    }
    const int pi = primerAt(p);
    if (pi >= 0) {
        const Primer &pr = m_seq.primers[size_t(pi)];
        return pr.name + " (primer), " + std::to_string(pr.bases.size()) + " nt  " + pr.bases;
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

        // Reading-frame translations: +1..+3 count from base 1, -1..-3 from the last base.
        for (int row = 0; row < m_frames; ++row) {
            const bool fwd = row < 3;
            const int k = row % 3;
            const double fy = fwd ? y0 + l.yFramesTop + k * m_aaH : y0 + l.yFramesBottom + 2 + k * m_aaH;
            const std::string tag = (fwd ? "+" : "\u2212") + std::to_string(k + 1);
            s.label(tag, {m_left - 10, fy + (m_aaH - 1) / 2}, m_smallFont, m_theme.mutedText, HAlign::Right,
                    VAlign::Middle);
            // First codon whose middle base is on this line.
            int p = l.start - 1;
            const int phase = fwd ? k : ((m_n - k) % 3 + 3) % 3;
            p += ((phase - p) % 3 + 3) % 3;
            for (; p + 1 < l.start + l.len; p += 3) {
                if (p < 0 || p + 3 > m_n)
                    continue;
                std::string codon = m_seq.bases.substr(size_t(p), 3);
                if (!fwd)
                    codon = reverseComplement(codon);
                const char aa = translateCodon(codon.c_str());
                const double cx = colX(p + 1 - l.start) + m_charW / 2;
                if (aa == '*' || aa == 'M')
                    s.rect({cx - m_charW * 1.5, fy, m_charW * 3, m_aaH - 1},
                           (aa == '*' ? m_theme.cutMarker : m_theme.accent).withAlpha(0.16));
                const Color ac = aa == '*' ? m_theme.cutMarker : aa == 'M' ? m_theme.accent : m_theme.mutedText;
                s.label(std::string(1, aa), {cx, fy + (m_aaH - 1) / 2}, m_baseFont, ac, HAlign::Center,
                        VAlign::Middle);
            }
        }

        // Enzyme labels with a short tick down to the strand.
        for (const SiteLabel &lab : l.labels) {
            const bool isActive = active && std::find(lab.sites.begin(), lab.sites.end(), st.activeSite) != lab.sites.end();
            const double yRow = y0 + l.yFramesTop - 3 - (lab.row + 1) * m_labelH;
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

        // Primers: thin arrows pointing 5' to 3' on the strand they read.
        for (const PrimerPiece &pc : l.primers) {
            const Primer &pr = m_seq.primers[size_t(pc.primer)];
            Rect r = primerRect(l, pc);
            r.y -= scrollY;
            const double x0 = r.x + 0.5, x1 = r.x + r.w - 0.5, ym = r.y + r.h / 2;
            const double head = pc.head ? std::min(r.h * 0.8, (x1 - x0) * 0.5) : 0;
            std::vector<Point> poly;
            if (pc.head && pc.top)
                poly = {{x0, r.y}, {x1 - head, r.y}, {x1, ym}, {x1 - head, r.y + r.h}, {x0, r.y + r.h}};
            else if (pc.head)
                poly = {{x0, ym}, {x0 + head, r.y}, {x1, r.y}, {x1, r.y + r.h}, {x0 + head, r.y + r.h}};
            else
                poly = {{x0, r.y}, {x1, r.y}, {x1, r.y + r.h}, {x0, r.y + r.h}};
            s.polygon(poly, m_theme.primer, m_theme.featureOutline, 0.6);
            Font nf{9, true, false};
            ApproxTextMeasurer approx;
            const double nx = (!pc.top && pc.head) ? x0 + head + 2 : x0 + 2;
            if (approx.width(pr.name, nf) < x1 - x0 - head - 4)
                s.label(pr.name, {nx, ym}, nf, FeatureStyle::textOn(m_theme.primer), HAlign::Left, VAlign::Middle);
        }

        // Aligned reads: matching bases quiet, differences marked.
        for (size_t row = 0; row < l.reads.size(); ++row) {
            const ReadTrack &r = m_reads[size_t(l.reads[row])];
            const double ry0 = y0 + l.yReads + row * (m_aaH + 1);
            const double ym = ry0 + (m_aaH - 1) / 2;
            std::string tag = r.name;
            ApproxTextMeasurer approx;
            while (tag.size() > 1 && approx.width(tag, m_smallFont) > m_left - 14)
                tag.pop_back();
            s.label(tag, {m_left - 10, ym}, m_smallFont, m_theme.mutedText, HAlign::Right, VAlign::Middle);
            int offset = 0;
            for (const Range &rr : readRanges(r)) {
                const int a = std::max(rr.start, l.start), b = std::min(rr.end, l.start + l.len);
                if (b > a) {
                    s.rect({boundaryX(a - l.start), ry0, boundaryX(b - l.start) - boundaryX(a - l.start), m_aaH - 1},
                           m_theme.accent.withAlpha(0.08));
                    for (int pos = a; pos < b; ++pos) {
                        const char q = r.bases[size_t(offset + pos - rr.start)];
                        const char ref = m_seq.bases[size_t(pos)];
                        const bool same = std::toupper(static_cast<unsigned char>(q))
                                       == std::toupper(static_cast<unsigned char>(ref));
                        const double cx = colX(pos - l.start) + m_charW / 2;
                        if (!same)
                            s.rect({colX(pos - l.start), ry0, m_charW, m_aaH - 1}, m_theme.cutMarker.withAlpha(0.2));
                        s.label(std::string(1, q), {cx, ym}, m_baseFont, same ? m_theme.mutedText : m_theme.cutMarker,
                                HAlign::Center, VAlign::Middle);
                    }
                }
                offset += rr.length();
            }
            for (int ins : r.insertions)
                if (ins > l.start && ins < l.start + l.len) {
                    const double x = boundaryX(ins - l.start);
                    s.polygon({{x - 3, ry0}, {x + 3, ry0}, {x, ry0 + 4}}, m_theme.cutMarker);
                    s.line({x, ry0}, {x, ry0 + m_aaH - 1}, m_theme.cutMarker, 1.5);
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
