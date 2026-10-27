// SPDX-License-Identifier: GPL-3.0-or-later
// Quick quality checks for a PCR or sequencing primer: length, GC, Tm, the
// 3' clamp, runs, and how strongly it can pair with itself or a partner.
// Pairing is scored as the longest run of Watson-Crick pairs, a simple and
// conservative stand-in for a full free-energy model.
#pragma once

#include <string>
#include <vector>

namespace oc {

struct PrimerCheck {
    int length = 0;
    double gc = 0;            // fraction 0..1
    double tm = 0;            // deg C, NaN when not computable
    int clampGC = 0;          // G or C among the last 5 bases at the 3' end
    int longestRun = 0;       // longest run of one base
    int selfDimer = 0;        // longest run of pairs in any self-dimer
    int selfDimer3 = 0;       // the same, counting only dimers that pair the 3' end
    int hairpinStem = 0;      // longest hairpin stem with a loop of 3 or more
    std::vector<std::string> warnings;   // plain-language problems, empty when it looks fine
};

PrimerCheck checkPrimer(const std::string &bases);

struct PairCheck {
    int dimer = 0;     // longest run of pairs between the two primers
    int dimer3 = 0;    // only dimers that pair a 3' end
    double tmDifference = 0;
    std::vector<std::string> warnings;
};

PairCheck checkPrimerPair(const std::string &a, const std::string &b);

} // namespace oc
