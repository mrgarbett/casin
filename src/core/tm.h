// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace oc {

struct TmConditions {
    double sodiumMolar = 0.050;     // 50 mM Na+
    double oligoMolar = 500e-9;     // 500 nM strand concentration
};

// Nearest-neighbor melting temperature (deg C) using the SantaLucia (1998)
// unified parameters, terminal A/T and G/C initiation, a -1.4 e.u. symmetry
// correction for self-complementary sequences and the 0.368*(N-1)*ln[Na+]
// entropy salt correction. Returns NaN for sequences shorter than 2 bases
// or containing anything other than A, C, G, T.
double meltingTemperature(const std::string &bases, const TmConditions &c = {});

// The polymerases (and their buffers) whose annealing temperatures we
// suggest, as New England Biolabs' Tm calculator works them out.
enum class Polymerase { Q5, Phusion, Taq };

// A primer's Tm in a polymerase's buffer, as NEB's calculator gives it,
// whole degrees C: SantaLucia (1998) nearest neighbours at the buffer's
// primer concentration (1 M salt), then the buffer's salt: Owczarzy et al.
// (2004) for Q5 (150 mM, 500 nM primer) and Taq (51 mM, 200 nM), Schildkraut
// and Lifson (1965) for Phusion HF (222 mM, 500 nM / 4). Pass only the bases
// that anneal. Returns INT_MIN for fewer than 2 bases or anything but ACGT.
int polymeraseTm(const std::string &bases, Polymerase p);

} // namespace oc
