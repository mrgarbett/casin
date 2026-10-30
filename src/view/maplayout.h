// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scene.h"
#include "style.h"
#include "../core/enzymes.h"
#include "../core/sequence.h"

#include <set>

namespace oc::view {

struct MapState {
    Span selection;          // length 0 = caret
    bool hasSelection = false;
    int activeFeature = -1;  // index into Sequence::features
    int activeSite = -1;     // index into the sites passed to the layout
};

// What the map shows. Hidden feature types are matched on Feature::type.
struct MapOptions {
    bool enzymes = true;
    bool primers = true;
    std::set<std::string> hiddenTypes;
    int frames = 0;           // 3: forward ORFs as arrows, 6: both strands (as the sequence's translation)
    int minOrfCodons = 75;    // as SeqOptions::minOrfCodons
};

struct MapHit {
    enum Kind { None, Feature, Site, Sequence } kind = None;
    int index = -1;          // feature index, or site index for Site
    int position = 0;        // nearest base boundary for Sequence
};

// Lays out a circular map (circular sequences) or a linear map (otherwise)
// for a given size. Construct once per size/content change; render() and
// the hit tests are cheap. Holds references to `seq` and `sites`.
class MapLayout {
public:
    MapLayout(const oc::Sequence &seq, const std::vector<CutSite> &sites, const Theme &theme,
              TextMeasurer &measure, double width, double height, MapOptions options = {});

    Scene render(const MapState &state) const;
    MapHit hitTest(Point p) const;
    int positionAt(Point p) const;
    // Circular only: angle of p around the center, radians clockwise from 12 o'clock.
    double angleAt(Point p) const;
    bool isCircular() const { return m_circular; }
    std::string tooltipAt(Point p) const;

private:
    struct FeatureGlyph {
        int feature = -1;
        int lane = 0;
        Color fill, text;
        std::vector<std::vector<Point>> polys;     // one per block
        std::vector<std::vector<Point>> connectors;
        std::vector<TextItem> label;               // empty when the name doesn't fit
    };
    struct LabelGlyph {
        std::vector<int> sites;
        std::string text;
        Font font;
        Color color;
        Rect rect;
        Point textPos;
        HAlign align = HAlign::Left;
        std::vector<Point> leader;
        int priority = 0;
        double ideal = 0;
        int side = 1;
        double angle = 0;      // anchor angle (circular)
        double anchorR = 0;    // leader starts at this radius
        int feature = -1;      // >= 0: a callout for a feature whose name didn't fit
        int primer = -1;       // >= 0: a primer's name
        Color swatch;
    };

    bool shows(const Feature &f) const;
    void layoutCircular(TextMeasurer &tm);
    void layoutLinear(TextMeasurer &tm);
    void buildLabels(TextMeasurer &tm);
    std::vector<std::vector<Span>> featureBlocks() const;
    std::vector<int> assignLanes(const std::vector<int> &order, int *laneCount) const;
    Point polar(double angle, double r) const;
    double angleOfPos(double pos) const;
    double xOfPos(double pos) const;
    void arcPoints(std::vector<Point> &out, double a0, double a1, double r) const;
    void renderCircular(Scene &s, const MapState &st) const;
    void renderLinear(Scene &s, const MapState &st) const;
    void renderLabels(Scene &s, const MapState &st) const;
    void findMapOrfs();
    void renderOrfs(Scene &s) const;
    std::string featureTooltip(int i) const;
    std::string siteTooltip(const std::vector<int> &sites) const;

    const oc::Sequence &m_seq;
    const std::vector<CutSite> &m_sites;
    Theme m_theme;
    MapOptions m_options;
    double m_w, m_h;
    int m_n;
    bool m_circular;
    std::vector<int> m_cutCount;   // per site: total cuts of its enzyme
    std::vector<int> m_methyl;     // per site: MethylationBlock flags
    std::vector<FeatureGlyph> m_features;
    // ORFs drawn as arrows (MapOptions::frames): each on a row of its strand.
    struct OrfGlyph {
        int start = 0, length = 0;
        bool forward = true;
        int row = 0;
    };
    std::vector<OrfGlyph> m_orfs;
    int m_orfRows = 0;      // rows per strand
    double m_orfBand = 0;   // room the rows take next to the backbone, before the ticks
    std::vector<LabelGlyph> m_labels;
    std::vector<TextItem> m_tickLabels;
    std::vector<std::pair<Point, Point>> m_ticks;
    // circular geometry
    Point m_c;
    double m_r = 0;          // backbone radius
    double m_laneH = 12;
    double m_featureOuter = 0;
    double m_innerR = 0;
    // linear geometry
    double m_xl = 0, m_xr = 0, m_yb = 0, m_linearBottom = 0;
    double m_primerRowY = 0;   // linear: the primers' row, or 0 for none
    Font m_labelFont{11, false, false};
};

std::string formatBp(int n);   // "2,686 bp"
std::string withThousands(int n);

} // namespace oc::view
