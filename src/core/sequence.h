// OpenClone - open-source plasmid viewer
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace oc {

// 0-based, half-open interval [start, end). Never wraps the origin: a feature
// that crosses the origin of a circular sequence is stored as two Ranges.
struct Range {
    int start = 0;
    int end = 0;

    int length() const { return end - start; }
    bool contains(int pos) const { return pos >= start && pos < end; }
    bool operator==(const Range &o) const { return start == o.start && end == o.end; }
    bool operator!=(const Range &o) const { return !(*this == o); }
};

enum class Strand { None, Forward, Reverse };

struct Qualifier {
    std::string key;
    std::string value;
    bool operator==(const Qualifier &o) const { return key == o.key && value == o.value; }
};

// A footprint on a sequence: start + length. Unlike Range, a Span may wrap
// the origin of a circular sequence (start + length > sequence length).
struct Span {
    int start = 0;
    int length = 0;

    bool empty() const { return length <= 0; }
    int end() const { return start + length; }   // exclusive; may exceed n when wrapping
    bool wraps(int n) const { return start + length > n; }
    bool contains(int pos, int n) const;
    bool overlaps(const Span &o, int n) const;
    bool operator==(const Span &o) const { return start == o.start && length == o.length; }
    bool operator!=(const Span &o) const { return !(*this == o); }
};

struct Feature {
    std::string type;
    std::string name;
    Strand strand = Strand::None;
    std::vector<Range> ranges;           // top-strand order
    std::vector<Qualifier> qualifiers;
    std::string color;                   // "#rrggbb"; empty = the type's default

    std::string qualifier(const std::string &key) const;   // first value, or empty
    bool hasQualifier(const std::string &key) const;
    int totalLength() const;             // sum of range lengths
    // Footprint of the feature. On a circular sequence of length n the span
    // begins just after the largest stretch the feature doesn't cover.
    Span span(int n, bool circular) const;
};

struct Primer {
    std::string name;
    std::string bases;
    Range binding;
    Strand strand = Strand::Forward;
};

class Sequence {
public:
    std::string name;
    std::string description;
    std::string bases;      // uppercase IUPAC
    bool circular = false;
    std::vector<Feature> features;
    std::vector<Primer> primers;

    int length() const { return int(bases.size()); }
    bool empty() const { return bases.empty(); }

    // `length` bases from `start`. Wraps the origin on circular sequences;
    // clipped at the end on linear ones.
    std::string slice(int start, int length) const;
    std::string slice(const Span &s) const { return slice(s.start, s.length); }
    // Positions covered by the feature, in 5'->3' order on its own strand.
    std::vector<int> featurePositions(const Feature &f) const;
    // The feature's bases read 5'->3' on its own strand.
    std::string featureBases(const Feature &f) const;
};

// ---- helpers ---------------------------------------------------------------

char complementBase(char b);                         // IUPAC aware, keeps case
std::string complement(const std::string &bases);
std::string reverseComplement(const std::string &bases);
// Standard genetic code. Ambiguous codons translate to the residue all their
// expansions agree on, else 'X'. Stops are '*'. A trailing partial codon is ignored.
std::string translate(const std::string &bases);
char translateCodon(const char *codon);
double gcFraction(const std::string &bases);         // S counts as GC; N etc. excluded
// Uppercase, U -> T, drops whitespace, digits and anything that isn't IUPAC.
std::string cleanBases(const std::string &raw);
bool isIupacBase(char c);

// label, gene, product, locus_tag, standard_name, note, then the type.
std::string defaultFeatureName(const std::string &type, const std::vector<Qualifier> &qualifiers);

struct CodonMark {
    int position;      // the codon's middle base, in sequence coordinates
    char aminoAcid;
};
// One mark per complete codon of the feature, honoring /codon_start.
std::vector<CodonMark> codonMarks(const Sequence &seq, const Feature &f);

const char *strandName(Strand s);

// Small string utilities shared by the readers.
std::string trim(const std::string &s);
std::string toLower(std::string s);
std::string toUpper(std::string s);
bool startsWith(const std::string &s, const std::string &prefix);
bool endsWith(const std::string &s, const std::string &suffix);
std::vector<std::string> splitWhitespace(const std::string &s);
std::string fileBaseName(const std::string &path);   // "dir/x.gb" -> "x"
std::string fileExtension(const std::string &path);  // lowercase, without dot

} // namespace oc
