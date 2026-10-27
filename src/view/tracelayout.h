// SPDX-License-Identifier: GPL-3.0-or-later
// A Sanger chromatogram strip: base calls with their quality, and the four
// dye channels below, drawn as a Scene. The strip scrolls horizontally;
// render() draws just the visible part.
#pragma once

#include "scene.h"
#include "style.h"
#include "../core/abif.h"

namespace oc::view {

class TraceLayout {
public:
    TraceLayout(const Trace &trace, const Theme &theme, double height, double pixelsPerScan = 2.0);
    double totalWidth() const;
    // The visible window [scrollX, scrollX + viewWidth), shifted to x = 0.
    // Bases in `selection` (start, length) are highlighted.
    Scene render(double scrollX, double viewWidth, Span selection = {0, 0}) const;
    // Base under content x, or -1.
    int baseAt(double x) const;
    // Content x of a base's peak.
    double xOfBase(int base) const;

    static Color channelColor(int channel, bool dark);   // A, C, G, T

private:
    const Trace &m_trace;
    Theme m_theme;
    double m_h, m_px;
    double m_scale = 1;   // signal to pixels
    Span m_good;
};

} // namespace oc::view
