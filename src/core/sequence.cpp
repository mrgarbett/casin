// SPDX-License-Identifier: GPL-3.0-or-later
#include "sequence.h"

#include <ctime>
#include "iupac.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace oc {

// ---- Span ------------------------------------------------------------------

bool Span::contains(int pos, int n) const
{
    if (length <= 0)
        return false;
    if (pos >= start && pos < start + length)
        return true;
    return n > 0 && start + length > n && pos < start + length - n;
}

bool Span::overlaps(const Span &o, int n) const
{
    if (length <= 0 || o.length <= 0)
        return false;
    auto pieces = [n](const Span &s, Range out[2]) {
        if (n > 0 && s.start + s.length > n) {
            out[0] = {s.start, n};
            out[1] = {0, std::min(s.start + s.length - n, n)};
            return 2;
        }
        out[0] = {s.start, s.start + s.length};
        return 1;
    };
    Range a[2], b[2];
    int na = pieces(*this, a), nb = pieces(o, b);
    for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j)
            if (a[i].start < b[j].end && b[j].start < a[i].end)
                return true;
    return false;
}

// ---- Feature ---------------------------------------------------------------

std::string Feature::qualifier(const std::string &key) const
{
    for (const Qualifier &q : qualifiers)
        if (q.key == key)
            return q.value;
    return {};
}

bool Feature::hasQualifier(const std::string &key) const
{
    for (const Qualifier &q : qualifiers)
        if (q.key == key)
            return true;
    return false;
}

int Feature::totalLength() const
{
    int len = 0;
    for (const Range &r : ranges)
        len += r.length();
    return len;
}

Span Feature::span(int n, bool circular) const
{
    if (ranges.empty())
        return {};
    std::vector<Range> sorted = ranges;
    std::sort(sorted.begin(), sorted.end(),
              [](const Range &a, const Range &b) { return a.start < b.start; });

    if (!circular || n <= 0) {
        int lo = sorted.front().start, hi = sorted.front().end;
        for (const Range &r : sorted)
            hi = std::max(hi, r.end);
        return {lo, hi - lo};
    }

    // Merge overlapping pieces, then find the largest uncovered gap,
    // counting the one that runs across the origin.
    std::vector<Range> merged;
    for (const Range &r : sorted) {
        if (!merged.empty() && r.start <= merged.back().end)
            merged.back().end = std::max(merged.back().end, r.end);
        else
            merged.push_back(r);
    }
    int bestGap = -1, bestGapEnd = merged.front().start;
    for (size_t i = 0; i < merged.size(); ++i) {
        const Range &cur = merged[i];
        const Range &next = merged[(i + 1) % merged.size()];
        int gap = (i + 1 < merged.size()) ? next.start - cur.end : (n - cur.end) + next.start;
        if (gap > bestGap) {
            bestGap = gap;
            bestGapEnd = next.start;
        }
    }
    return {bestGapEnd % n, std::max(0, n - bestGap)};
}

// ---- Sequence --------------------------------------------------------------

std::string Sequence::slice(int start, int len) const
{
    const int n = length();
    if (n == 0 || len <= 0)
        return {};
    if (!circular) {
        start = std::clamp(start, 0, n);
        len = std::min(len, n - start);
        return bases.substr(size_t(start), size_t(len));
    }
    start = ((start % n) + n) % n;
    std::string out;
    out.reserve(size_t(len));
    while (len > 0) {
        int take = std::min(len, n - start);
        out.append(bases, size_t(start), size_t(take));
        len -= take;
        start = 0;
    }
    return out;
}

std::vector<int> Sequence::featurePositions(const Feature &f) const
{
    std::vector<int> pos;
    pos.reserve(size_t(std::max(0, f.totalLength())));
    const int n = length();
    for (const Range &r : f.ranges)
        for (int p = r.start; p < r.end; ++p)
            if (p >= 0 && p < n)
                pos.push_back(p);
    if (f.strand == Strand::Reverse)
        std::reverse(pos.begin(), pos.end());
    return pos;
}

std::string Sequence::featureBases(const Feature &f) const
{
    const std::vector<int> pos = featurePositions(f);
    std::string out;
    out.reserve(pos.size());
    const bool rev = f.strand == Strand::Reverse;
    for (int p : pos)
        out.push_back(rev ? complementBase(bases[size_t(p)]) : bases[size_t(p)]);
    return out;
}

// ---- helpers ---------------------------------------------------------------

