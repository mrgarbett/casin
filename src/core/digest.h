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

// Picks enzymes by what their cuts do to the sequence, for example "cuts once,
// inside the multiple cloning site, and never inside the insert".
struct EnzymeCriteria {
    int minCuts = 1;
    int maxCuts = 1;             // 0 = no upper limit
    bool inside = false;         // every cut must fall within `region`
    Span region;
    std::vector<Span> spare;     // no cut may fall strictly inside these
    enum class Ends { Any, FivePrime, ThreePrime, Blunt } ends = Ends::Any;
};

// Names of the matching enzymes, sorted by name. `sites` comes from
// findSites(seq, enzymes).
std::vector<std::string> chooseEnzymes(const Sequence &seq, const std::vector<Enzyme> &enzymes,
                                       const std::vector<CutSite> &sites, const EnzymeCriteria &criteria);

struct Ladder {
    std::string name;
    std::vector<int> bands;   // bp, largest first
    std::vector<int> bright;  // bands drawn brighter as reference points
};
// Generic size standards, sizes rounded to the values most ladders share.
const std::vector<Ladder> &commonLadders();

} // namespace oc
