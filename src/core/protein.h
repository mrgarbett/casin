// SPDX-License-Identifier: GPL-3.0-or-later
// Protein properties from a one-letter amino acid sequence.
#pragma once

#include <string>

namespace oc {

struct ProteinStats {
    int length = 0;              // residues counted (up to the first stop)
    int unknown = 0;             // residues left out of the mass (X and other letters)
    bool truncated = false;      // an internal stop ended the protein early
    double mass = 0;             // average mass in daltons
    double pI = 0;               // isoelectric point
    int extinctionReduced = 0;   // at 280 nm, M⁻¹cm⁻¹, all cysteines reduced
    int extinctionCystines = 0;  // at 280 nm, every cysteine pair forming a cystine
    int positive = 0;            // K + R
    int negative = 0;            // D + E
};

// Stats of `protein` up to its first '*'. Average residue masses; pI from
// the EMBOSS pKa set; extinction coefficients after Pace et al. (1995).
ProteinStats proteinStats(const std::string &protein);

// Net charge at `pH` from the same pKa set.
double proteinCharge(const std::string &protein, double pH);

} // namespace oc
