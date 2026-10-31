// SPDX-License-Identifier: GPL-3.0-or-later
#include "maplayout.h"

#include "../core/orf.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace oc::view {

namespace {

const double kPi = 3.14159265358979323846;
const double kMargin = 14;
const double kLaneGap = 3;
const double kLinearPitch = 32;
const double kOrfPitch = 6;       // between ORF rows   // lane height + room for a name underneath

double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

int niceStep(int n, int targetTicks)
{
    if (n <= 0)
        return 1;
    const double raw = double(n) / targetTicks;
    double p = std::pow(10.0, std::floor(std::log10(std::max(raw, 1.0))));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (p * m >= raw)
            return std::max(1, int(p * m));
    return std::max(1, int(p * 10));
}

std::string tickText(int pos, int step)
{
    if (step >= 1000 && step % 1000 == 0)
        return std::to_string(pos / 1000) + " kb";
    return withThousands(pos);
}

} // namespace

std::string withThousands(int n)
{
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(size_t(i), ",");
    return n < 0 ? "-" + s : s;
}

std::string siteText(const Range &r, int n)
{
    return withThousands(r.start + 1) + "–" + withThousands(r.end > n && n > 0 ? r.end - n : r.end);
}

std::string formatBp(int n)
{
    return withThousands(n) + " bp";
}

// ---------------------------------------------------------------------------

MapLayout::MapLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
                     TextMeasurer &measure, double width, double height, MapOptions options)
    : m_seq(seq), m_sites(sites), m_theme(theme), m_options(std::move(options)), m_w(width), m_h(height),
      m_n(seq.length()), m_circular(seq.circular)
{
    m_ts = clampd(m_options.textScale, 0.5, 3);
    m_labelFont.size *= m_ts;
    std::map<const Enzyme *, int> counts;
    for (const CutSite &s : m_sites)
        ++counts[s.enzyme];
    for (const CutSite &s : m_sites) {
        m_cutCount.push_back(counts[s.enzyme]);
        m_methyl.push_back(methylationBlock(m_seq, s));
    }
    if (m_n == 0)
        return;
    findMapOrfs();
    if (m_circular)
        layoutCircular(measure);
    else
        layoutLinear(measure);
}

// ORFs for the translation toggle, as on the sequence: forward ones for 3
// frames, both strands for 6. Each strand's ORFs share rows where they don't
// overlap.
void MapLayout::findMapOrfs()
{
    if (m_options.frames != 3 && m_options.frames != 6)
        return;
    std::vector<std::vector<std::pair<int, int>>> used[2];   // per strand, per row: [start, end) spans
    for (const Orf &o : findOrfs(m_seq, std::max(1, m_options.minOrfCodons))) {
        const bool fwd = o.strand != Strand::Reverse;
        if (!fwd && m_options.frames != 6)
            continue;
        auto &rows = used[fwd ? 0 : 1];
        // A span past the origin counts as two pieces.
        std::vector<std::pair<int, int>> pieces;
        if (o.start + o.length > m_n) {
            pieces = {{o.start, m_n}, {0, o.start + o.length - m_n}};
        } else {
            pieces = {{o.start, o.start + o.length}};
        }
        size_t row = 0;
        for (; row < rows.size(); ++row) {
            bool clash = false;
            for (const auto &u : rows[row])
                for (const auto &p : pieces)
                    clash = clash || (p.first < u.second && u.first < p.second);
            if (!clash)
                break;
        }
        if (row >= 3)   // keep the band small; the sequence lists them all
            continue;
        if (row == rows.size())
            rows.emplace_back();
        rows[row].insert(rows[row].end(), pieces.begin(), pieces.end());
        m_orfs.push_back({o.start, o.length, fwd, int(row)});
        m_orfRows = std::max(m_orfRows, int(row) + 1);
    }
    if (m_orfRows > 0)
        m_orfBand = (m_options.frames == 6 ? 2 : 1) * m_orfRows * kOrfPitch + 6;
}

// ORF arrows: green forward ones, red reverse ones, each pointing the way it
// reads, in a band between the backbone and the ticks and their numbers.
void MapLayout::renderOrfs(Scene &s) const
{
    const double head = 6, w = 2.4, pitch = kOrfPitch;
    for (const OrfGlyph &o : m_orfs) {
        const Color c = o.forward ? m_theme.orfForward : m_theme.orfReverse;
        const int a = o.start, b = o.start + o.length;   // b may run past the origin
        if (m_circular) {
            const double r = m_r - 7 - (o.forward ? 0 : m_orfRows * pitch) - o.row * pitch;
            double a0 = angleOfPos(a), a1 = angleOfPos(b);
            if (a1 <= a0)
                a1 += 2 * kPi;
            const double ah = std::min(head / std::max(r, 1.0), (a1 - a0) / 2);
            PathItem arc;
            if (o.forward)
                arcPoints(arc.points, a0, a1 - ah, r);
            else
                arcPoints(arc.points, a0 + ah, a1, r);
            arc.stroke = c;
            arc.lineWidth = w;
            s.path(arc);
            const double tip = o.forward ? a1 : a0, base = o.forward ? a1 - ah : a0 + ah;
            s.polygon({polar(tip, r), polar(base, r - 3), polar(base, r + 3)}, c);
        } else {
            const double y = m_yb + 9 + (o.forward ? 0 : m_orfRows * pitch) + o.row * pitch;
            const double x0 = xOfPos(a), x1 = xOfPos(std::min(b, m_n));
            const double hh = std::min(head, (x1 - x0) / 2);
            if (o.forward) {
                s.line({x0, y}, {x1 - hh, y}, c, w);
                s.polygon({{x1, y}, {x1 - hh, y - 3}, {x1 - hh, y + 3}}, c);
            } else {
                s.line({x0 + hh, y}, {x1, y}, c, w);
                s.polygon({{x0, y}, {x0 + hh, y - 3}, {x0 + hh, y + 3}}, c);
            }
        }
    }
}

bool MapLayout::shows(const Feature &f) const
{
    return FeatureStyle::isDrawn(f) && !m_options.hiddenTypes.count(f.type);
}

Point MapLayout::polar(double angle, double r) const
{
    return {m_c.x + r * std::sin(angle), m_c.y - r * std::cos(angle)};
}

namespace {
// Whether a primer has a site to draw: on a circular sequence one across the
// origin runs past the end, up to a whole turn.
bool primerShown(const Primer &pr, const Sequence &seq)
{
    const int n = seq.length();
    return pr.binding.length() > 0 && pr.binding.end <= (seq.circular ? pr.binding.start + n : n);
}
} // namespace

