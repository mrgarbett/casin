// SPDX-License-Identifier: GPL-3.0-or-later
// Printed pages: the map on the first page, then the sequence view split at
// line boundaries. Each page is one Scene of the given size with a header.
#pragma once

#include "maplayout.h"
#include "seqlayout.h"

namespace oc::view {

struct PrintSettings {
    double width = 760, height = 1060;   // layout units per page; the frontend scales to paper
    bool map = true, sequence = true;
    MapOptions mapOptions;
    bool complement = true;
    int frames = 0;
};

std::vector<Scene> printPages(const oc::Sequence &seq, const std::vector<CutSite> &sites,
                              const std::vector<ReadTrack> &reads, TextMeasurer &tm, const PrintSettings &ps);

} // namespace oc::view
