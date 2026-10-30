// SPDX-License-Identifier: GPL-3.0-or-later
// A virtual agarose gel: lanes of bands placed by size, drawn as a Scene.
#pragma once

#include "scene.h"
#include "style.h"

#include <string>
#include <vector>

namespace oc::view {

struct GelLane {
    std::string name;
    std::vector<int> bands;    // bp
    std::vector<int> bright;   // ladder reference bands, drawn brighter
    bool ladder = false;       // band sizes are labeled beside the lane
    bool uncutCircular = false;
};

// How far a band of `bp` runs on a `percent` agarose gel, 0 at the wells to 1
// at the bottom of a full run (more: it ran off the gel); a supercoiled
// (uncut circular) molecule runs ahead of its linear size.
double gelRunFraction(double bp, bool supercoiled = false, double percent = 1.0);
// The agarose percentages offered, lowest first.
int gelPercentCount();
double gelPercent(int index);
// How bright band `bp` of `lane` looks, 0..1.
double gelBandIntensity(const GelLane &lane, int bp);

// Bands migrate by log(size), as on a roughly 1% gel; brightness follows the
// mass in each band, so longer fragments of an equimolar digest look brighter.
class GelLayout {
public:
    GelLayout(std::vector<GelLane> lanes, const Theme &theme, TextMeasurer &measure, double width, double height);
    Scene render() const;
    double yOfSize(double bp) const;

private:
    std::vector<GelLane> m_lanes;
    Theme m_theme;
    double m_w, m_h;
    double m_gelX = 0, m_gelY = 0, m_gelW = 0, m_gelH = 0;
    double m_laneW = 0, m_laneGap = 0, m_runTop = 0, m_runBottom = 0;
};

} // namespace oc::view
