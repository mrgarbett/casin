// SPDX-License-Identifier: GPL-3.0-or-later
#include "featurelib.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace oc {

// The long elements come from pBAD30 (GenBank X81837) as shipped with
// Biopython's test data, and the pMB1 (ColE1/pBR322/pUC) origin from the
// Addgene pFA6a-kanMX6 record in the same test data; the short ones are the
// standard published sites.
const std::vector<CommonElement> &commonElements()
{
    static const std::vector<CommonElement> els = {
        {"AmpR", "CDS",
            "ATGAGTATTCAACATTTCCGTGTCGCCCTTATTCCCTTTTTTGCGGCATTTTGCCTTCCTGTTTTTGCTC"
            "ACCCAGAAACGCTGGTGAAAGTAAAAGATGCTGAAGATCAGTTGGGTGCACGAGTGGGTTACATCGAACT"
            "GGATCTCAACAGCGGTAAGATCCTTGAGAGTTTTCGCCCCGAAGAACGTTTTCCAATGATGAGCACTTTT"
            "AAAGTTCTGCTATGTGGCGCGGTATTATCCCGTGTTGACGCCGGGCAAGAGCAACTCGGTCGCCGCATAC"
            "ACTATTCTCAGAATGACTTGGTTGAGTACTCACCAGTCACAGAAAAGCATCTTACGGATGGCATGACAGT"
            "AAGAGAATTATGCAGTGCTGCCATAACCATGAGTGATAACACTGCGGCCAACTTACTTCTGACAACGATC"
            "GGAGGACCGAAGGAGCTAACCGCTTTTTTGCACAACATGGGGGATCATGTAACTCGCCTTGATCGTTGGG"
            "AACCGGAGCTGAATGAAGCCATACCAAACGACGAGCGTGACACCACGATGCCTGCAGCAATGGCAACAAC"
            "GTTGCGCAAACTATTAACTGGCGAACTACTTACTCTAGCTTCCCGGCAACAATTAATAGACTGGATGGAG"
            "GCGGATAAAGTTGCAGGACCACTTCTGCGCTCGGCCCTTCCGGCTGGCTGGTTTATTGCTGATAAATCTG"
            "GAGCCGGTGAGCGTGGGTCTCGCGGTATCATTGCAGCACTGGGGCCAGATGGTAAGCCCTCCCGTATCGT"
            "AGTTATCTACACGACGGGGAGTCAGGCAACTATGGATGAACGAAATAGACAGATCGCTGAGATAGGTGCC"
            "TCACTGATTAAGCATTGGTAACTG"},
        {"araC", "CDS",
            "ATGGCTGAAGCGCAAAATGATCCCCTGCTGCCGGGATACTCGTTTAATGCCCATCTGGTGGCGGGTTTAA"
            "CGCCGATTGAGGCCAACGGTTATCTCGATTTTTTTATCGACCGACCGCTGGGAATGAAAGGTTATATTCT"
            "CAATCTCACCATTCGCGGTCAGGGGGTGGTGAAAAATCAGGGACGAGAATTTGTTTGCCGACCGGGTGAT"
            "ATTTTGCTGTTCCCGCCAGGAGAGATTCATCACTACGGTCGTCATCCGGAGGCTCGCGAATGGTATCACC"
            "AGTGGGTTTACTTTCGTCCGCGCGCCTACTGGCATGAATGGCTTAACTGGCCGTCAATATTTGCCAATAC"
            "GGGGTTCTTTCGCCCGGATGAAGCGCACCAGCCGCATTTCAGCGACCTGTTTGGGCAAATCATTAACGCC"
            "GGGCAAGGGGAAGGGCGCTATTCGGAGCTGCTGGCGATAAATCTGCTTGAGCAATTGTTACTGCGGCGCA"
            "TGGAAGCGATTAACGAGTCGCTCCATCCACCGATGGATAATCGGGTACGCGAGGCTTGTCAGTACATCAG"
            "CGATCACCTGGCAGACAGCAATTTTGATATCGCCAGCGTCGCACAGCATGTTTGCTTGTCGCCGTCGCGT"
            "CTGTCACATCTTTTCCGCCAGCAGTTAGGGATTAGCGTCTTAAGCTGGCGCGAGGACCAACGTATCAGCC"
            "AGGCGAAGCTGCTTTTGAGCACCACCCGGATGCCTATCGCCACCGTCGGTCGCAATGTTGGTTTTGACGA"
            "TCAACTCTATTTCTCGCGGGTATTTAAAAAATGCACCGGGGCCAGCCCGAGCGAGTTCCGTGCCGGTTGT"
            "GAAGAAAAAGTGAATGATGTAGCCGTCAAGTTGTCATAA"},
        {"pMB1 ori", "rep_origin",
            "TTGAGATCCTTTTTTTCTGCGCGTAATCTGCTGCTTGCAAACAAAAAAACCACCGCTACCAGCGGTGGTT"
            "TGTTTGCCGGATCAAGAGCTACCAACTCTTTTTCCGAAGGTAACTGGCTTCAGCAGAGCGCAGATACCAA"
            "ATACTGTCCTTCTAGTGTAGCCGTAGTTAGGCCACCACTTCAAGAACTCTGTAGCACCGCCTACATACCT"
            "CGCTCTGCTAATCCTGTTACCAGTGGCTGCTGCCAGTGGCGATAAGTCGTGTCTTACCGGGTTGGACTCA"
            "AGACGATAGTTACCGGATAAGGCGCAGCGGTCGGGCTGAACGGGGGGTTCGTGCACACAGCCCAGCTTGG"
            "AGCGAACGACCTACACCGAACTGAGATACCTACAGCGTGAGCATTGAGAAAGCGCCACGCTTCCCGAAGG"
            "GAGAAAGGCGGACAGGTATCCGGTAAGCGGCAGGGTCGGAACAGGAGAGCGCACGAGGGAGCTTCCAGGG"
            "GGAAACGCCTGGTATCTTTATAGTCCTGTCGGGTTTCGCCACCTCTGACTTGAGCGTCGATTTTTGTGAT"
            "GCTCGTCAGGGGGGCGGAGCCTATGGAAA"},
        {"p15A ori", "rep_origin",
            "GCTAGCGGAGTGTATACTGGCTTACTATGTTGGCACTGATGAGGGTGTCAGTGAAGTGCTTCATGTGGCA"
            "GGAGAAAAAAGGCTGCACCGGTGCGTCAGCAGAATATGTGATACAGGATATATTCCGCTTCCTCGCTCAC"
            "TGACTCGCTACGCTCGGTCGTTCGACTGCGGCGAGCGGAAATGGCTTACGAACGGGGCGGAGATTTCCTG"
            "GAAGATGCCAGGAAGATACTTAACAGGGAAGTGAGAGGGCCGCGGCAAAGCCGTTTTTCCATAGGCTCCG"
            "CCCCCCTGACAAGCATCACGAAATCTGACGCTCAAATCAGTGGTGGCGAAACCCGACAGGACTATAAAGA"
            "TACCAGGCGTTTCCCCCTGGCGGCTCCCTCGTGCGCTCTCCTGTTCCTGCCTTTCGGTTTACCGGTGTCA"
            "TTCCGCTGTTATGGCCGCGTTTGTCTCATTCCACGCCTGACACTCAGTTCCGGGTAGGCAGTTCGCTCCA"
            "AGCTGGACTGTATGCACGAACCCCCCGTTCAGTCCGACCGCTGCGCCTTATCCGGTAACTATCGTCTTGA"
            "GTCCAACCCGGAAAGACATGCAAAAGCACCACTGGCAGCAGCCACTGGTAATTGATTTAGAGGAGTTAGT"
            "CTTGAAGTCATGCGCCGGTTAAGGCTAAACTGAAAGGACAAGTTTTGGTGACTGCGCTCCTCCAAGCCAG"
            "TTACCTCGGTTCAAAGAGTTGGTAGCTCAGAGAACCTTCGAAAAACCGCCCTGCAAGGCGGTTTTTTCGT"
            "TTTCAGAGCAAGAGATTACGCGCAGACCAAAACGATCTCAAGAAGATCATCTTATTAATCAGATAAAATA"
            "TTT"},
        {"f1 ori", "rep_origin",
            "TTTACGCGCCCTGTAGCGGCGCATTAAGCGCGGCGGGTGTGGTGGTTACGCGCAGCGTGACCGCTACACT"
            "TGCCAGCGCCCTAGCGCCCGCTCCTTTCGCTTTCTTCCCTTCCTTTCTCGCCACGTTCGCCGGCTTTCCC"
            "CGTCAAGCTCTAAATCGGGGGCTCCCTTTAGGGTTCCGATTTAGTGCTTTACGGCACCTCGACCCCAAAA"
            "AACTTGATTTGGGTGATGGTTCACGTAGTGGGCCATCGCCCTGATAGACGGTTTTTCGCCCTTTGACGTT"
            "GGAGTCCACGTTCTTTAATAGTGGACTCTTGTTCCAAACTTGAACAACACTCAACCCTATCTCGGGCTAT"
            "TCTTTTGATTTATAAGGGATTTTGCCGATTTCGGCCTATTGGTTAAAAAATGAGCTGATTTAACAAAAAT"
            "TTAACGCGAATTTTAACAAAATATTAACGTTTACAATTT"},
        {"rrnB T1/T2 terminator", "terminator",
            "GGCTGTTTTGGCGGATGAGAGAAGATTTTCAGCCTGATACAGATTAAATCAGAACGCAGAAGCGGTCTGA"
            "TAAAACAGAATTTGCCTGGCGGCAGTAGCGCGGTGGTCCCACCTGACCCCATGCCGAACTCAGAAGTGAA"
            "ACGCCGTAGCGCCGATGGTAGTGTGGGGTCTCCCCATGCGAGAGTAGGGAACTGCCAGGCATCAAATAAA"
            "ACGAAAGGCTCAGTCGAAAGACTGGGCCTTTCGTTTTATCTGTTGTTTGTCGGTGAACGCTCTCCTGAGT"
            "AGGACAAATCCGCCGGGAGCGGATTTGAACGTTGCGAAGCAACGGCCCGGAGGGTGGCGGGCAGGACGCC"
            "CGCCATAAACTGCCAGGCATCAAATTAAGCAGAAGGCCATCCTGACGGATGGCCTTTTTGCGTTTCTACA"
            "AACTCT"},
        {"pBAD promoter", "promoter",
            "GACGCTTTTTATCGCAACTCTCTACTGT"},
        {"T7 promoter", "promoter",
            "TAATACGACTCACTATAGG"},
        {"T3 promoter", "promoter",
            "AATTAACCCTCACTAAAGG"},
        {"SP6 promoter", "promoter",
            "ATTTAGGTGACACTATAG"},
        {"lac operator", "protein_bind",
            "TTGTGAGCGGATAACAA"},
        {"lac promoter", "promoter",
            "TTTACACTTTATGCTTCCGGCTCGTATGTTGTGTGG"},
        {"M13 fwd", "primer_bind",
            "GTAAAACGACGGCCAGT"},
        {"M13 rev", "primer_bind",
            "CAGGAAACAGCTATGAC"},
        {"T7 terminator", "terminator",
            "CTAGCATAACCCCTTGGGGCCTCTAAACGGGTCTTGAGGGGTTTTTTG"},
    };
    return els;
}

