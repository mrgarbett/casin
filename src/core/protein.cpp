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

// Dipeptide instability weights (DIWV), Guruprasad et al., Protein Eng. 4:155 (1990).
const char kResidues[] = "ACDEFGHIKLMNPQRSTVWY";
const double kDiwv[20][20] = {
    // Rows: the first residue, columns: the next; both in "ACDEFGHIKLMNPQRSTVWY" order.
        {1, 44.94, -7.49, 1, 1, 1, -7.49, 1, 1, 1, 1, 1, 20.26, 1, 1, 1, 1, 1, 1, 1},   // A
        {1, 1, 20.26, 1, 1, 1, 33.6, 1, 1, 20.26, 33.6, 1, 20.26, -6.54, 1, 1, 33.6, -6.54, 24.68, 1},   // C
        {1, 1, 1, 1, -6.54, 1, 1, 1, -7.49, 1, 1, 1, 1, 1, -6.54, 20.26, -14.03, 1, 1, 1},   // D
        {1, 44.94, 20.26, 33.6, 1, 1, -6.54, 20.26, 1, 1, 1, 1, 20.26, 20.26, 1, 20.26, 1, 1, -14.03, 1},   // E
        {1, 1, 13.34, 1, 1, 1, 1, 1, -14.03, 1, 1, 1, 20.26, 1, 1, 1, 1, 1, 1, 33.601},   // F
        {-7.49, 1, 1, -6.54, 1, 13.34, 1, -7.49, -7.49, 1, 1, -7.49, 1, 1, 1, 1, -7.49, 1, 13.34, -7.49},   // G
        {1, 1, 1, 1, -9.37, -9.37, 1, 44.94, 24.68, 1, 1, 24.68, -1.88, 1, 1, 1, -6.54, 1, -1.88, 44.94},   // H
        {1, 1, 1, 44.94, 1, 1, 13.34, 1, -7.49, 20.26, 1, 1, -1.88, 1, 1, 1, 1, -7.49, 1, 1},   // I
        {1, 1, 1, 1, 1, -7.49, 1, -7.49, 1, -7.49, 33.6, 1, -6.54, 24.64, 33.6, 1, 1, -7.49, 1, 1},   // K
        {1, 1, 1, 1, 1, 1, 1, 1, -7.49, 1, 1, 1, 20.26, 33.6, 20.26, 1, 1, 1, 24.68, 1},   // L
        {13.34, 1, 1, 1, 1, 1, 58.28, 1, 1, 1, -1.88, 1, 44.94, -6.54, -6.54, 44.94, -1.88, 1, 1, 24.68},   // M
        {1, -1.88, 1, 1, -14.03, -14.03, 1, 44.94, 24.68, 1, 1, 1, -1.88, -6.54, 1, 1, -7.49, 1, -9.37, 1},   // N
        {20.26, -6.54, -6.54, 18.38, 20.26, 1, 1, 1, 1, 1, -6.54, 1, 20.26, 20.26, -6.54, 20.26, 1, 20.26, -1.88, 1},   // P
        {1, -6.54, 20.26, 20.26, -6.54, 1, 1, 1, 1, 1, 1, 1, 20.26, 20.26, 1, 44.94, 1, -6.54, 1, -6.54},   // Q
        {1, 1, 1, 1, 1, -7.49, 20.26, 1, 1, 1, 1, 13.34, 20.26, 20.26, 58.28, 44.94, 1, 1, 58.28, -6.54},   // R
        {1, 33.6, 1, 20.26, 1, 1, 1, 1, 1, 1, 1, 1, 44.94, 20.26, 20.26, 20.26, 1, 1, 1, 1},   // S
        {1, 1, 1, 20.26, 13.34, -7.49, 1, 1, 1, 1, 1, -14.03, 1, -6.54, 1, 1, 1, 1, -14.03, 1},   // T
        {1, 1, -14.03, 1, 1, -7.49, 1, 1, -1.88, 1, 1, 1, 20.26, 1, 1, 1, -7.49, 1, 1, -6.54},   // V
        {-14.03, 1, 1, 1, 1, -9.37, 24.68, 1, 1, 13.34, 24.68, 13.34, 1, 1, 1, 1, -14.03, -7.49, 1, 1},   // W
        {24.68, 1, 24.68, -6.54, 1, -7.49, 13.34, 1, 1, 1, 44.94, 1, 13.34, 1, -15.91, 1, -7.49, 1, -9.37, 13.34},   // Y
};

// Kyte-Doolittle hydropathy, in kResidues order.
const double kHydropathy[20] = {1.8, 2.5, -3.5, -3.5, 2.8, -0.4, -3.2, 4.5, -3.9, 3.8,
                                1.9, -3.5, -1.6, -3.5, -4.5, -0.8, -0.7, 4.2, -0.9, -1.3};

int residueIndex(char aa)
{
    for (int i = 0; i < 20; ++i)
        if (kResidues[i] == aa)
            return i;
    return -1;
}

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
    double sum = 0, hydropathy = 0;
    int known = 0, a = 0, v = 0, il = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const int r = residueIndex(p[i]);
        if (r < 0)
            continue;
        ++known;
        hydropathy += kHydropathy[r];
        a += p[i] == 'A';
        v += p[i] == 'V';
        il += p[i] == 'I' || p[i] == 'L';
        const int next = i + 1 < p.size() ? residueIndex(p[i + 1]) : -1;
        if (next >= 0)
            sum += kDiwv[r][next];
    }
    s.instability = 10.0 / s.length * sum;
    if (known > 0) {
        s.gravy = hydropathy / known;
        s.aliphatic = 100.0 * (a + 2.9 * v + 3.9 * il) / known;
    }
    return s;
}

} // namespace oc
