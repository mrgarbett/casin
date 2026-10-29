// SPDX-License-Identifier: GPL-3.0-or-later
// Quick quality checks for a PCR or sequencing primer: length, GC, Tm, the
// 3' clamp, runs, and how strongly it can pair with itself or a partner.
// Pairing is scored as the longest run of Watson-Crick pairs, a simple and
// conservative stand-in for a full free-energy model.
#pragma once

#include "sequence.h"

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

// Primers that amplify exactly `target`: the forward primer starts at its
// first base and the reverse primer at its last. Lengths between 18 and 32
// are tried and the pair closest to `targetTm` with matching Tms, a 3' G or C
// and the fewest warnings wins. The target must not wrap the origin.
struct PrimerDesign {
    Primer forward, reverse;
    PrimerCheck forwardCheck, reverseCheck;
    PairCheck pair;
};

// With `flank` = 0 the primers sit exactly on the target's ends. With a
// flank, each primer may sit up to `flank` bases outside its end, so the
// product contains the target; nearer primers are preferred.
bool designPrimers(const Sequence &seq, Span target, double targetTm, PrimerDesign *out,
                   std::string *error = nullptr, int flank = 0);

// Site-directed mutagenesis with back-to-back primers (as in Q5-style kits):
// `target` is replaced by `replacement` (empty = deletion, empty target =
// insertion). The forward primer carries the new bases at its 5' end and
// anneals just after the target; the reverse primer anneals just before it.
// Annealing parts are sized for `targetTm`. The mutant keeps features and
// carries both primers.
struct MutagenesisDesign {
    Sequence mutant;
    Primer forward, reverse;      // binding sites on the template (annealing part)
    double forwardTm = 0, reverseTm = 0;   // of the annealing parts
    std::vector<std::string> warnings;
};

bool designMutagenesis(const Sequence &seq, Span target, const std::string &replacement, double targetTm,
                       MutagenesisDesign *out, std::string *error = nullptr);

// Where a primer can anneal: its 3' part matches the sequence, allowing a 5'
// tail that doesn't. Each hit is the 3' stretch (at least `minBinding`
// bases, at most `maxMismatches` mismatches, the last three bases exact)
// that scores best, counting a mismatch against three matches. Both strands are searched; hits are sorted by quality.
struct PrimerSite {
    Range binding;            // on the top strand
    Strand strand = Strand::Forward;
    int mismatches = 0;
};

std::vector<PrimerSite> findPrimerSites(const Sequence &seq, const std::string &primer, int maxMismatches = 2,
                                        int minBinding = 15);

// Grows or shrinks the part of `primer` that anneals at `site` so its Tm
// comes as close as it can to `targetTm`, between 15 and 40 bases. The 3' end
// and any 5' tail stay; the annealing part's 5' end moves along the template.
// Returns the new primer (its tail as it was) and sets *fitted to the new
// site. Returns `primer` unchanged when it doesn't anneal at `site`.
std::string fitAnnealingToTm(const Sequence &seq, const std::string &primer, const PrimerSite &site,
                             double targetTm, PrimerSite *fitted);

// Length of the product two primers make from these sites, tails included,
// or -1 when they don't face each other on opposite strands.
int pcrProductLength(const Sequence &seq, const PrimerSite &a, int tailA, const PrimerSite &b, int tailB);

// Whether a part in a primer's 5' tail (a codon or a tag) lands in frame with
// the CDS the primer's annealing part joins: the forward primer's 5' end, or
// the reverse primer's, within a CDS or at its ends. `partStart` and
// `partLength` count bases from the primer's 5' end; `partForward` is true
// when the part reads 5'->3' along the primer.
struct FrameCheck {
    bool applies = false;      // a CDS was found at the junction
    bool againstCds = false;   // the part reads the other way to the CDS
    bool inFrame = false;
    int fix = 0;               // bases to add between the part and the annealing part (1 or 2)
    std::string cds;           // the CDS's name
};

FrameCheck checkPartFrame(const Sequence &seq, const PrimerSite &site, int tailLength, int partStart,
                          int partLength, bool partForward);

} // namespace oc
