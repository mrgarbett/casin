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
    int sourceStart = 0;    // where `top` starts in the sequence it was cut from, -1 once flipped
    int sourceLength = 0;   // length of that sequence
    int length() const { return top.length(); }
    // Offset of the source's origin inside `top`, or -1 when the piece doesn't span it.
    int originOffset() const;
};

// The piece between two cuts, reading left to right on the top strand. On a
// circular sequence the piece may run across the origin; `left` == `right`
// opens the circle at one site. Linear sequences need left before right.
bool cutPiece(const Sequence &seq, const CutSite &left, const CutSite &right, ClonePiece *out,
              std::string *error = nullptr);
ClonePiece flipPiece(const ClonePiece &p);

// What stays after cutting at two sites and dropping the other part: on a
// circular sequence the larger (`keepLarger`) or smaller arc, on a linear one
// the piece between the sites. Two cuts at one place open a circle there.
bool pieceBetween(const Sequence &seq, const CutSite &a, const CutSite &b, bool keepLarger, ClonePiece *out,
                  std::string *error = nullptr);

// Sticky ends need the same overhang type and bases; blunt joins blunt.
bool endsLigate(const FragmentEnd &right, const FragmentEnd &left);

// Joins the pieces in order and closes the circle (last right end to first
// left end). Returns false with *error naming the first junction that
// doesn't ligate.
bool ligateCircular(const std::vector<ClonePiece> &pieces, Sequence *out, std::string *error = nullptr);

// Golden Gate: each input is cut with one Type IIS enzyme and keeps the
// piece that has no recognition site left in it (the part between two
// inward-facing sites, or the vector backbone outside two outward-facing
// ones). The first input is the vector. The pieces are chained by their
// overhangs, flipping a part when its reverse complement fits, and the
// circle must close.
struct AssemblyStep {
    int input = 0;          // index into the inputs
    bool flipped = false;
    ClonePiece piece;       // as placed, already flipped when `flipped`
};

bool goldenGatePiece(const Sequence &seq, const Enzyme &enzyme, ClonePiece *out, std::string *error = nullptr);
bool goldenGate(const std::vector<Sequence> &inputs, const Enzyme &enzyme, Sequence *out,
                std::vector<AssemblyStep> *steps = nullptr, std::string *error = nullptr);

// Gibson / HiFi / In-Fusion: linear fragments joined by identical sequence
// at their ends. The first fragment (usually the linearized vector) keeps
// its orientation; each next one is the unused fragment, either way round,
// whose start repeats the end of the chain over `minOverlap` bases or more.
// The last fragment must overlap the first to close the circle.
struct GibsonJoin {
    int input = 0;
    bool flipped = false;
    int overlap = 0;      // bases shared with the fragment before it (the first: with the last)
};

bool gibsonAssemble(const std::vector<Sequence> &fragments, int minOverlap, Sequence *out,
                    std::vector<GibsonJoin> *joins = nullptr, std::string *error = nullptr);

} // namespace oc