double MapLayout::angleOfPos(double pos) const
{
    return 2 * kPi * pos / m_n;
}

double MapLayout::xOfPos(double pos) const
{
    return m_xl + (m_xr - m_xl) * pos / m_n;
}

void MapLayout::arcPoints(std::vector<Point> &out, double a0, double a1, double r) const
{
    const int steps = std::max(1, int(std::ceil(std::fabs(a1 - a0) / 0.015)));
    for (int i = 0; i <= steps; ++i)
        out.push_back(polar(a0 + (a1 - a0) * i / steps, r));
}

// Ranges merged into contiguous blocks; pieces that meet at the origin of a
// circular sequence form one block (which may wrap).
std::vector<std::vector<Span>> MapLayout::featureBlocks() const
{
    std::vector<std::vector<Span>> out;
    for (const Feature &f : m_seq.features) {
        std::vector<Span> blocks;
        for (const Range &r : f.ranges) {
            if (r.length() <= 0)
                continue;
            if (!blocks.empty()) {
                Span &b = blocks.back();
                const int bEnd = b.end() % m_n;
                if (bEnd == r.start % m_n && (m_circular || b.end() == r.start)) {
                    b.length += r.length();
                    continue;
                }
            }
            blocks.push_back({r.start, r.length()});
        }
        // First and last may meet across the origin too.
        if (m_circular && blocks.size() > 1 && blocks.back().end() == m_n && blocks.front().start == 0) {
            blocks.back().length += blocks.front().length;
            blocks.erase(blocks.begin());
        }
        // A site between two bases (GenBank's 5^6) has no length: it's drawn
        // as a tick where it lies.
        if (blocks.empty() && !f.ranges.empty())
            blocks.push_back({f.ranges.front().start, 0});
        out.push_back(blocks);
    }
    return out;
}

std::vector<int> MapLayout::assignLanes(const std::vector<int> &order, int *laneCount) const
{
    std::vector<int> lane(m_seq.features.size(), -1);
    std::vector<std::vector<Span>> lanes;
    const int pad = std::max(1, m_n / 300);
    for (int fi : order) {
        const Feature &f = m_seq.features[size_t(fi)];
        Span sp = f.span(m_n, m_circular);
        sp.start = m_circular ? ((sp.start - pad) % m_n + m_n) % m_n : sp.start - pad;
        sp.length = std::max(1, sp.length + 2 * pad);
        if (m_circular)
            sp.length = std::min(sp.length, m_n);
        int chosen = -1;
        for (size_t l = 0; l < lanes.size() && chosen < 0; ++l) {
            bool clash = false;
            for (const Span &o : lanes[l])
                if (m_circular ? sp.overlaps(o, m_n) : (sp.start < o.end() && o.start < sp.end())) {
                    clash = true;
                    break;
                }
            if (!clash)
                chosen = int(l);
        }
        if (chosen < 0) {
            if (lanes.size() >= 10) {
                chosen = int(lanes.size()) - 1;   // overflow: share the last lane
            } else {
                lanes.emplace_back();
                chosen = int(lanes.size()) - 1;
            }
        }
        lanes[size_t(chosen)].push_back(sp);
        lane[size_t(fi)] = chosen;
    }
    *laneCount = int(lanes.size());
    return lane;
}

void MapLayout::buildLabels(TextMeasurer &tm)
{
    // One label per distinct cut position.
    std::map<int, std::vector<int>> byPos;
    for (size_t i = 0; m_options.enzymes && i < m_sites.size(); ++i)
        byPos[m_sites[i].cutTop].push_back(int(i));
    for (auto &kv : byPos) {
        LabelGlyph g;
        g.sites = kv.second;
        int minCount = 1 << 30;
        bool typeIIS = false, blocked = true;
        for (int si : g.sites) {
            const CutSite &cs = m_sites[size_t(si)];
            if (!g.text.empty())
                g.text += ", ";
            g.text += cs.enzyme->name;
            minCount = std::min(minCount, m_cutCount[size_t(si)]);
            typeIIS = typeIIS || cs.enzyme->cutsOutsideSite();
            blocked = blocked && m_methyl[size_t(si)] != MethylNone;
        }
        g.font = m_labelFont;
        g.font.bold = minCount == 1;
        // Sites methylation blocks are greyed out.
        g.color = blocked       ? m_theme.mutedText
                : minCount == 1 ? m_theme.enzymeUnique
                : typeIIS       ? m_theme.enzymeTypeIIS
                                : m_theme.enzymeLabel;
        g.priority = minCount;
        const double w = tm.width(g.text, g.font);
        g.rect = {0, 0, w, tm.height(g.font)};
        m_labels.push_back(g);
    }
}

// ---- circular ----------------------------------------------------------------