char complementBase(char b)
{
    const bool lower = b >= 'a' && b <= 'z';
    char u = lower ? char(b - 32) : b;
    char c;
    switch (u) {
    case 'A': c = 'T'; break;
    case 'T': case 'U': c = 'A'; break;
    case 'G': c = 'C'; break;
    case 'C': c = 'G'; break;
    case 'R': c = 'Y'; break;
    case 'Y': c = 'R'; break;
    case 'K': c = 'M'; break;
    case 'M': c = 'K'; break;
    case 'B': c = 'V'; break;
    case 'V': c = 'B'; break;
    case 'D': c = 'H'; break;
    case 'H': c = 'D'; break;
    case 'S': c = 'S'; break;
    case 'W': c = 'W'; break;
    case 'N': c = 'N'; break;
    default: return b;
    }
    return lower ? char(c + 32) : c;
}

std::string complement(const std::string &bases)
{
    std::string out(bases);
    for (char &c : out)
        c = complementBase(c);
    return out;
}

std::string reverseComplement(const std::string &bases)
{
    std::string out(bases.rbegin(), bases.rend());
    for (char &c : out)
        c = complementBase(c);
    return out;
}

static const char kStandardCode[] =
    "FFLLSSSSYY**CC*WLLLLPPPPHHQQRRRRIIIMTTTTNNKKSSRRVVVVAAAADDEEGGGG";

char translateCodon(const char *codon)
{
    unsigned masks[3];
    for (int i = 0; i < 3; ++i) {
        masks[i] = iupacMask(codon[i]);
        if (!masks[i])
            return 'X';
    }
    static const unsigned order[4] = {8, 2, 1, 4};   // T C A G
    char result = 0;
    for (int a = 0; a < 4; ++a) {
        if (!(masks[0] & order[a])) continue;
        for (int b = 0; b < 4; ++b) {
            if (!(masks[1] & order[b])) continue;
            for (int c = 0; c < 4; ++c) {
                if (!(masks[2] & order[c])) continue;
                char aa = kStandardCode[a * 16 + b * 4 + c];
                if (result == 0)
                    result = aa;
                else if (result != aa)
                    return 'X';
            }
        }
    }
    return result ? result : 'X';
}

std::string translate(const std::string &bases)
{
    std::string out;
    out.reserve(bases.size() / 3);
    for (size_t i = 0; i + 2 < bases.size(); i += 3)
        out.push_back(translateCodon(bases.data() + i));
    return out;
}

double gcFraction(const std::string &bases)
{
    int gc = 0, total = 0;
    for (char c : bases) {
        switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'G': case 'C': case 'S':
            ++gc; ++total; break;
        case 'A': case 'T': case 'W': case 'U':
            ++total; break;
        default: break;
        }
    }
    return total ? double(gc) / total : 0.0;
}

bool isIupacBase(char c)
{
    return iupacMask(c) != 0;
}

std::string cleanBases(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'a' && c <= 'z')
            c = char(c - 32);
        if (c == 'U')
            c = 'T';
        if (iupacMask(c))
            out.push_back(c);
    }
    return out;
}

std::string cleanPrimerBases(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const bool lower = c >= 'a' && c <= 'z';
        char u = lower ? char(c - 32) : c;
        if (u == 'U')
            u = 'T';
        if (iupacMask(u))
            out.push_back(lower ? char(u + 32) : u);
    }
    const bool hasLower = std::any_of(out.begin(), out.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    const bool hasUpper = std::any_of(out.begin(), out.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    return hasLower && hasUpper ? out : cleanBases(out);
}

std::string defaultFeatureName(const std::string &type, const std::vector<Qualifier> &qualifiers)
{
    static const char *keys[] = {"label", "gene", "product", "locus_tag", "standard_name", "note"};
    for (const char *k : keys)
        for (const Qualifier &q : qualifiers)
            if (q.key == k) {
                std::string v = trim(q.value);
                if (!v.empty())
                    return v;
            }
    return type;
}

std::vector<CodonMark> codonMarks(const Sequence &seq, const Feature &f)
{
    std::vector<CodonMark> marks;
    const std::vector<int> pos = seq.featurePositions(f);
    const std::string b = seq.featureBases(f);
    int codonStart = std::atoi(f.qualifier("codon_start").c_str());
    int offset = (codonStart >= 1 && codonStart <= 3) ? codonStart - 1 : 0;
    marks.reserve(pos.size() / 3);
    for (size_t i = size_t(offset); i + 2 < pos.size(); i += 3)
        marks.push_back({pos[i + 1], translateCodon(b.data() + i)});
    return marks;
}

const char *strandName(Strand s)
{
    switch (s) {
    case Strand::Forward: return "forward";
    case Strand::Reverse: return "reverse";
    default: return "none";
    }
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string toLower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s)
{
    for (char &c : s)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string &s, const std::string &prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> splitWhitespace(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])))
            ++j;
        if (j > i)
            out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string fileBaseName(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name = name.substr(0, dot);
    return name;
}

std::string fileExtension(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    return toLower(path.substr(dot + 1));
}

void addHistory(Sequence &seq, const std::string &step)
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
    seq.history.push_back(std::string(date) + " " + step);
}

} // namespace oc
