// SPDX-License-Identifier: GPL-3.0-or-later
// Sanger sequencing traces in the ABIF format (.ab1), as written by Applied
// Biosystems capillary sequencers. The layout follows the public ABIF file
// format specification: a big-endian directory of tagged entries.
#pragma once

#include "sequence.h"

#include <array>

namespace oc {

struct Trace {
    std::string name;              // sample name (SMPL), may be empty
    std::string bases;             // base calls (PBAS 2, else PBAS 1)
    std::vector<int> quality;      // per base, Phred (PCON), may be empty
    std::vector<int> peaks;        // per base, scan index of its peak (PLOC)
    std::array<std::vector<int>, 4> channels;   // analysed traces for A, C, G, T
    int scans() const { return channels[0].empty() ? 0 : int(channels[0].size()); }
};

bool looksLikeAbif(const std::string &data);
bool readAbif(const std::string &data, Trace *out, std::string *error = nullptr);

// The stretch of good calls: trims both ends until a window of `window`
// bases averages at least `minQuality`. Length 0 when nothing is good.
Span goodRegion(const Trace &trace, int minQuality = 20, int window = 10);

} // namespace oc