void MapLayout::layoutCircular(TextMeasurer &tm)
{
    buildLabels(tm);
    double enzymeLabelW = 0;
    for (const LabelGlyph &g : m_labels)
        enzymeLabelW = std::max(enzymeLabelW, g.rect.w);
    const Font calloutFont{11 * m_ts, false, false};

    // Lanes: longest features first so short ones can stack outside them.
    std::vector<int> order;
    for (size_t i = 0; i < m_seq.features.size(); ++i)
        if (shows(m_seq.features[i]) && !m_seq.features[i].ranges.empty())
            order.push_back(int(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return m_seq.features[size_t(a)].span(m_n, true).length > m_seq.features[size_t(b)].span(m_n, true).length;
    });
    int laneCount = 0;
    const std::vector<int> lanes = assignLanes(order, &laneCount);

    // The label columns need room for the widest label. Without enzyme
    // labels, that is the feature and primer callouts: only features whose
    // names don't fit on their arrows get one, which depends on the radius.
    // (With enzyme labels, a longer callout is pulled inward to stay on the map.)
    auto columnWidth = [&](double r, double laneH) {
        double w = enzymeLabelW;
        if (m_labels.empty()) {
            for (int fi : order) {
                const Feature &f = m_seq.features[size_t(fi)];
                const double rMid = r + 6 + lanes[size_t(fi)] * (laneH + kLaneGap) + laneH / 2;
                const double arc = 2 * kPi * rMid * f.span(m_n, true).length / std::max(1, m_n) - laneH;
                const Font font{std::min(10.5 * m_ts, laneH * 0.72), true, false};
                if (tm.width(f.name, font) + 8 >= arc)
                    w = std::max(w, tm.width(f.name, calloutFont) + 12);
            }
            for (const Primer &pr : m_seq.primers)
                if (m_options.primers && pr.binding.length() > 0)
                    w = std::max(w, tm.width(pr.name, calloutFont) + 12);
        }
        // Callouts alone reserve no more than a typical enzyme label, so the
        // map doesn't shrink when the enzymes are hidden.
        // A narrow pane keeps most of its width for the circle; a longer
        // label is pulled inward to stay on the map.
        w = std::min({w, m_labels.empty() ? 70.0 : 160.0, m_w * 0.18});
        return w > 0 ? w + 26 : 0.0;
    };

    m_c = {m_w / 2, m_h / 2};
    double r = std::min(m_w, m_h) / 3;
    for (int iter = 0; iter < 4; ++iter) {
        m_laneH = clampd(r * 0.075, 9, 17);
        const double colW = columnWidth(r, m_laneH);
        const double extent = 6 + laneCount * (m_laneH + kLaneGap);
        const double horiz = m_w / 2 - kMargin - colW - 20 - extent;
        const double vert = m_h / 2 - kMargin - 22 - extent;
        r = std::max(40.0, std::min(horiz, vert));
    }
    m_r = r;
    m_featureOuter = m_r + 6 + laneCount * (m_laneH + kLaneGap);
    m_innerR = m_r * 0.62;

    // Ticks, inside the backbone, with labels kept clear of the name and
    // length in the centre (see renderCircular).
    const std::string centreName = m_seq.name.empty() ? "Untitled" : m_seq.name;
    const double nameSize = clampd(m_innerR / 7, 11, 18) * m_ts;
    const Font centreFont{nameSize, true, false}, lengthFont{nameSize * 0.72, false, false};
    const double centreW = std::max(tm.width(centreName, centreFont), tm.width(formatBp(m_n), lengthFont));
    const Rect centre{m_c.x - centreW / 2 - 4, m_c.y - 2 - tm.height(centreFont) - 2, centreW + 8,
                      tm.height(centreFont) + nameSize * 0.6 + 10 + tm.height(lengthFont)};
    const int step = niceStep(m_n, int(clampd(m_r / 14, 6, 24)));
    const Font tickFont{std::max(8.0, std::min(10.0, m_r / 14)) * m_ts, false, false};
    for (int p = 0; p < m_n; p += step) {
        const double a = angleOfPos(p);
        const double len = p == 0 ? 9 : 5;
        // Inside the backbone, past the ORF band (if any).
        m_ticks.push_back({polar(a, m_r - 1.5 - m_orfBand), polar(a, m_r - 1.5 - m_orfBand - len)});
        if (p == 0)
            continue;
        // Skip a label that would crowd the origin.
        if (m_n - p < step * 0.6)
            continue;
        TextItem t;
        t.text = tickText(p, step);
        t.font = tickFont;
        t.color = m_theme.mutedText;
        const double tw = tm.width(t.text, tickFont);
        const double th = tm.height(tickFont);
        // Push the label inward far enough that its box clears the ticks.
        const double extra = std::fabs(std::sin(a)) * tw / 2 + std::fabs(std::cos(a)) * th / 2;
        t.pos = polar(a, m_r - 10 - m_orfBand - extra);
        const Rect box{t.pos.x - tw / 2, t.pos.y - th / 2, tw, th};
        if (box.intersects(centre))
            continue;
        t.halign = HAlign::Center;
        t.valign = VAlign::Middle;
        m_tickLabels.push_back(t);
    }

    // Feature glyphs.
    const auto blocks = featureBlocks();
    for (int fi : order) {
        const Feature &f = m_seq.features[size_t(fi)];
        FeatureGlyph g;
        g.feature = fi;
        g.lane = lanes[size_t(fi)];
        g.fill = FeatureStyle::fill(f.type, f.color);
        g.text = FeatureStyle::textOn(g.fill);
        const double rIn = m_r + 6 + g.lane * (m_laneH + kLaneGap);
        const double rOut = rIn + m_laneH;
        const double rMid = (rIn + rOut) / 2;
        const std::vector<Span> &bl = blocks[size_t(fi)];
        if (bl.empty())
            continue;
        const double minA = 2.0 / rMid;
        double bestLen = -1;
        double textA0 = 0, textA1 = 0;
        for (size_t b = 0; b < bl.size(); ++b) {
            double a0 = angleOfPos(bl[b].start), a1 = angleOfPos(bl[b].end());
            if (a1 - a0 < minA) {
                const double mid = (a0 + a1) / 2;
                a0 = mid - minA / 2;
                a1 = mid + minA / 2;
            }
            const bool headHere = (f.strand == Strand::Forward && b + 1 == bl.size())
                               || (f.strand == Strand::Reverse && b == 0);
            const double head = headHere ? std::min(m_laneH * 0.8 / rMid, (a1 - a0) * 0.6) : 0;
            std::vector<Point> poly;
            if (headHere && f.strand == Strand::Forward) {
                arcPoints(poly, a0, a1 - head, rOut);
                poly.push_back(polar(a1, rMid));
                arcPoints(poly, a1 - head, a0, rIn);
            } else if (headHere) {
                poly.push_back(polar(a0, rMid));
                arcPoints(poly, a0 + head, a1, rOut);
                arcPoints(poly, a1, a0 + head, rIn);
            } else {
                arcPoints(poly, a0, a1, rOut);
                arcPoints(poly, a1, a0, rIn);
            }
            g.polys.push_back(poly);
            const double ta0 = (headHere && f.strand == Strand::Reverse) ? a0 + head : a0;
            const double ta1 = (headHere && f.strand == Strand::Forward) ? a1 - head : a1;
            if (ta1 - ta0 > bestLen) {
                bestLen = ta1 - ta0;
                textA0 = ta0;
                textA1 = ta1;
            }
            if (b + 1 < bl.size()) {
                std::vector<Point> conn;
                double c0 = angleOfPos(bl[b].end()), c1 = angleOfPos(bl[b + 1].start);
                if (c1 < c0)
                    c1 += 2 * kPi;
                arcPoints(conn, c0, c1, rMid);
                g.connectors.push_back(conn);
            }
        }

        // Name along the arc, when it fits.
        Font font{std::min(10.5 * m_ts, m_laneH * 0.72), true, false};
        const double textW = tm.width(f.name, font);
        if (textW + 8 < bestLen * rMid) {
            const double am = (textA0 + textA1) / 2;
            const bool flip = std::cos(am) < 0;   // bottom half: keep text upright
            const double total = textW / rMid;
            double cursor = 0;
            for (char ch : f.name) {
                const std::string cs(1, ch);
                const double cw = tm.width(cs, font);
                const double off = (cursor + cw / 2) / rMid;
                const double a = flip ? am + total / 2 - off : am - total / 2 + off;
                TextItem t;
                t.text = cs;
                t.font = font;
                t.color = g.text;
                t.pos = polar(a, rMid);
                t.halign = HAlign::Center;
                t.valign = VAlign::Middle;
                t.angle = flip ? a - kPi : a;
                g.label.push_back(t);
                cursor += cw;
            }
        }
        m_features.push_back(g);
    }
    // Draw inner lanes first.
    std::stable_sort(m_features.begin(), m_features.end(),
                     [](const FeatureGlyph &a, const FeatureGlyph &b) { return a.lane < b.lane; });

    // Callouts for features whose names don't fit on their arrows. They share
    // the label columns with the enzymes and are never dropped.
    for (const FeatureGlyph &fg : m_features) {
        if (!fg.label.empty())
            continue;
        const Feature &f = m_seq.features[size_t(fg.feature)];
        const Span sp = f.span(m_n, true);
        LabelGlyph g;
        g.feature = fg.feature;
        g.text = f.name;
        g.font = calloutFont;
        g.color = m_theme.text;
        g.swatch = fg.fill;
        g.priority = -1;
        g.angle = angleOfPos(std::fmod(sp.start + sp.length / 2.0, double(m_n)));
        g.anchorR = m_r + 6 + fg.lane * (m_laneH + kLaneGap) + m_laneH;
        g.rect = {0, 0, tm.width(g.text, g.font) + 12, tm.height(g.font)};
        m_labels.push_back(g);
    }
    // Primer names, from the primer arrows just outside the lanes.
    for (size_t i = 0; m_options.primers && i < m_seq.primers.size(); ++i) {
        const Primer &pr = m_seq.primers[i];
        if (!primerShown(pr, m_seq) || pr.name.empty())
            continue;
        LabelGlyph g;
        g.primer = int(i);
        g.text = pr.name;
        g.font = calloutFont;
        g.color = m_theme.text;
        g.swatch = Color::parse(pr.color, m_theme.primer);
        g.priority = -1;
        // The middle of a primer across the origin lies past the end.
        g.angle = angleOfPos(std::fmod((pr.binding.start + pr.binding.end) / 2.0, double(m_n)));
        g.anchorR = m_featureOuter + 10;
        g.rect = {0, 0, tm.width(g.text, g.font) + 12, tm.height(g.font)};
        m_labels.push_back(g);
    }

    // Labels in left and right columns, spread so they don't overlap.
    // With primers on the map, bend the leaders outside the primer band so
    // the bends don't sit on the primer arrows.
    bool primerBand = false;
    for (const Primer &pr : m_seq.primers)
        primerBand = primerBand || (m_options.primers && primerShown(pr, m_seq));
    const double elbowR = m_featureOuter + (primerBand ? 15 : 6);
    const double rLabel = elbowR + 12;
    for (LabelGlyph &g : m_labels) {
        if (g.feature < 0 && g.primer < 0) {
            g.angle = angleOfPos(m_sites[size_t(g.sites.front())].cutTop);
            g.anchorR = m_r;
        }
        const double a = g.angle;
        g.side = a <= kPi ? 1 : -1;
        g.ideal = m_c.y - rLabel * std::cos(a);
    }
    // Labels near the bottom (or top) of the circle would all stack in one
    // column under (or over) it, e.g. the sites of an MCS. Share such a band
    // between the two columns, keeping their order so leaders don't cross:
    // at the bottom, smaller angles go right; at the top, larger ones do.
    for (bool bottom : {true, false}) {
        auto offset = [&](double a) {   // signed angle from the band's centre, right side positive
            const double c = bottom ? kPi : 0;
            double d = std::remainder(a - c, 2 * kPi);
            return bottom ? -d : d;
        };
        std::vector<LabelGlyph *> band;
        int right = 0, left = 0;   // labels in this half outside the band
        for (LabelGlyph &g : m_labels) {
            const double d = offset(g.angle);
            if (std::fabs(d) <= kPi / 9)
                band.push_back(&g);
            else if (std::fabs(d) < kPi / 2)
                ++(d > 0 ? right : left);
        }
        if (band.size() < 2)
            continue;
        std::stable_sort(band.begin(), band.end(),
                         [&](const LabelGlyph *a, const LabelGlyph *b) { return offset(a->angle) > offset(b->angle); });
        const int m = int(band.size());
        int natural = 0;
        for (const LabelGlyph *g : band)
            natural += offset(g->angle) >= 0;
        int best = natural;
        for (int k = 0; k <= m; ++k) {
            const auto load = [&](int kk) { return std::max(right + kk, left + m - kk); };
            if (load(k) < load(best) || (load(k) == load(best) && std::abs(k - natural) < std::abs(best - natural)))
                best = k;
        }
        for (int i = 0; i < m; ++i)
            band[size_t(i)]->side = i < best ? 1 : -1;
    }
    const double lh = tm.height(m_labelFont) + 2;
    const double top = kMargin + lh / 2, bottom = m_h - kMargin - lh / 2;
    const int capacity = std::max(1, int((bottom - top) / lh) + 1);
    std::vector<LabelGlyph> kept;
    for (int side : {1, -1}) {
        std::vector<LabelGlyph> col;
        for (const LabelGlyph &g : m_labels)
            if (g.side == side)
                col.push_back(g);
        if (int(col.size()) > capacity) {
            std::stable_sort(col.begin(), col.end(),
                             [](const LabelGlyph &a, const LabelGlyph &b) { return a.priority < b.priority; });
            col.resize(size_t(capacity));
        }
        // On the right, labels run top to bottom with increasing angle; on the
        // left, bottom to top. Sorting by ideal y works for both.
        std::stable_sort(col.begin(), col.end(),
                         [](const LabelGlyph &a, const LabelGlyph &b) { return a.ideal < b.ideal; });
        std::vector<double> y(col.size());
        for (size_t i = 0; i < col.size(); ++i)
            y[i] = std::max(col[i].ideal, i ? y[i - 1] + lh : top);
        for (size_t i = col.size(); i-- > 0;) {
            const double limit = (i + 1 < col.size()) ? y[i + 1] - lh : bottom;
            y[i] = std::min(y[i], limit);
        }
        for (size_t i = 0; i < col.size(); ++i) {
            y[i] = std::max(y[i], i ? y[i - 1] + lh : top);
            LabelGlyph &g = col[i];
            const double dy = y[i] - m_c.y;
            const double rr = rLabel + 4;
            const double dx = std::fabs(dy) < rr ? std::sqrt(rr * rr - dy * dy) : 0;
            // Keep the whole label inside the map when the pane is narrow.
            const double room = m_w - 2 * kMargin;
            const double a = g.angle;
            // A label pushed down (or up) past the circle stays out beside its
            // site, so its leader runs outside the circle instead of across it.
            const double elbowX = m_c.x + elbowR * std::sin(a);
            const double near = side > 0 ? std::max(m_c.x + dx, elbowX) + 6 : std::min(m_c.x - dx, elbowX) - 6;
            const double x = side > 0 ? std::min(near, kMargin + std::max(0.0, room - g.rect.w))
                                      : std::max(near, m_w - kMargin - std::max(0.0, room - g.rect.w));
            g.leader = {polar(a, g.anchorR), polar(a, elbowR), {x - side * 3, y[i]}};
            g.align = side > 0 ? HAlign::Left : HAlign::Right;
            g.textPos = {x, y[i]};
            g.rect.x = side > 0 ? x : x - g.rect.w;
            g.rect.y = y[i] - g.rect.h / 2;
            kept.push_back(g);
        }
    }
    m_labels = kept;
}

