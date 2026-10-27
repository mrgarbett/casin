// SPDX-License-Identifier: GPL-3.0-or-later
// Restriction-ligation cloning: cut fragments with their sticky or blunt
// ends, flip them, check which ends can be ligated and join them.
#pragma once

#include "enzymes.h"

namespace oc {

// One end of a double-stranded fragment. `overhang` follows Enzyme::overhang()
// (> 0 = 5', < 0 = 3', 0 = blunt); `bases` is the single-stranded part, read
// on the fragment's top strand 5'->3'.
struct FragmentEnd {
    std::string enzyme;
    int overhang = 0;
    std::string bases;
};

// A fragment cut out of a sequence. `top` holds the top strand from the left
// top-strand cut to the right one, with the features inside; `bottom` holds
// the bottom strand, already reverse complemented, so flipping is a swap.
struct ClonePiece {
    Sequence top;
    Sequence bottom;
    FragmentEnd left, right;
    int length() const { return top.length(); }
};

// The piece between two cuts, reading left to right on the top strand. On a
// circular sequence the piece may run across the origin; `left` == `right`
// opens the circle at one site. Linear sequences need left before right.
bool cutPiece(const Sequence &seq, const CutSite &left, const CutSite &right, ClonePiece *out,
              std::string *error = nullptr);
ClonePiece flipPiece(const ClonePiece &p);

// Sticky ends need the same overhang type and bases; blunt joins blunt.
bool endsLigate(const FragmentEnd &right, const FragmentEnd &left);

// Joins the pieces in order and closes the circle (last right end to first
// left end). Returns false with *error naming the first junction that
// doesn't ligate.
bool ligateCircular(const std::vector<ClonePiece> &pieces, Sequence *out, std::string *error = nullptr);

} // namespace oc
