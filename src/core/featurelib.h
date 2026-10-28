// SPDX-License-Identifier: GPL-3.0-or-later
// A small library of common plasmid elements (promoters, origins, resistance
// genes, primer sites) and a search that finds them in a sequence.
#pragma once

#include "sequence.h"

namespace oc {

struct CommonElement {
    const char *name;
    const char *type;    // GenBank feature key
    const char *bases;   // top strand, as the element reads
};

const std::vector<CommonElement> &commonElements();

// Features for the common elements found in `seq`, on either strand and across
// the origin of a circular sequence. Long elements match at 95% identity or
// better (ungapped); short ones (under 40 bp) must match exactly. Elements
// already annotated (a feature of the same name or type over the same bases)
// are left out. Sorted by start.
std::vector<Feature> findCommonFeatures(const Sequence &seq);

} // namespace oc
