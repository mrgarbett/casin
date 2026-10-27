// SPDX-License-Identifier: GPL-3.0-or-later
// Aligning a sequencing read to a reference: local alignment (Smith-Waterman
// with affine gap costs) on both strands, across the origin of a circular
// reference, and the differences it finds.
#pragma once

#include "sequence.h"

namespace oc {

struct AlignmentDiff {
    int refPos = 0;         // first reference base involved (for an insertion: the base after it)
    std::string ref;        // reference bases ("" for an insertion in the read)
    std::string read;       // read bases on the reference strand ("" for a deletion)
};

struct ReadAlignment {
    bool reverse = false;       // the read matches the bottom strand
    Span refSpan;               // aligned part of the reference (may wrap a circular one)
    int readStart = 0, readEnd = 0;   // aligned part of the read, in the read's own coordinates
    int matches = 0, mismatches = 0, gaps = 0;
    std::string refAligned, readAligned;   // with '-' for gaps, on the reference strand
    std::vector<AlignmentDiff> diffs;
    double identity() const
    {
        const int cols = int(refAligned.size());
        return cols ? double(matches) / cols : 0;
    }
};

// Returns false (with *error) when the read doesn't align well anywhere:
// fewer than `minMatches` matching bases.
bool alignRead(const Sequence &reference, const std::string &read, ReadAlignment *out, std::string *error = nullptr,
               int minMatches = 30);

} // namespace oc
