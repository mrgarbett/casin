// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scene.h"
#include "style.h"
#include "../core/align.h"
#include "../core/enzymes.h"
#include "../core/sequence.h"

#include <map>
#include <set>

namespace oc::view {

struct SeqViewState {
    Span selection;
    bool hasSelection = false;
    int activeFeature = -1;
    int activeSite = -1;   // index into the sites passed to the layout
    int hoverCodon = -1;   // first base of a codon outlined on the strands (its amino acid is under the mouse)
};

// What the sequence view shows besides the bases.
struct SeqOptions {
    int frames = 0;            // 0, 3 (forward) or 6 reading frames
    bool groupTens = false;    // a small gap after every 10 bases
    bool enzymes = true;
    bool features = true;
    bool primers = true;
    std::set<std::string> hiddenTypes;   // feature types left out (Feature::type)
    int minOrfCodons = 75;     // frames show only ORFs at least this long (stop excluded)
};

// Wraps the sequence into lines of a multiple of 10 bp that fit `width`.
// Each line: enzyme label rows, top strand, optional complement, ruler,
// then feature lanes (CDS lanes get an amino-acid row). Forward primers sit
// just above the top strand, reverse primers just below the bottom one.
// With `frames` = 3 or 6, open reading frames are translated above the strands
// (forward, green) and for 6 below them (reverse, red); a line only gets rows
// for the frames that have an ORF on it. 5' and 3' mark the strand ends. Aligned reads get a row
// each under the ruler, with differences from the reference highlighted.
// Holds references to `seq` and `sites`.
class SequenceLayout {
public:
    SequenceLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
                   TextMeasurer &measure, double width, bool showComplement, int frames = 0,
                   std::vector<ReadTrack> reads = {});
    SequenceLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
                   TextMeasurer &measure, double width, bool showComplement, const SeqOptions &options,
                   std::vector<ReadTrack> reads = {});

    double totalHeight() const { return m_totalHeight; }
    int basesPerLine() const { return m_bpl; }
    // Draws the lines visible in [scrollY, scrollY + viewHeight), shifted up by scrollY.
    Scene render(double scrollY, double viewHeight, const SeqViewState &st) const;

    // Hit tests take content coordinates (view y + scrollY).
    int positionAt(Point p) const;     // nearest base boundary, 0..n
    int featureAt(Point p) const;      // -1 if none
    int siteAt(Point p) const;         // -1 if none
    int primerAt(Point p) const;       // -1 if none
    // First base of the codon whose amino acid is under p (in an ORF frame
    // row or a CDS translation row), or -1.
    int codonAt(Point p) const;
    // The base under p on either strand, or -1.
    int baseAt(Point p) const;
    int readAt(Point p) const;         // -1 if none
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
        bool blocked = true;   // every site here blocked by Dam/Dcm methylation
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
        int a, b;   // columns within the line, [a, b): the annealing bases and the 5' tail
        int bindA, bindB;   // the annealing columns among them (empty when only tail)
        int nameCols;       // columns kept free past the 3' end for the name (0: no name here)
        bool head;  // 3' end is in this piece
        bool top;   // forward primers sit above the top strand, reverse ones below
        int shift = 0;   // what to add to a base's position to place it on the primer: n or -n across the origin
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
        std::vector<int> framesTop, framesBottom;   // frame rows on this line (0..2, as +1..+3 / -1..-3)
        std::vector<int> reads;   // reads that cover part of this line, one row each
        // y offsets within the line
        double yFramesTop = 0, yFramesBottom = 0, yPrimersTop = 0, yTop = 0, yBottom = 0, yPrimersBottom = 0, yRuler = 0, yReads = 0, yLanes = 0;
    };

    double colX(int col) const;        // left edge of the base at column col
    int columnAt(double x) const;      // the column whose base covers x (may be < 0 or >= m_bpl)
    double boundaryX(int b) const;     // x of the boundary before column b
    int lineIndexAt(double y) const;
    double laneY(const Line &l, int lane) const;
    // Up to two pieces of the reference that a read covers.
    std::vector<Range> readRanges(const ReadTrack &r) const;
    Rect primerRect(const Line &l, const PrimerPiece &p) const;   // content coordinates
    void buildLine(Line &line, TextMeasurer &tm);

    const oc::Sequence &m_seq;
    const std::vector<CutSite> &m_sites;
    Theme m_theme;
    bool m_complement;
    SeqOptions m_opt;
    std::vector<char> m_orfFwd, m_orfRev;   // amino acid at each ORF codon's first top-strand base, else 0
    std::vector<ReadTrack> m_reads;
    int m_n = 0;
    int m_bpl = 10;
    double m_width = 0;
    double m_charW = 8;
    double m_left = 0;
    double m_numRight = 0;   // right edge of the position numbers
    double m_endW = 0;       // room for a 5'/3' mark at each end of a line
    double m_gap = 6;
    double m_totalHeight = 0;
    Font m_baseFont{13, false, true};
    Font m_smallFont{10, false, false};
    Font m_numFont{10, false, true};
    Font m_primerNameFont{10, true, false};
    double m_baseH = 18, m_labelH = 13, m_rulerH = 19, m_laneH = 14, m_aaH = 15, m_primerH = 18;
    std::vector<Line> m_lines;
    std::vector<int> m_cutCount;
    std::vector<int> m_methyl;   // per site: MethylationBlock flags
    std::map<int, std::vector<CodonMark>> m_marks;   // CDS feature -> marks sorted by position
};

} // namespace oc::view