// ---- linear ------------------------------------------------------------------

void MapLayout::layoutLinear(TextMeasurer &tm)
{
    buildLabels(tm);
    m_xl = kMargin + 20;   // room for the 5'/3' marks
    m_xr = m_w - kMargin - 20;
    m_laneH = 14;
    const double lh = tm.height(m_labelFont) + 3;

    // Labels: rows above the backbone, greedily, closest row first.
    std::stable_sort(m_labels.begin(), m_labels.end(), [&](const LabelGlyph &a, const LabelGlyph &b) {
        return m_sites[size_t(a.sites.front())].cutTop < m_sites[size_t(b.sites.front())].cutTop;
    });
    const int maxRows = std::max(1, int((m_h * 0.5 - kMargin - 20) / lh));
    std::vector<double> rowEnd;
    std::vector<int> rowOf(m_labels.size(), -1);
    for (size_t i = 0; i < m_labels.size(); ++i) {
        LabelGlyph &g = m_labels[i];
        const double x = xOfPos(m_sites[size_t(g.sites.front())].cutTop);
        double left = clampd(x - g.rect.w / 2, 2, m_w - 2 - g.rect.w);
        for (size_t r = 0; r <= rowEnd.size() && int(r) < maxRows; ++r) {
            if (r == rowEnd.size())
                rowEnd.push_back(-1e9);
            if (left > rowEnd[r] + 6) {
                rowEnd[r] = left + g.rect.w;
                rowOf[i] = int(r);
                g.rect.x = left;
                break;
            }
        }
    }
    const int rows = int(rowEnd.size());
    m_yb = kMargin + rows * lh + 22;
    std::vector<LabelGlyph> kept;
    for (size_t i = 0; i < m_labels.size(); ++i) {
        if (rowOf[i] < 0)
            continue;
        LabelGlyph g = m_labels[i];
        const double x = xOfPos(m_sites[size_t(g.sites.front())].cutTop);
        const double yc = m_yb - 22 - rowOf[i] * lh - lh / 2;
        g.rect.y = yc - g.rect.h / 2;
        g.textPos = {g.rect.x, yc};
        g.align = HAlign::Left;
        g.leader = {{x, m_yb - 1}, {x, m_yb - 10}, {g.rect.x + g.rect.w / 2, yc + g.rect.h / 2 + 1}};
        kept.push_back(g);
    }
    m_labels = kept;

    // Ticks below the backbone.
    const int step = niceStep(m_n, int(clampd((m_xr - m_xl) / 90, 3, 15)));
    const Font tickFont{9.5 * m_ts, false, false};
    for (int p = 0; p <= m_n; p += step) {
        const double x = xOfPos(p == 0 ? 0 : p);
        m_ticks.push_back({{x, m_yb + 4 + m_orfBand}, {x, m_yb + 9 + m_orfBand}});
        TextItem t;
        t.text = withThousands(p == 0 ? 1 : p);
        t.font = tickFont;
        t.color = m_theme.mutedText;
        t.pos = {x, m_yb + 20 + m_orfBand};
        t.halign = HAlign::Center;
        m_tickLabels.push_back(t);
    }

    // Features in lanes below the ticks.
    std::vector<int> order;
    for (size_t i = 0; i < m_seq.features.size(); ++i)
        if (shows(m_seq.features[i]) && !m_seq.features[i].ranges.empty())
            order.push_back(int(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return m_seq.features[size_t(a)].span(m_n, false).length > m_seq.features[size_t(b)].span(m_n, false).length;
    });
    int laneCount = 0;
    const std::vector<int> lanes = assignLanes(order, &laneCount);
    // Feature lanes start below the ORF rows, if any, and the ticks.
    const double lane0 = m_yb + 30 + m_orfBand;
    std::map<int, std::vector<std::pair<double, double>>> below;   // lane -> used x ranges
    const auto blocks = featureBlocks();
    for (int fi : order) {
        const Feature &f = m_seq.features[size_t(fi)];
        FeatureGlyph g;
        g.feature = fi;
        g.lane = lanes[size_t(fi)];
        g.fill = FeatureStyle::fill(f.type, f.color);
        g.text = FeatureStyle::textOn(g.fill);
        const double y0 = lane0 + g.lane * kLinearPitch, y1 = y0 + m_laneH, ym = (y0 + y1) / 2;
        const std::vector<Span> &bl = blocks[size_t(fi)];
        double bestW = -1, tx0 = 0, tx1 = 0;
        for (size_t b = 0; b < bl.size(); ++b) {
            double x0 = xOfPos(bl[b].start), x1 = xOfPos(bl[b].end());
            if (x1 - x0 < 2) {
                const double mid = (x0 + x1) / 2;
                x0 = mid - 1;
                x1 = mid + 1;
            }
            const bool headHere = (f.strand == Strand::Forward && b + 1 == bl.size())
                               || (f.strand == Strand::Reverse && b == 0);
            const double head = headHere ? std::min(m_laneH * 0.7, (x1 - x0) * 0.6) : 0;
            std::vector<Point> poly;
            if (headHere && f.strand == Strand::Forward)
                poly = {{x0, y0}, {x1 - head, y0}, {x1, ym}, {x1 - head, y1}, {x0, y1}};
            else if (headHere)
                poly = {{x0, ym}, {x0 + head, y0}, {x1, y0}, {x1, y1}, {x0 + head, y1}};
            else
                poly = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            g.polys.push_back(poly);
            const double a = (headHere && f.strand == Strand::Reverse) ? x0 + head : x0;
            const double z = (headHere && f.strand == Strand::Forward) ? x1 - head : x1;
            if (z - a > bestW) {
                bestW = z - a;
                tx0 = a;
                tx1 = z;
            }
            if (b + 1 < bl.size())
                g.connectors.push_back({{x1, ym}, {xOfPos(bl[b + 1].start), ym}});
        }
        Font font{10 * m_ts, true, false};
        if (tm.width(f.name, font) + 8 < bestW) {
            TextItem t;
            t.text = f.name;
            t.font = font;
            t.color = g.text;
            t.pos = {(tx0 + tx1) / 2, ym};
            t.halign = HAlign::Center;
            t.valign = VAlign::Middle;
            g.label.push_back(t);
        } else {
            // Name under the arrow, unless it would collide with another in this lane.
            const Font small{10 * m_ts, false, false};
            const double w = tm.width(f.name, small);
            const double cx = std::clamp((tx0 + tx1) / 2, w / 2 + 2, m_w - w / 2 - 2);
            auto &used = below[g.lane];
            bool clash = false;
            for (const auto &u : used)
                if (cx - w / 2 < u.second + 4 && u.first < cx + w / 2 + 4)
                    clash = true;
            if (!clash) {
                used.push_back({cx - w / 2, cx + w / 2});
                TextItem t;
                t.text = f.name;
                t.font = small;
                t.color = m_theme.text;
                t.pos = {cx, y1 + 9};
                t.halign = HAlign::Center;
                t.valign = VAlign::Middle;
                g.label.push_back(t);
            }
        }
        m_features.push_back(g);
    }
    m_linearBottom = lane0 + laneCount * kLinearPitch;
    // A row for the primers under the features.
    for (const Primer &pr : m_seq.primers)
        if (m_options.primers && primerShown(pr, m_seq)) {
            m_primerRowY = std::max(m_linearBottom, m_yb + 30) + 6;
            m_linearBottom = m_primerRowY + 22;
            break;
        }
}

