// SPDX-License-Identifier: GPL-3.0-or-later
// Restriction digests and the band pattern they give on an agarose gel.
#pragma once

#include "enzymes.h"

namespace oc {

struct DigestFragment {
    int start = 0;    // top-strand cut that begins the fragment (0 for a linear end)
    int length = 0;   // top-strand length
    std::string leftEnzyme, rightEnzyme;   // empty = the end of a linear sequence
    // Overhang at each end, as Enzyme::overhang(): > 0 = 5', < 0 = 3', 0 = blunt.
    int leftOverhang = 0, rightOverhang = 0;
};

struct DigestResult {
    std::vector<DigestFragment> fragments;   // in sequence order
    bool uncut = false;                      // no enzyme cut; a circular sequence stays circular
};

// Cuts with every given enzyme at once. Cut sites come from findSites().
DigestResult digest(const Sequence &seq, const std::vector<Enzyme> &enzymes);

struct Ladder {
    std::string name;
    std::vector<int> bands;   // bp, largest first
    std::vector<int> bright;  // bands drawn brighter as reference points
};
// Generic size standards, sizes rounded to the values most ladders share.
const std::vector<Ladder> &commonLadders();

} // namespace oc
