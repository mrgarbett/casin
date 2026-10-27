// SPDX-License-Identifier: GPL-3.0-or-later
// Sequence edits that keep features and primers in step with the bases.
//
// Rules: a feature grows when bases go in strictly inside it (or at the
// origin of a feature that crosses it), shifts when they go in before it, and
// loses the bases deleted from it; a feature with nothing left is dropped.
// A primer moves with its binding site and is dropped when an edit touches
// the inside of that site.
#pragma once

#include "sequence.h"

namespace oc {

// Inserts `bases` (already clean) before boundary `pos` (0..n).
void insertBases(Sequence &seq, int pos, const std::string &bases);
// Deletes the span; it may wrap the origin of a circular sequence.
void deleteBases(Sequence &seq, Span span);
// Deletes the span, then inserts `bases` where it started. Returns the
// boundary where the inserted bases begin.
int replaceBases(Sequence &seq, Span span, const std::string &bases);
// Flips the whole molecule: bases, features and primers.
void reverseComplementSequence(Sequence &seq);
// Circular sequences only: base `origin` becomes base 0.
void rotateOrigin(Sequence &seq, int origin);

// Orders ranges in top-strand order, starting just after the feature's
// largest uncovered gap (origin-crossing pieces come out end-then-start).
void sortRangesTopStrand(std::vector<Range> &ranges, int n, bool circular);

} // namespace oc