// ---- rendering ---------------------------------------------------------------

Scene MapLayout::render(const MapState &st) const
{
    Scene s;
    s.width = m_w;
    s.height = m_h;
    s.background = m_theme.background;
    if (m_n == 0) {
        s.label("No sequence", {m_w / 2, m_h / 2}, {13, false, false}, m_theme.mutedText, HAlign::Center,
                VAlign::Middle);
        return s;
    }
    if (m_circular)
        renderCircular(s, st);
    else
        renderLinear(s, st);
    return s;
}

void MapLayout::renderLabels(Scene &s, const MapState &st) const
{
    for (const LabelGlyph &g : m_labels) {
        const bool active = std::find(g.sites.begin(), g.sites.end(), st.activeSite) != g.sites.end();
        PathItem leader;
        leader.points = g.leader;
        leader.stroke = g.feature >= 0 || g.primer >= 0 ? g.swatch.mix(m_theme.text, 0.35)
                      : active         ? m_theme.cutMarker
                                       : m_theme.leader;
        leader.lineWidth = active ? 1.5 : 0.8;
        s.path(leader);
    }
}

void MapLayout::renderCircular(Scene &s, const MapState &st) const
{
    // Selection wedge (under everything else).
    if (st.hasSelection && st.selection.length > 0) {
        const double a0 = angleOfPos(st.selection.start);
        const double a1 = angleOfPos(st.selection.start + std::min(st.selection.length, m_n));
        std::vector<Point> poly;
        arcPoints(poly, a0, a1, m_featureOuter + 3);
        arcPoints(poly, a1, a0, m_innerR);
        s.polygon(poly, m_theme.selection);
        s.line(polar(a0, m_innerR), polar(a0, m_featureOuter + 3), m_theme.selectionEdge, 1);
        s.line(polar(a1, m_innerR), polar(a1, m_featureOuter + 3), m_theme.selectionEdge, 1);
    }

    renderLabels(s, st);   // leaders first, so features sit on top of them

    // Backbone: two thin concentric circles.
    for (double r : {m_r, m_r - 3.0}) {
        PathItem ring;
        arcPoints(ring.points, 0, 2 * kPi, r);
        ring.closed = true;
        ring.stroke = m_theme.backbone;
        ring.lineWidth = 1.2;
        s.path(ring);
    }
    for (const auto &t : m_ticks)
        s.line(t.first, t.second, m_theme.tick, 1);
    for (const TextItem &t : m_tickLabels)
        s.text(t);
    renderOrfs(s);

    for (const FeatureGlyph &g : m_features) {
        const bool active = g.feature == st.activeFeature;
        for (const auto &c : g.connectors) {
            PathItem p;
            p.points = c;
            p.stroke = g.fill.mix(m_theme.text, 0.3);
            p.lineWidth = 1.2;
            s.path(p);
        }
        for (const auto &poly : g.polys)
            s.polygon(poly, g.fill, active ? m_theme.accent : m_theme.featureOutline, active ? 2.0 : 0.8);
        for (const TextItem &t : g.label)
            s.text(t);
    }

    // Primers: outlined arrows just outside the feature lanes, pointing 5' to 3'.
    for (const Primer &pr : m_seq.primers) {
        if (!m_options.primers || !primerShown(pr, m_seq))
            continue;
        const double r0 = m_featureOuter + 2, r1 = r0 + 7, rm = (r0 + r1) / 2;
        double a0 = angleOfPos(pr.binding.start), a1 = angleOfPos(pr.binding.end);
        const double head = std::min(8.0 / rm, (a1 - a0) * 0.5);
        std::vector<Point> poly;
        if (pr.strand == Strand::Reverse) {
            poly.push_back(polar(a0, rm));
            arcPoints(poly, a0 + head, a1, r1);
            arcPoints(poly, a1, a0 + head, r0);
        } else {
            arcPoints(poly, a0, a1 - head, r1);
            poly.push_back(polar(a1, rm));
            arcPoints(poly, a1 - head, a0, r0);
        }
        const Color pc = Color::parse(pr.color, m_theme.primer);
        s.polygon(poly, pc.withAlpha(0.12), pc, 1.4);
        // A 5' tail that doesn't anneal: a short stub kinked off the arrow's
        // 5' end, away from the sequence, about as long as the tail.
        const int tail = std::max(0, int(pr.bases.size()) - pr.binding.length());
        if (tail > 0) {
            const double len = std::clamp(tail * 2 * kPi / std::max(1, m_n), 4.0 / rm, 24.0 / rm);
            const bool rev = pr.strand == Strand::Reverse;
            const double a5 = rev ? a1 : a0, out = rev ? a1 + len : a0 - len;
            s.line(polar(a5, rm), polar(a5 + (out - a5) * 0.25, r1 + 4), pc, 1.4);
            std::vector<Point> stub;
            arcPoints(stub, a5 + (out - a5) * 0.25, out, r1 + 4);
            for (size_t i = 1; i < stub.size(); ++i)
                s.line(stub[i - 1], stub[i], pc, 1.4);
        }
    }

    // Caret.
    if (st.hasSelection && st.selection.length == 0) {
        const double a = angleOfPos(st.selection.start);
        s.line(polar(a, m_innerR), polar(a, m_featureOuter + 3), m_theme.selectionEdge, 1.5);
    }

    // Labels.
    for (const LabelGlyph &g : m_labels) {
        if (g.feature >= 0 || g.primer >= 0) {
            const bool activeF = g.feature >= 0 && g.feature == st.activeFeature;
            if (activeF)
                s.rect(g.rect.adjusted(2), m_theme.accent.withAlpha(0.15));
            const double cy = g.rect.y + g.rect.h / 2;
            s.rect({g.rect.x + 1, cy - 4, 8, 8}, g.swatch, m_theme.featureOutline, 0.8);
            s.label(g.text, {g.rect.x + 12, cy}, g.font, g.color, HAlign::Left, VAlign::Middle);
            continue;
        }
        const bool active = std::find(g.sites.begin(), g.sites.end(), st.activeSite) != g.sites.end();
        if (active)
            s.rect(g.rect.adjusted(2), m_theme.cutMarker.withAlpha(0.15));
        s.label(g.text, g.textPos, g.font, active ? m_theme.cutMarker : g.color, g.align, VAlign::Middle);
    }

    // Name and length in the center.
    const std::string name = m_seq.name.empty() ? "Untitled" : m_seq.name;
    const double nameSize = clampd(m_innerR / 7, 11, 18) * m_ts;   // as in layoutCircular
    s.label(name, {m_c.x, m_c.y - 2}, {nameSize, true, false}, m_theme.text, HAlign::Center, VAlign::Baseline);
    s.label(formatBp(m_n), {m_c.x, m_c.y + nameSize * 0.6 + 4}, {nameSize * 0.72, false, false},
            m_theme.mutedText, HAlign::Center, VAlign::Top);
}

