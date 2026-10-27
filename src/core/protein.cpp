// SPDX-License-Identifier: GPL-3.0-or-later
#include "protein.h"

#include <cctype>
#include <cmath>

namespace oc {

namespace {
double residueMass(char aa)
{
    switch (aa) {
    case 'A': return 71.0788;
    case 'R': return 156.1875;
    case 'N': return 114.1038;
    case 'D': return 115.0886;
    case 'C': return 103.1388;
    case 'E': return 129.1155;
    case 'Q': return 128.1307;
    case 'G': return 57.0519;
    case 'H': return 137.1411;
    case 'I': return 113.1594;
    case 'L': return 113.1594;
    case 'K': return 128.1741;
    case 'M': return 131.1926;
    case 'F': return 147.1766;
    case 'P': return 97.1167;
    case 'S': return 87.0782;
    case 'T': return 101.1051;
    case 'W': return 186.2132;
    case 'Y': return 163.1760;
    case 'V': return 99.1326;
    default: return 0;
    }
}

const double kWater = 18.01524;

std::string upToStop(const std::string &protein, bool *truncated)
{
    std::string p;
    for (char c : protein) {
        if (c == '*') {
            *truncated = p.size() + 1 < protein.size();
            break;
        }
        if (!std::isspace(static_cast<unsigned char>(c)))
            p += char(std::toupper(static_cast<unsigned char>(c)));
    }
    return p;
}

double positive(double pH, double pK) { return 1 / (1 + std::pow(10.0, pH - pK)); }
double negative(double pH, double pK) { return -1 / (1 + std::pow(10.0, pK - pH)); }
} // namespace

double proteinCharge(const std::string &protein, double pH)
{
    bool truncated = false;
    const std::string p = upToStop(protein, &truncated);
    if (p.empty())
        return 0;
    double q = positive(pH, 8.6) + negative(pH, 3.6);   // the two ends
    for (char c : p) {
        switch (c) {
        case 'K': q += positive(pH, 10.8); break;
        case 'R': q += positive(pH, 12.5); break;
        case 'H': q += positive(pH, 6.5); break;
        case 'D': q += negative(pH, 3.9); break;
        case 'E': q += negative(pH, 4.1); break;
        case 'C': q += negative(pH, 8.5); break;
        case 'Y': q += negative(pH, 10.1); break;
        default: break;
        }
    }
    return q;
}

ProteinStats proteinStats(const std::string &protein)
{
    ProteinStats s;
    const std::string p = upToStop(protein, &s.truncated);
    int w = 0, y = 0, c = 0;
    for (char aa : p) {
        const double m = residueMass(aa);
        if (m == 0) {
            ++s.unknown;
            continue;
        }
        s.mass += m;
        w += aa == 'W';
        y += aa == 'Y';
        c += aa == 'C';
        s.positive += aa == 'K' || aa == 'R';
        s.negative += aa == 'D' || aa == 'E';
    }
    s.length = int(p.size());
    if (s.length == 0)
        return s;
    s.mass += kWater;
    s.extinctionReduced = 5500 * w + 1490 * y;
    s.extinctionCystines = s.extinctionReduced + 125 * (c / 2);
    // The charge falls as the pH rises, so bisect for zero.
    double lo = 0, hi = 14;
    for (int i = 0; i < 60; ++i) {
        const double mid = (lo + hi) / 2;
        (proteinCharge(p, mid) > 0 ? lo : hi) = mid;
    }
    s.pI = (lo + hi) / 2;
    return s;
}

} // namespace oc
