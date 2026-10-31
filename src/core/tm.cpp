// SPDX-License-Identifier: GPL-3.0-or-later
#include "tm.h"
#include "sequence.h"

#include <climits>
#include <cmath>
#include <limits>

namespace oc {

namespace {

struct NN {
    double dH;   // kcal/mol
    double dS;   // cal/(K*mol)
};

int idx(char b)
{
    switch (b) {
    case 'A': return 0;
    case 'C': return 1;
    case 'G': return 2;
    case 'T': return 3;
    default: return -1;
    }
}

// SantaLucia J. (1998) PNAS 95:1460-1465, Table 2 (unified parameters),
// indexed by the 5'->3' top-strand dinucleotide.
const NN kTable[4][4] = {
    //            A              C              G              T
    /* A */ {{-7.9, -22.2}, {-8.4, -22.4}, {-7.8, -21.0}, {-7.2, -20.4}},
    /* C */ {{-8.5, -22.7}, {-8.0, -19.9}, {-10.6, -27.2}, {-7.8, -21.0}},
    /* G */ {{-8.2, -22.2}, {-9.8, -24.4}, {-8.0, -19.9}, {-8.4, -22.4}},
    /* T */ {{-7.2, -21.3}, {-8.2, -22.2}, {-8.5, -22.7}, {-7.9, -22.2}},
};

const NN kInitGC = {0.1, -2.8};
const NN kInitAT = {2.3, 4.1};
const double kSymmetry = -1.4;
const double kR = 1.987;   // cal/(K*mol)

} // namespace

double meltingTemperature(const std::string &raw, const TmConditions &c)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::string s = toUpper(raw);
    const int n = int(s.size());
    if (n < 2 || c.sodiumMolar <= 0 || c.oligoMolar <= 0)
        return nan;
    for (char b : s)
        if (idx(b) < 0)
            return nan;

    double dH = 0, dS = 0;
    for (int i = 0; i + 1 < n; ++i) {
        const NN &p = kTable[idx(s[size_t(i)])][idx(s[size_t(i) + 1])];
        dH += p.dH;
        dS += p.dS;
    }
    for (char end : {s.front(), s.back()}) {
        const NN &init = (end == 'G' || end == 'C') ? kInitGC : kInitAT;
        dH += init.dH;
        dS += init.dS;
    }
    const bool selfComplementary = reverseComplement(s) == s;
    if (selfComplementary)
        dS += kSymmetry;
    dS += 0.368 * (n - 1) * std::log(c.sodiumMolar);

    const double x = selfComplementary ? 1.0 : 4.0;
    return dH * 1000.0 / (dS + kR * std::log(c.oligoMolar / x)) - 273.15;
}

double polymeraseTmExact(const std::string &raw, Polymerase p)
{
    const std::string s = toUpper(raw);
    const int n = int(s.size());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (n < 2)
        return nan;
    // Every base must be one of the four before any pair is looked up (the
    // pair at i reads base i + 1, which hasn't been checked yet).
    for (char c : s)
        if (idx(c) < 0)
            return nan;
    double dH = 0, dS = 0;
    int gc = 0;
    for (int i = 0; i < n; ++i) {
        gc += s[size_t(i)] == 'G' || s[size_t(i)] == 'C';
        if (i + 1 < n) {
            const NN &nn = kTable[idx(s[size_t(i)])][idx(s[size_t(i) + 1])];
            dH += nn.dH;
            dS += nn.dS;
        }
    }
    for (char end : {s.front(), s.back()}) {
        const NN &init = (end == 'G' || end == 'C') ? kInitGC : kInitAT;
        dH += init.dH;
        dS += init.dS;
    }
    // The primers are in excess, so their own concentration sets the Tm
    // (NEB divides it by 4 for Phusion only).
    const double primerMolar = p == Polymerase::Phusion ? 500e-9 / 4 : 500e-9;
    const double tm1M = dH * 1000.0 / (dS + kR * std::log(primerMolar)) - 273.15;
    double tm;
    if (p == Polymerase::Phusion) {
        tm = tm1M + 16.6 * std::log10(0.222);
    } else {
        const double ln = std::log(p == Polymerase::Q5 ? 0.150 : 0.055);
        const double fgc = double(gc) / n;
        tm = 1.0 / (1.0 / (tm1M + 273.15) + (4.29e-5 * fgc - 3.95e-5) * ln + 9.40e-6 * ln * ln) - 273.15;
    }
    return tm;
}

int polymeraseTm(const std::string &bases, Polymerase p)
{
    const double tm = polymeraseTmExact(bases, p);
    return std::isnan(tm) ? INT_MIN : int(std::lround(tm));
}

} // namespace oc