void MapLayout::renderLinear(Scene &s, const MapState &st) const
{
    const double bottom = std::max(m_linearBottom, m_yb + 30);
    if (st.hasSelection && st.selection.length > 0) {
        const double x0 = xOfPos(st.selection.start);
        const double x1 = xOfPos(std::min(st.selection.end(), m_n));
        s.rect({x0, m_yb - 8, std::max(1.0, x1 - x0), bottom - m_yb + 10}, m_theme.selection);
    }
    renderLabels(s, st);
    s.line({m_xl, m_yb}, {m_xr, m_yb}, m_theme.backbone, 1.2);
    s.line({m_xl, m_yb + 3}, {m_xr, m_yb + 3}, m_theme.backbone, 1.2);
    const Font endFont{10 * m_ts, false, false};
    s.label("5\u2032", {m_xl - 4, m_yb + 1.5}, endFont, m_theme.mutedText, HAlign::Right, VAlign::Middle);
    s.label("3\u2032", {m_xr + 4, m_yb + 1.5}, endFont, m_theme.mutedText, HAlign::Left, VAlign::Middle);
    for (const auto &t : m_ticks)
        s.line(t.first, t.second, m_theme.tick, 1);
    for (const TextItem &t : m_tickLabels)
        s.text(t);
    renderOrfs(s);
    for (const FeatureGlyph &g : m_features) {
        const bool active = g.feature == st.activeFeature;
        for (const auto &c : g.connectors)
            s.line(c.front(), c.back(), g.fill.mix(m_theme.text, 0.3), 1.2);
        for (const auto &poly : g.polys)
            s.polygon(poly, g.fill, active ? m_theme.accent : m_theme.featureOutline, active ? 2.0 : 0.8);
        for (const TextItem &t : g.label)
            s.text(t);
    }
    // Primers: outlined arrows in their own row, pointing 5' to 3', named
    // beside them, with a stub up off the 5' end for a tail that doesn't anneal.
    for (const Primer &pr : m_seq.primers) {
        if (!m_options.primers || !primerShown(pr, m_seq) || m_primerRowY <= 0)
            continue;
        const bool rev = pr.strand == Strand::Reverse;
        const double y0 = m_primerRowY, y1 = y0 + 7, ym = (y0 + y1) / 2;
        double x0 = xOfPos(pr.binding.start), x1 = xOfPos(pr.binding.end);
        if (x1 - x0 < 6) {
            const double mid = (x0 + x1) / 2;
            x0 = mid - 3;
            x1 = mid + 3;
        }
        const double head = std::min(6.0, (x1 - x0) * 0.5);
        const std::vector<Point> poly = rev ? std::vector<Point>{{x0, ym}, {x0 + head, y0}, {x1, y0}, {x1, y1}, {x0 + head, y1}}
                                            : std::vector<Point>{{x0, y0}, {x1 - head, y0}, {x1, ym}, {x1 - head, y1}, {x0, y1}};
        const Color pc = Color::parse(pr.color, m_theme.primer);
        s.polygon(poly, pc.withAlpha(0.12), pc, 1.4);
        const int tail = std::max(0, int(pr.bases.size()) - pr.binding.length());
        if (tail > 0) {
            const double x5 = rev ? x1 : x0;
            const double len = std::clamp(tail * (m_xr - m_xl) / std::max(1, m_n), 4.0, 24.0);
            const double out = rev ? x5 + len : x5 - len;
            s.line({x5, ym}, {x5 + (out - x5) * 0.25, y0 - 4}, pc, 1.4);
            s.line({x5 + (out - x5) * 0.25, y0 - 4}, {out, y0 - 4}, pc, 1.4);
        }
        // The name past the 3' end, inside the map.
        const Font font{10 * m_ts, false, false};
        if (rev)
            s.label(pr.name, {std::max(x0 - 4, m_xl), ym}, font, m_theme.text,
                    x0 - 4 > m_xl + 40 ? HAlign::Right : HAlign::Left, VAlign::Middle);
        else
            s.label(pr.name, {std::min(x1 + 4, m_xr), ym}, font, m_theme.text,
                    x1 + 4 < m_xr - 40 ? HAlign::Left : HAlign::Right, VAlign::Middle);
    }
    if (st.hasSelection && st.selection.length == 0) {
        const double x = xOfPos(st.selection.start);
        s.line({x, m_yb - 8}, {x, bottom + 2}, m_theme.selectionEdge, 1.5);
    }
    for (const LabelGlyph &g : m_labels) {
        const bool active = std::find(g.sites.begin(), g.sites.end(), st.activeSite) != g.sites.end();
        // Opaque backing so leaders from labels above don't run through this one.
        s.rect(g.rect.adjusted(1), m_theme.background);
        if (active)
            s.rect(g.rect.adjusted(2), m_theme.cutMarker.withAlpha(0.15));
        s.label(g.text, g.textPos, g.font, active ? m_theme.cutMarker : g.color, g.align, VAlign::Middle);
    }
    const std::string title = (m_seq.name.empty() ? std::string("Untitled") : m_seq.name) + "  ·  "
                            + formatBp(m_n) + " linear";
    s.label(title, {m_w / 2, m_h - kMargin}, {12, true, false}, m_theme.mutedText, HAlign::Center);
}