namespace {

constexpr int kSeed = 16;
constexpr int kShort = 40;

bool sameFootprint(const Feature &a, const Feature &b, int n, bool circular)
{
    const Span x = a.span(n, circular), y = b.span(n, circular);
    return x.start == y.start && x.length == y.length;
}

std::string lowerName(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<Feature> findCommonFeatures(const Sequence &seq)
{
    std::vector<Feature> found;
    const int n = seq.length();
    if (n < kSeed)
        return found;
    // Circular sequences are searched doubled, so matches can cross the origin.
    const std::string text = seq.circular ? seq.bases + seq.bases.substr(0, size_t(n - 1)) : seq.bases;
    std::unordered_map<std::string, std::vector<int>> index;
    for (int i = 0; i + kSeed <= int(text.size()); ++i)
        if (i < n)
            index[text.substr(size_t(i), kSeed)].push_back(i);

    for (const CommonElement &el : commonElements()) {
        const std::string fwd = el.bases;
        const int len = int(fwd.size());
        if (len > n)
            continue;
        for (const Strand strand : {Strand::Forward, Strand::Reverse}) {
            const std::string probe = strand == Strand::Forward ? fwd : reverseComplement(fwd);
            const int maxMismatch = len < kShort ? 0 : len / 20;
            std::vector<int> starts;
            for (const int off : {0, len / 4, len / 2, 3 * len / 4, len - kSeed}) {
                auto it = index.find(probe.substr(size_t(off), kSeed));
                if (it == index.end())
                    continue;
                for (const int p : it->second) {
                    const int s = p - off;
                    if (s < 0 || s + len > int(text.size()) || (!seq.circular && s + len > n))
                        continue;
                    if (std::find(starts.begin(), starts.end(), s) != starts.end())
                        continue;
                    starts.push_back(s);
                }
            }
            for (const int s : starts) {
                int mismatches = 0;
                for (int i = 0; i < len && mismatches <= maxMismatch; ++i)
                    mismatches += text[size_t(s + i)] != probe[size_t(i)];
                if (mismatches > maxMismatch)
                    continue;
                Feature f;
                f.type = el.type;
                f.name = el.name;
                f.strand = strand;
                if (s + len > n)
                    f.ranges = {{s, n}, {0, s + len - n}};
                else
                    f.ranges = {{s, s + len}};
                f.qualifiers.push_back({"label", el.name});
                const bool known = std::any_of(seq.features.begin(), seq.features.end(), [&](const Feature &g) {
                    return sameFootprint(f, g, n, seq.circular) &&
                           (g.type == f.type || lowerName(g.name) == lowerName(f.name));
                }) || std::any_of(found.begin(), found.end(), [&](const Feature &g) {
                    return sameFootprint(f, g, n, seq.circular) && g.name == f.name;
                });
                if (!known)
                    found.push_back(f);
            }
        }
    }
    std::sort(found.begin(), found.end(),
              [&](const Feature &a, const Feature &b) { return a.span(n, seq.circular).start < b.span(n, seq.circular).start; });
    return found;
}

} // namespace oc
