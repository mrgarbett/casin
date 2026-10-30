// SPDX-License-Identifier: GPL-3.0-or-later
#include "gellayout.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace oc::view {

namespace {
const double kMaxBp = 20000, kMinBp = 70;
// A supercoiled plasmid runs ahead of its linear form; this is a rough factor.
const double kSupercoiled = 0.7;
// The open circle drags behind its linear form.
const double kOpenCircular = 1.6;

std::string bandText(int bp)
{
    if (bp >= 1000 && bp % 1000 == 0)
        return std::to_string(bp / 1000) + " kb";
    if (bp >= 1000 && bp % 100 == 0)
        return std::to_string(bp / 1000) + "." + std::to_string(bp / 100 % 10) + " kb";
    return std::to_string(bp);
}
} // namespace

// Agarose percentages and the sizes each resolves: a band of `top` bp barely
// leaves the wells; one of `bottom` bp reaches the end of a full run. Rough
// figures for TAE at the usual voltages; in between, sizes are interpolated.
struct Agarose {
    double percent, top, bottom;
};
const Agarose kAgarose[] = {{0.5, 40000, 400}, {0.7, 30000, 150}, {1.0, kMaxBp, kMinBp},
                            {1.5, 8000, 40},   {2.0, 4000, 25},   {3.0, 1500, 10}};

int gelPercentCount() { return int(std::size(kAgarose)); }
double gelPercent(int index) { return kAgarose[std::clamp(index, 0, gelPercentCount() - 1)].percent; }

GelForm gelBandForm(const GelLane &lane, size_t band)
{
    if (band < lane.forms.size())
        return lane.forms[band];
    return lane.uncutCircular ? GelForm::Supercoiled : GelForm::Linear;
}

double gelRunFraction(double bp, GelForm form, double percent)
{
    // The gel's range, between the two percentages around it.
    const int n = gelPercentCount();
    double top = kAgarose[0].top, bottom = kAgarose[0].bottom;
    if (percent >= kAgarose[n - 1].percent) {
        top = kAgarose[n - 1].top;
        bottom = kAgarose[n - 1].bottom;
    } else {
        for (int i = 0; i + 1 < n; ++i) {
            const Agarose &a = kAgarose[i], &b = kAgarose[i + 1];
            if (percent < a.percent || percent > b.percent)
                continue;
            const double f = (percent - a.percent) / (b.percent - a.percent);
            top = std::pow(10, std::log10(a.top) + f * (std::log10(b.top) - std::log10(a.top)));
            bottom = std::pow(10, std::log10(a.bottom) + f * (std::log10(b.bottom) - std::log10(a.bottom)));
            break;
        }
    }
    bp *= form == GelForm::Supercoiled ? kSupercoiled : form == GelForm::OpenCircular ? kOpenCircular : 1.0;
    bp = std::max(1.0, bp);
    const double t = (std::log10(top) - std::log10(bp)) / (std::log10(top) - std::log10(bottom));
    // Bands too big to resolve crowd together just under the wells.
    const double crowd = 0.08;
    return t >= crowd ? t : crowd * std::exp((t - crowd) / crowd);
}

double gelBandIntensity(const GelLane &lane, size_t band)
{
    if (band >= lane.bands.size())
        return 0;
    const int bp = lane.bands[band];
    // Mass per band, relative to the heaviest band in the lane: an equimolar
    // digest's big fragments carry more stain than its small ones.
    double heaviest = 1;
    for (int b : lane.bands)
        heaviest = std::max(heaviest, double(b));
    const bool bright = std::find(lane.bright.begin(), lane.bright.end(), bp) != lane.bright.end();
    double alpha = lane.ladder ? (bright ? 0.95 : 0.6) : 0.18 + 0.82 * std::pow(bp / heaviest, 0.7);
    // Most of a plasmid prep is supercoiled; a little is nicked.
    const GelForm form = gelBandForm(lane, band);
    if (form == GelForm::OpenCircular)
        alpha *= 0.4;
    if (bp > kMaxBp || bp < kMinBp)
        alpha *= 0.5;   // off the gel's useful range
    return std::min(1.0, alpha);
}

GelLayout::GelLayout(std::vector<GelLane> lanes, const Theme &theme, TextMeasurer &tm, double width, double height)
    : m_lanes(std::move(lanes)), m_theme(theme), m_w(width), m_h(height)
{
    const Font small{10, false, false};
    const double labelW = tm.width("10 kb", small) + 12;
    const int n = std::max<int>(1, int(m_lanes.size()));
    const double room = std::max(60.0, width - labelW - 8);
    m_laneGap = std::min(22.0, room / (n * 5.0));
    m_laneW = std::min(64.0, (room - m_laneGap * (n + 1)) / n);
    // The gel is as wide as its lanes, centered, with the size labels on its left.
    m_gelW = n * m_laneW + (n + 1) * m_laneGap;
    m_gelX = labelW + (room - m_gelW) / 2;
    m_gelY = 34;
    m_gelH = std::max(80.0, height - m_gelY - 8);
    m_runTop = m_gelY + 22;
    m_runBottom = m_gelY + m_gelH - 14;
}

double GelLayout::yOfSize(double bp) const
{
    return m_runTop + std::min(1.0, gelRunFraction(bp)) * (m_runBottom - m_runTop);
}

Scene GelLayout::render() const
{
    Scene s;
    s.width = m_w;
    s.height = m_h;
    s.background = m_theme.background;
    const Color gel = Color::rgb(0x24, 0x1f, 0x31);
    const Color band = Color::rgb(0xff, 0xf4, 0xd8);
    s.rect({m_gelX, m_gelY, m_gelW, m_gelH}, gel, m_theme.featureOutline, 1);

    double x = m_gelX + m_laneGap;
    bool first = true;
    const Font nameFont{10.5, true, false};
    const Font small{10, false, false};
    for (const GelLane &lane : m_lanes) {
        const double cx = x + m_laneW / 2;
        s.label(lane.name, {cx, m_gelY - 8}, nameFont, m_theme.text, HAlign::Center, VAlign::Baseline);
        // Well.
        s.rect({x + 2, m_gelY + 8, m_laneW - 4, 6}, Color::rgb(0, 0, 0, 0.55));

        for (size_t i = 0; i < lane.bands.size(); ++i) {
            const int bp = lane.bands[i];
            const double run = std::min(1.0, gelRunFraction(bp, gelBandForm(lane, i)));
            const double y = m_runTop + run * (m_runBottom - m_runTop);
            const double alpha = gelBandIntensity(lane, i);
            s.rect({x + 3, y - 2.5, m_laneW - 6, 5}, band.withAlpha(std::min(1.0, alpha * 0.35)));
            s.rect({x + 3, y - 1.5, m_laneW - 6, 3}, band.withAlpha(std::min(1.0, alpha)));
            if (lane.ladder && first)
                s.label(bandText(bp), {m_gelX - 6, y}, small, m_theme.mutedText, HAlign::Right, VAlign::Middle);
        }
        if (lane.uncutCircular && !lane.bands.empty())
            s.label("uncut", {cx, m_gelY + m_gelH - 4}, small, Color::rgb(0xde, 0xdd, 0xda), HAlign::Center,
                    VAlign::Baseline);
        x += m_laneW + m_laneGap;
        first = false;
    }
    return s;
}

} // namespace oc::view