// ---- hit testing ---------------------------------------------------------------

double MapLayout::angleAt(Point p) const
{
    double a = std::atan2(p.x - m_c.x, -(p.y - m_c.y));
    if (a < 0)
        a += 2 * kPi;
    return a;
}

int MapLayout::positionAt(Point p) const
{
    if (m_n == 0)
        return 0;
    if (m_circular)
        return int(std::lround(angleAt(p) / (2 * kPi) * m_n)) % m_n;
    return int(std::lround(clampd((p.x - m_xl) / (m_xr - m_xl), 0, 1) * m_n));
}

MapHit MapLayout::hitTest(Point p) const
{
    MapHit hit;
    if (m_n == 0)
        return hit;
    if (const int pr = primerAt(p); pr >= 0) {
        hit.kind = MapHit::Primer;
        hit.index = pr;
        hit.position = positionAt(p);
        return hit;
    }
    for (const LabelGlyph &g : m_labels)
        if (g.primer < 0 && g.rect.adjusted(2).contains(p)) {
            hit.kind = g.feature >= 0 ? MapHit::Feature : MapHit::Site;
            hit.index = g.feature >= 0 ? g.feature : g.sites.front();
            return hit;
        }
    for (auto it = m_features.rbegin(); it != m_features.rend(); ++it)
        for (const auto &poly : it->polys)
            if (pointInPolygon(poly, p)) {
                hit.kind = MapHit::Feature;
                hit.index = it->feature;
                return hit;
            }
    hit.kind = MapHit::Sequence;
    hit.position = positionAt(p);
    return hit;
}

