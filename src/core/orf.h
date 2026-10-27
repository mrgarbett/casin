// SPDX-License-Identifier: GPL-3.0-or-later
// Open reading frames: ATG to stop, on both strands, across the origin of a
// circular sequence.
#pragma once

#include "sequence.h"

namespace oc {

struct Orf {
    int start = 0;        // first base on the top strand (the ATG, or the stop for the reverse strand)
    int length = 0;       // bases including the stop codon; the span may wrap a circular sequence
    Strand strand = Strand::Forward;
    int frame = 0;        // 0..2: offset of the codon phase on its strand
    std::string protein;  // without the stop
};

// ORFs of at least `minCodons` codons (stop excluded), longest first. Nested
// ATGs in the same frame give one ORF, from the first ATG.
std::vector<Orf> findOrfs(const Sequence &seq, int minCodons = 100);

// An ORF as a CDS feature with /translation.
Feature orfFeature(const Sequence &seq, const Orf &orf, const std::string &name);

} // namespace oc
