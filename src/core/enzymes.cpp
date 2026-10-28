// SPDX-License-Identifier: GPL-3.0-or-later
#include "enzymes.h"
#include "iupac.h"

#include <algorithm>
#include <cstdint>
#include <set>

namespace oc {

bool Enzyme::isPalindromic() const
{
    return reverseComplement(site) == site;
}

bool Enzyme::cutsOutsideSite() const
{
    const int L = siteLength();
    return cutTop < 0 || cutTop > L || cutBottom < 0 || cutBottom > L;
}

// Recognition sites and cut positions from REBASE (rebase.neb.com), the
// Restriction Enzyme Database, Roberts R.J. et al., Nucleic Acids Res.
// 51:D629-D630 (2023). Values were cross-checked against REBASE's EMBOSS
// export (emboss_e, version 404) by scripts/check_enzymes.py.
// Format: name, site, top cut, bottom cut, then the second pair for enzymes
// that cut twice (see Enzyme).
const std::vector<Enzyme> &commonEnzymes()
{
    static const std::vector<Enzyme> list = {
        {"AatII", "GACGTC", 5, 1},
        {"Acc65I", "GGTACC", 1, 5},
        {"AccI", "GTMKAC", 2, 4},
        {"AflII", "CTTAAG", 1, 5},
        {"AgeI", "ACCGGT", 1, 5},
        {"AhdI", "GACNNNNNGTC", 6, 5},
        {"AluI", "AGCT", 2, 2},
        {"ApaI", "GGGCCC", 5, 1},
        {"ApaLI", "GTGCAC", 1, 5},
        {"AscI", "GGCGCGCC", 2, 6},
        {"AseI", "ATTAAT", 2, 4},
        {"AvaI", "CYCGRG", 1, 5},
        {"AvrII", "CCTAGG", 1, 5},
        {"BamHI", "GGATCC", 1, 5},
        {"BanII", "GRGCYC", 5, 1},
        {"BclI", "TGATCA", 1, 5},
        {"BglI", "GCCNNNNNGGC", 7, 4},
        {"BglII", "AGATCT", 1, 5},
        {"BlpI", "GCTNAGC", 2, 5},
        {"BmtI", "GCTAGC", 5, 1},
        {"BsiWI", "CGTACG", 1, 5},
        {"BsmI", "GAATGC", 7, 5},
        {"BspHI", "TCATGA", 1, 5},
        {"BsrGI", "TGTACA", 1, 5},
        {"BssHII", "GCGCGC", 1, 5},
        {"BstBI", "TTCGAA", 2, 4},
        {"BstEII", "GGTNACC", 1, 6},
        {"BstXI", "CCANNNNNNTGG", 8, 4},
        {"BstZ17I", "GTATAC", 3, 3},
        {"ClaI", "ATCGAT", 2, 4},
        {"DraI", "TTTAAA", 3, 3},
        {"EagI", "CGGCCG", 1, 5},
        {"EcoNI", "CCTNNNNNAGG", 5, 6},
        {"EcoRI", "GAATTC", 1, 5},
        {"EcoRV", "GATATC", 3, 3},
        {"FseI", "GGCCGGCC", 6, 2},
        {"HaeIII", "GGCC", 2, 2},
        {"HincII", "GTYRAC", 3, 3},
        {"HindIII", "AAGCTT", 1, 5},
        {"HpaI", "GTTAAC", 3, 3},
        {"KasI", "GGCGCC", 1, 5},
        {"KpnI", "GGTACC", 5, 1},
        {"MfeI", "CAATTG", 1, 5},
        {"MluI", "ACGCGT", 1, 5},
        {"MscI", "TGGCCA", 3, 3},
        {"MspI", "CCGG", 1, 3},
        {"NaeI", "GCCGGC", 3, 3},
        {"NarI", "GGCGCC", 2, 4},
        {"NcoI", "CCATGG", 1, 5},
        {"NdeI", "CATATG", 2, 4},
        {"NheI", "GCTAGC", 1, 5},
        {"NotI", "GCGGCCGC", 2, 6},
        {"NruI", "TCGCGA", 3, 3},
        {"NsiI", "ATGCAT", 5, 1},
        {"PacI", "TTAATTAA", 5, 3},
        {"PciI", "ACATGT", 1, 5},
        {"PmeI", "GTTTAAAC", 4, 4},
        {"PmlI", "CACGTG", 3, 3},
        {"PsiI", "TTATAA", 3, 3},
        {"PstI", "CTGCAG", 5, 1},
        {"PvuI", "CGATCG", 4, 2},
        {"PvuII", "CAGCTG", 3, 3},
        {"SacI", "GAGCTC", 5, 1},
        {"SacII", "CCGCGG", 4, 2},
        {"SalI", "GTCGAC", 1, 5},
        {"SbfI", "CCTGCAGG", 6, 2},
        {"ScaI", "AGTACT", 3, 3},
        {"SfiI", "GGCCNNNNNGGCC", 8, 5},
        {"SgrAI", "CRCCGGYG", 2, 6},
        {"SmaI", "CCCGGG", 3, 3},
        {"SnaBI", "TACGTA", 3, 3},
        {"SpeI", "ACTAGT", 1, 5},
        {"SphI", "GCATGC", 5, 1},
        {"SrfI", "GCCCGGGC", 4, 4},
        {"SspI", "AATATT", 3, 3},
        {"StuI", "AGGCCT", 3, 3},
        {"SwaI", "ATTTAAAT", 4, 4},
        {"XbaI", "TCTAGA", 1, 5},
        {"XhoI", "CTCGAG", 1, 5},
        {"XmaI", "CCCGGG", 1, 5},
        {"XmnI", "GAANNNNTTC", 5, 5},
        {"ZraI", "GACGTC", 3, 3},
        // Type IIS
        {"AarI", "CACCTGC", 11, 15},
        {"BbsI", "GAAGAC", 8, 12},
        {"BsaI", "GGTCTC", 7, 11},
        {"BsmBI", "CGTCTC", 7, 11},
        {"BspMI", "ACCTGC", 10, 14},
        {"BtgZI", "GCGATG", 16, 20},
        {"Esp3I", "CGTCTC", 7, 11},
        {"FokI", "GGATG", 14, 18},
        {"HgaI", "GACGC", 10, 15},
        {"MlyI", "GAGTC", 10, 10},
        {"PaqCI", "CACCTGC", 11, 15},
        {"SapI", "GCTCTTC", 8, 11},
        // Cut on both sides of the site, excising it
        {"BaeI", "ACNNNNGTAYC", -10, -15, 23, 18, true},
        {"BcgI", "CGANNNNNNTGC", -10, -12, 24, 22, true},
        {"BsaXI", "ACNNNNNCTCC", -9, -12, 21, 18, true},
        {"CspCI", "CAANNNNNGTGG", -11, -13, 24, 22, true},
    };
    return list;
}

const Enzyme *findEnzyme(const std::string &name)
{
    for (const Enzyme &e : commonEnzymes())
        if (toLower(e.name) == toLower(name))
            return &e;
    return nullptr;
}

namespace {

std::vector<uint8_t> masksOf(const std::string &s)
{
    std::vector<uint8_t> m(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        m[i] = uint8_t(iupacMask(s[i]));
    return m;
}

// Sequence masks, extended by L-1 wrapped bases on circular sequences so
// matches that span the origin are found.
std::vector<uint8_t> extendedMasks(const Sequence &seq, int extra)
{
    std::vector<uint8_t> m = masksOf(seq.bases);
    const int n = seq.length();
    if (seq.circular && n > 0)
        for (int i = 0; i < extra; ++i)
            m.push_back(m[size_t(i % n)]);
    return m;
}

bool matchAt(const std::vector<uint8_t> &text, size_t p, const std::vector<uint8_t> &pat)
{
    for (size_t i = 0; i < pat.size(); ++i)
        if (!iupacMatches(pat[i], text[p + i]))
            return false;
    return true;
}

int mod(int a, int n) { return ((a % n) + n) % n; }

} // namespace

std::vector<CutSite> findSites(const Sequence &seq, const std::vector<Enzyme> &enzymes)
{
    std::vector<CutSite> out;
    const int n = seq.length();
    if (n == 0)
        return out;
    int maxLen = 0;
    for (const Enzyme &e : enzymes)
        maxLen = std::max(maxLen, e.siteLength());
    const std::vector<uint8_t> text = extendedMasks(seq, maxLen);

    for (const Enzyme &e : enzymes) {
        const int L = e.siteLength();
        if (L == 0 || L > n)
            continue;
        const std::vector<uint8_t> fwd = masksOf(e.site);
        const bool palindrome = e.isPalindromic();
        const std::vector<uint8_t> rev = palindrome ? fwd : masksOf(reverseComplement(e.site));
        const int lastStart = seq.circular ? n - 1 : n - L;

        auto addCut = [&](int p, Strand strand, int cutTop, int cutBottom) {
            int top, bottom;
            if (strand == Strand::Forward) {
                top = p + cutTop;
                bottom = p + cutBottom;
            } else {
                top = p + L - cutBottom;
                bottom = p + L - cutTop;
            }
            if (seq.circular) {
                top = mod(top, n);
                bottom = mod(bottom, n);
            } else if (top <= 0 || top >= n || bottom <= 0 || bottom >= n) {
                return;
            }
            out.push_back({&e, p, top, bottom, strand});
        };
        auto add = [&](int p, Strand strand) {
            addCut(p, strand, e.cutTop, e.cutBottom);
            if (e.cutsTwice)
                addCut(p, strand, e.cutTop2, e.cutBottom2);
        };

        for (int p = 0; p <= lastStart; ++p) {
            if (matchAt(text, size_t(p), fwd))
                add(p, Strand::Forward);
            if (!palindrome && matchAt(text, size_t(p), rev))
                add(p, Strand::Reverse);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const CutSite &a, const CutSite &b) {
        if (a.cutTop != b.cutTop)
            return a.cutTop < b.cutTop;
        return a.enzyme->name < b.enzyme->name;
    });
    return out;
}

std::vector<PatternMatch> findPattern(const Sequence &seq, const std::string &pattern)
{
    std::vector<PatternMatch> out;
    const std::string pat = cleanBases(pattern);
    const int n = seq.length(), L = int(pat.size());
    if (L == 0 || L > n)
        return out;
    const std::vector<uint8_t> text = extendedMasks(seq, L);
    const std::vector<uint8_t> fwd = masksOf(pat);
    const std::string rc = reverseComplement(pat);
    const bool palindrome = rc == pat;
    const std::vector<uint8_t> rev = masksOf(rc);
    const int lastStart = seq.circular ? n - 1 : n - L;
    for (int p = 0; p <= lastStart; ++p) {
        if (matchAt(text, size_t(p), fwd))
            out.push_back({p, L, Strand::Forward});
        else if (!palindrome && matchAt(text, size_t(p), rev))
            out.push_back({p, L, Strand::Reverse});
    }
    return out;
}

int methylationBlock(const Sequence &seq, const CutSite &site)
{
    // Enzymes in commonEnzymes() whose cutting Dam or Dcm methylation blocks
    // or impairs (always, or when the motif overlaps their site).
    static const std::set<std::string> damSensitive = {"BclI", "BspHI", "ClaI", "NruI", "XbaI"};
    static const std::set<std::string> dcmSensitive = {"Acc65I", "ApaI", "BsaI", "MscI", "StuI"};
    if (!site.enzyme)
        return MethylNone;
    const bool dam = damSensitive.count(site.enzyme->name) > 0;
    const bool dcm = dcmSensitive.count(site.enzyme->name) > 0;
    const int n = seq.length();
    if ((!dam && !dcm) || n == 0)
        return MethylNone;
    const int a = site.matchStart, len = site.enzyme->siteLength();
    auto at = [&](int p) -> char {
        if (seq.circular)
            return seq.bases[size_t(((p % n) + n) % n)];
        return p >= 0 && p < n ? seq.bases[size_t(p)] : 'N';
    };
    auto inSite = [&](int p) { return p >= a && p < a + len; };
    int block = MethylNone;
    // Every motif that could put a methylated base inside the site starts at
    // most 4 bases before it.
    for (int m = a - 4; m < a + len; ++m) {
        if (dam && at(m) == 'G' && at(m + 1) == 'A' && at(m + 2) == 'T' && at(m + 3) == 'C'
            && (inSite(m + 1) || inSite(m + 2)))
            block |= MethylDam;   // the A on the top strand, and the A opposite the T
        const char w = at(m + 2);
        if (dcm && at(m) == 'C' && at(m + 1) == 'C' && (w == 'A' || w == 'T') && at(m + 3) == 'G'
            && at(m + 4) == 'G' && (inSite(m + 1) || inSite(m + 3)))
            block |= MethylDcm;   // the inner C, and the inner C opposite the G
    }
    return block;
}

std::string methylationName(int block)
{
    if (block == (MethylDam | MethylDcm))
        return "Dam and Dcm";
    return block == MethylDam ? "Dam" : block == MethylDcm ? "Dcm" : "";
}

} // namespace oc
