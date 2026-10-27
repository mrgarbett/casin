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

} // namespace oc