// The primer whose arrow or name is at p, or -1. The arrows are drawn
// as in render(): a band just outside the features, or the linear primer row.
int MapLayout::primerAt(Point p) const
{
    if (!m_options.primers)
        return -1;
    for (const LabelGlyph &g : m_labels)
        if (g.primer >= 0 && g.rect.adjusted(2).contains(p))
            return g.primer;
    for (int i = int(m_seq.primers.size()) - 1; i >= 0; --i) {
        const Primer &pr = m_seq.primers[size_t(i)];
        if (!primerShown(pr, m_seq))
            continue;
        if (m_circular) {
            const double r = std::hypot(p.x - m_c.x, p.y - m_c.y);
            const double r0 = m_featureOuter + 2;
            if (r < r0 - 3 || r > r0 + 10)
                continue;
            const double pos = angleAt(p) / (2 * kPi) * m_n;
            const double slack = 3.0 / std::max(1.0, r) / (2 * kPi) * m_n;   // 3 px either side
            if ((pos >= pr.binding.start - slack && pos <= pr.binding.end + slack)
                || pos + m_n <= pr.binding.end + slack)   // past the origin
                return i;
        } else {
            if (m_primerRowY <= 0 || p.y < m_primerRowY - 3 || p.y > m_primerRowY + 10)
                continue;
            if (p.x >= xOfPos(pr.binding.start) - 3 && p.x <= xOfPos(pr.binding.end) + 3)
                return i;
        }
    }
    return -1;
}

std::string MapLayout::featureTooltip(int i) const
{
    const Feature &f = m_seq.features[size_t(i)];
    const Span sp = f.span(m_n, m_circular);
    int end = sp.end() > m_n ? sp.end() - m_n : sp.end();
    std::string s = f.name + " (" + f.type + ")\n" + withThousands(sp.start + 1) + " .. " + withThousands(end)
                  + "  ·  " + formatBp(f.totalLength());
    if (f.strand != Strand::None)
        s += std::string("  ·  ") + strandName(f.strand);
    return s;
}

std::string MapLayout::siteTooltip(const std::vector<int> &sites) const
{
    std::string s;
    for (int si : sites) {
        const CutSite &c = m_sites[size_t(si)];
        if (!s.empty())
            s += "\n";
        s += c.enzyme->name + "  " + c.enzyme->site + "  cuts after " + withThousands(c.cutTop);
        const int count = m_cutCount[size_t(si)];
        s += count == 1 ? "  (unique)" : "  (" + std::to_string(count) + " sites)";
        if (m_methyl[size_t(si)] != MethylNone)
            s += "\nBlocked by " + methylationName(m_methyl[size_t(si)]) + " methylation (DNA from dam+ dcm+ E. coli)";
    }
    return s;
}

std::string MapLayout::tooltipAt(Point p) const
{
    for (const LabelGlyph &g : m_labels)
        if (g.rect.adjusted(2).contains(p)) {
            if (g.primer >= 0) {
                const Primer &pr = m_seq.primers[size_t(g.primer)];
                return pr.name + " (primer), " + std::to_string(pr.bases.size()) + " nt\n" + pr.bases;
            }
            return g.feature >= 0 ? featureTooltip(g.feature) : siteTooltip(g.sites);
        }
    for (auto it = m_features.rbegin(); it != m_features.rend(); ++it)
        for (const auto &poly : it->polys)
            if (pointInPolygon(poly, p))
                return featureTooltip(it->feature);
    return {};
}

} // namespace oc::view
