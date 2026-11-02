// SPDX-License-Identifier: GPL-3.0-or-later
// Gateway recombination cloning (BP and LR).
//
// Every att site of one number shares the same core, and the strands cross
// inside it, so a reaction is a swap: the piece of one molecule between its
// two cores changes places with the piece of the other between the same two.
// BP puts an attB-flanked insert into a donor and gives an entry clone; LR
// puts an entry clone's insert into a destination vector and gives an
// expression clone. Both are the same swap, so one function does them, and
// the reaction only decides what the new sites are called (B x P -> L and R,
// L x R -> B and P).
//
// Sites are found by their core, not by annotation, so a plasmid from any
// source works. att1 to att4 are known, which covers BP, LR and the
// three-fragment MultiSite order (attB4-attB1r, attB1-attB2, attB2r-attB3).
#pragma once

#include "sequence.h"

#include <string>
#include <vector>

namespace oc {

// Where an att core lies. `span` is the core on the top strand, whichever
// strand it reads on.
struct AttSite {
    int number = 0;          // 1 to 4
    Span span;
    bool reversed = false;   // the core reads 5'->3' on the bottom strand
};

// The core of att `number` (1 to 4), read 5'->3', or empty for another
// number. The cores are the stretch every att site of that number holds,
// whatever family it belongs to.
std::string attCore(int number);

// Every att core in the sequence, in top-strand order. A circular sequence
// is searched across its origin too.
std::vector<AttSite> findAttSites(const Sequence &seq);

enum class GatewayReaction { BP, LR };

struct GatewayResult {
    Sequence clone;        // what you keep: the entry clone, or the expression clone
    Sequence byproduct;    // the other circle, or empty when there isn't one
    int firstSite = 0;     // the att numbers the two molecules crossed at
    int secondSite = 0;
    bool insertFlipped = false;   // the insert only fitted read from its other strand
};

// Recombines `insert` (the attB-flanked fragment, or the entry clone) with
// `vector` (the donor, or the destination vector). Both must carry two att
// cores of the same two numbers, pointing the same way; the piece of
// `insert` between its cores ends up between the vector's. An insert whose
// sites face the other way is read from its other strand, and the result
// says so. Fails, with *error, when the sites don't match or don't line up.
bool gatewayRecombine(const Sequence &insert, const Sequence &vector, GatewayReaction reaction, GatewayResult *out,
                      std::string *error = nullptr);

} // namespace oc
