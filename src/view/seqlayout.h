// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scene.h"
#include "style.h"
#include "../core/enzymes.h"
#include "../core/sequence.h"

#include <map>

namespace oc::view {

struct SeqViewState {
    Span selection;
    bool hasSelection = false;
    int activeFeature = -1;
    int activeSite = -1;   // index into the sites passed to the layout
};

// Wraps the sequence into lines of a multiple of 10 bp that fit `width`.
// Each line: enzyme label rows, top strand, optional complement, ruler,
// then feature lanes (CDS lanes get an amino-acid row). Forward primers sit
// just above the top strand, reverse primers just below the bottom one.
// With `frames` = 3 or 6, the forward reading frames are translated above the
// strands and (for 6) the reverse frames below them.
// Holds references to `seq` and `sites`.
class SequenceLayout {
public:
    SequenceLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
                   TextMeasurer &measure, double width, bool showComplement, int frames = 0);

    double totalHeight() const { return m_totalHeight; }
    int basesPerLine() const { return m_bpl; }
    // Draws the lines visible in [scrollY, scrollY + viewHeight), shifted up by scrollY.
    Scene render(double scrollY, double viewHeight, const SeqViewState &st) const;

    // Hit tests take content coordinates (view y + scrollY).
    int positionAt(Point p) const;     // nearest base boundary, 0..n
    int featureAt(Point p) const;      // -1 if none
    int siteAt(Point p) const;         // -1 if none
    int primerAt(Point p) const;       // -1 if none
    double lineTopFor(int pos) const;
    double lineHeightFor(int pos) const;
    std::string tooltipAt(Point p) const;

private:
    struct SiteLabel {
        std::vector<int> sites;
        std::string text;
        double x = 0, w = 0;
        int row = 0;
        bool unique = false, typeIIS = false;
    };
    struct LanePiece {
        int feature;
        int lane;
        int a, b;   // columns within the line, [a, b)
        bool head;  // 3' end of the feature is in this piece
    };
    struct PrimerPiece {
        int primer;
        int row;
        int a, b;   // columns within the line, [a, b)
        bool head;  // 3' end is in this piece
        bool top;   // forward primers sit above the top strand, reverse ones below
    };
    struct Line {
        int start = 0, len = 0;
        double y = 0, height = 0;
        int enzymeRows = 0;
        std::vector<SiteLabel> labels;
        std::vector<LanePiece> pieces;
        std::vector<bool> laneHasAA;
        std::vector<PrimerPiece> primers;
        int primerRowsTop = 0, primerRowsBottom = 0;
        // y offsets within the line
        double yFramesTop = 0, yFramesBottom = 0, yPrimersTop = 0, yTop = 0, yBottom = 0, yPrimersBottom = 0, yRuler = 0, yLanes = 0;
    };

    double colX(int col) const;        // left edge of the base at column col
    double boundaryX(int b) const;     // x of the boundary before column b
    int lineIndexAt(double y) const;
    double laneY(const Line &l, int lane) const;
    Rect primerRect(const Line &l, const PrimerPiece &p) const;   // content coordinates
    void buildLine(Line &line, TextMeasurer &tm);

    const oc::Sequence &m_seq;
    const std::vector<CutSite> &m_sites;
    Theme m_theme;
    bool m_complement;
    int m_frames = 0;
    int m_n = 0;
    int m_bpl = 10;
    double m_width = 0;
    double m_charW = 8;
    double m_left = 0;
    double m_gap = 6;
    double m_totalHeight = 0;
    Font m_baseFont{13, false, true};
    Font m_smallFont{10, false, false};
    Font m_numFont{10, false, true};
    double m_baseH = 18, m_labelH = 13, m_rulerH = 19, m_laneH = 14, m_aaH = 15, m_primerH = 13;
    std::vector<Line> m_lines;
    std::vector<int> m_cutCount;
    std::map<int, std::vector<CodonMark>> m_marks;   // CDS feature -> marks sorted by position
};

} // namespace oc::view
