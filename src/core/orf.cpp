// SPDX-License-Identifier: GPL-3.0-or-later
#include "orf.h"

#include <algorithm>

namespace oc {

namespace {

bool isStop(const char *c)
{
    return (c[0] == 'T' && c[1] == 'A' && (c[2] == 'A' || c[2] == 'G')) || (c[0] == 'T' && c[1] == 'G' && c[2] == 'A');
}

// ORFs on one strand string `s` (length n, circular or not). Positions are
// in `s` coordinates.
void scanStrand(const std::string &s, bool circular, int minCodons, Strand strand, std::vector<Orf> &out)
{
    const int n = int(s.size());
    if (n < 6)
        return;
    // On a circle, read on past the end, up to one full turn.
    const std::string ext = circular ? s + s + s.substr(0, 2) : s;
    std::vector<bool> taken(size_t(n), false);   // ATGs already inside a reported ORF (same frame)
    for (int f = 0; f < 3; ++f) {
        for (int i = f; i + 3 <= n; i += 3) {
            if (s.compare(size_t(i), 3, "ATG") != 0 || taken[size_t(i)])
                continue;
            int j = i;
            bool closed = false;
            while (j + 3 <= int(ext.size()) && j - i < n) {
                if (isStop(ext.c_str() + j)) {
                    closed = true;
                    break;
                }
                j += 3;
            }
            if (!closed)
                continue;
            // Mark the in-frame ATGs it contains so they don't repeat it.
            for (int k = i; k < j; k += 3)
                if (ext.compare(size_t(k), 3, "ATG") == 0)
                    taken[size_t(k % n)] = true;
            const int codons = (j - i) / 3;
            if (codons < minCodons)
                continue;
            Orf o;
            o.length = j - i + 3;
            o.strand = strand;
            o.start = i;   // converted by the caller for the reverse strand
            o.frame = i % 3;
            o.protein = translate(ext.substr(size_t(i), size_t(j - i)));
            out.push_back(o);
        }
    }
}

} // namespace

std::vector<Orf> findOrfs(const Sequence &seq, int minCodons)
{
    std::vector<Orf> out;
    const int n = seq.length();
    scanStrand(seq.bases, seq.circular, minCodons, Strand::Forward, out);
    std::vector<Orf> rev;
    scanStrand(reverseComplement(seq.bases), seq.circular, minCodons, Strand::Reverse, rev);
    for (Orf &o : rev) {
        // Bottom-strand position p maps to top-strand base n - 1 - p.
        const int lastTop = n - 1 - o.start;            // the A of the ATG on the top strand
        o.start = ((lastTop - o.length + 1) % n + n) % n;
        out.push_back(o);
    }
    std::stable_sort(out.begin(), out.end(), [](const Orf &a, const Orf &b) { return a.length > b.length; });
    return out;
}

Feature orfFeature(const Sequence &seq, const Orf &orf, const std::string &name)
{
    const int n = seq.length();
    Feature f;
    f.name = name;
    f.type = "CDS";
    f.strand = orf.strand;
    const int end = orf.start + orf.length;
    f.ranges = end <= n ? std::vector<Range>{{orf.start, end}} : std::vector<Range>{{orf.start, n}, {0, end - n}};
    f.qualifiers = {{"label", name}, {"codon_start", "1"}, {"translation", orf.protein}};
    return f;
}

} // namespace oc
