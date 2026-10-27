// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "scene.h"
#include "../core/sequence.h"

namespace oc::view {

// OpenClone's own palette, based on the GNOME HIG color palette.
struct FeatureStyle {
    // Default fill for a feature type; `explicitColor` ("#rrggbb") wins when set.
    static Color fill(const std::string &type, const std::string &explicitColor = {});
    // Black or white, whichever reads better on `fill`.
    static Color textOn(const Color &fill);
    // "source" features cover the whole record and are not drawn.
    static bool isDrawn(const Feature &f);
    static bool showsTranslation(const Feature &f) { return f.type == "CDS"; }
};

struct Theme {
    bool dark = false;
    Color background, surface, text, mutedText, backbone, tick, selection, selectionEdge,
        enzymeLabel, enzymeUnique, enzymeTypeIIS, leader, cutMarker, featureOutline, accent, primer;

    static Theme light();
    static Theme darkTheme();
};

} // namespace oc::view
