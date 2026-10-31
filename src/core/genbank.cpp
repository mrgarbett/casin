// SPDX-License-Identifier: GPL-3.0-or-later
#include "genbank.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace oc {

// ---- location parsing ------------------------------------------------------

namespace {

struct LocItem {
    Range range;
    bool reverse = false;
};

class LocationParser {
public:
    LocationParser(const std::string &text, int n, bool circular) : m_n(n), m_circular(circular)
    {
        for (char c : text)
            if (!std::isspace(static_cast<unsigned char>(c)))
                m_s.push_back(c);
    }

    bool parse(std::vector<LocItem> *items, std::string *error)
    {
        if (!parseExpr(items)) {
            *error = m_error.empty() ? "cannot parse location" : m_error;
            return false;
        }
        if (m_pos != m_s.size()) {
            *error = "unexpected text at " + std::to_string(m_pos);
            return false;
        }
        return true;
    }

private:
    bool eat(const char *tok)
    {
        size_t len = std::char_traits<char>::length(tok);
        if (m_s.size() - m_pos < len)
            return false;
        for (size_t i = 0; i < len; ++i)
            if (std::tolower(static_cast<unsigned char>(m_s[m_pos + i])) != tok[i])
                return false;
        m_pos += len;
        return true;
    }

    bool parseInt(int *v)
    {
        size_t start = m_pos;
        long long value = 0;
        while (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos]))) {
            value = value * 10 + (m_s[m_pos] - '0');
            if (value > 2000000000LL)
                return false;
            ++m_pos;
        }
        if (m_pos == start) {
            m_error = "expected a number at " + std::to_string(m_pos);
            return false;
        }
        *v = int(value);
        return true;
    }

    bool parseList(std::vector<LocItem> *items)
    {
        do {
            if (!parseExpr(items))
                return false;
        } while (eat(","));
        if (!eat(")")) {
            m_error = "expected ')'";
            return false;
        }
        return true;
    }

    bool parseExpr(std::vector<LocItem> *items)
    {
        if (++m_depth > 64) {
            m_error = "location nested too deeply";
            return false;
        }
        bool ok;
        if (eat("complement(")) {
            std::vector<LocItem> inner;
            ok = parseList(&inner);
            std::reverse(inner.begin(), inner.end());
            for (LocItem &it : inner)
                it.reverse = !it.reverse;
            items->insert(items->end(), inner.begin(), inner.end());
        } else if (eat("join(") || eat("order(")) {
            ok = parseList(items);
        } else {
            ok = parseSimple(items);
        }
        --m_depth;
        return ok;
    }

    bool parseSimple(std::vector<LocItem> *items)
    {
        // Remote reference "ACC.1:12..34": parse and drop.
        bool remote = false;
        size_t stop = m_pos;
        while (stop < m_s.size() && m_s[stop] != ',' && m_s[stop] != ')')
            ++stop;
        size_t colon = m_s.find(':', m_pos);
        if (colon != std::string::npos && colon < stop) {
            remote = true;
            m_pos = colon + 1;
        }
        eat("<");
        eat(">");
        int a = 0, b = 0;
        if (!parseInt(&a))
            return false;
        b = a;
        bool between = false;
        if (eat("..")) {
            eat("<");
            eat(">");
            if (!parseInt(&b))
                return false;
        } else if (eat("^")) {
            if (!parseInt(&b))
                return false;
            between = true;
        } else if (eat(".")) {
            // "a.b": one base somewhere between a and b; take a.
            if (!parseInt(&b))
                return false;
            b = a;
        }
        if (remote)
            return true;
        if (a < 1 || b < 1 || (m_n > 0 && (a > m_n || b > m_n))) {
            m_error = "position out of range";
            return false;
        }
        if (between) {
            // Site between two bases: a zero-length range at the boundary after a.
            items->push_back({{a, a}, false});
            return true;
        }
        if (a <= b) {
            items->push_back({{a - 1, b}, false});
            return true;
        }
        if (!m_circular) {
            m_error = "start after end on a linear sequence";
            return false;
        }
        items->push_back({{a - 1, m_n}, false});
        items->push_back({{0, b}, false});
        return true;
    }

    std::string m_s;
    size_t m_pos = 0;
    int m_depth = 0;
    int m_n;
    bool m_circular;
    std::string m_error;
};

} // namespace

Location parseLocation(const std::string &text, int seqLength, bool circular)
{
    Location loc;
    std::vector<LocItem> items;
    LocationParser parser(text, seqLength, circular);
    if (!parser.parse(&items, &loc.error))
        return loc;
    if (items.empty()) {
        loc.error = "empty location";
        return loc;
    }
    size_t rev = 0;
    for (const LocItem &it : items)
        rev += it.reverse ? 1 : 0;
    const bool reverse = rev * 2 > items.size();
    loc.strand = reverse ? Strand::Reverse : Strand::Forward;
    // Reverse-strand items come out 5'->3' on that strand; store top-strand order.
    if (reverse)
        std::reverse(items.begin(), items.end());
    for (const LocItem &it : items)
        loc.ranges.push_back(it.range);
    loc.ok = true;
    return loc;
}

std::string formatLocation(const std::vector<Range> &ranges, Strand strand, int seqLength, bool circular)
{
    std::vector<std::string> parts;
    for (const Range &r : ranges) {
        if (r.length() == 0) {
            // A site between bases. Both ends of a circle are the one
            // boundary, written n^1 (0^1 isn't a location); a linear
            // sequence has nothing before base 1, so a site there is
            // written as base 1.
            if (circular && (r.start >= seqLength || r.start <= 0))
                parts.push_back(std::to_string(seqLength) + "^1");
            else if (r.start <= 0)
                parts.push_back("1");
            else
                parts.push_back(std::to_string(r.start) + "^" + std::to_string(r.start + 1));
        } else if (r.length() == 1) {
            parts.push_back(std::to_string(r.start + 1));
        } else {
            parts.push_back(std::to_string(r.start + 1) + ".." + std::to_string(r.end));
        }
    }
    std::string body;
    if (parts.size() == 1) {
        body = parts.front();
    } else {
        body = "join(";
        for (size_t i = 0; i < parts.size(); ++i)
            body += (i ? "," : "") + parts[i];
        body += ")";
    }
    if (strand == Strand::Reverse)
        body = "complement(" + body + ")";
    return body;
}

// ---- reader ----------------------------------------------------------------

namespace {

struct RawFeature {
    std::string type;
    std::string location;
    std::vector<Qualifier> qualifiers;
};

bool isQuoteComplete(const std::string &raw)
{
    if (raw.empty() || raw[0] != '"')
        return true;
    if (raw.size() < 2 || raw.back() != '"')
        return false;
    return std::count(raw.begin(), raw.end(), '"') % 2 == 0;
}

std::string unquote(std::string v)
{
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    else if (!v.empty() && v.front() == '"')
        v = v.substr(1);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        out.push_back(v[i]);
        if (v[i] == '"' && i + 1 < v.size() && v[i + 1] == '"')
            ++i;
    }
    return out;
}

// The /note that marks a primer_bind as one of the document's primers.
const char *const kPrimerNote = "sequence: ";

// A primer_bind whose note gives the primer's bases (see writeGenBank) is a
// primer, not a feature. The location is the binding site; the note has the
// whole primer, 5' tail included. "color: black; sequence: ..." reads too.
bool readPrimer(const RawFeature &rf, const Location &loc, int n, Primer *out)
{
    if (rf.type != "primer_bind")
        return false;
    std::string bases, label;
    for (const Qualifier &q : rf.qualifiers) {
        if (q.key == "label" && label.empty())
            label = trim(q.value);
        if (q.key != "note" || !bases.empty())
            continue;
        size_t at = q.value.find("sequence:");
        if (at == std::string::npos || (at > 0 && (at < 2 || q.value.compare(at - 2, 2, "; ") != 0)))
            continue;
        std::string v = q.value.substr(at + 9);
        v = v.substr(0, v.find(';'));
        // Only bases, and the spaces of wrapped lines.
        const size_t letters = size_t(std::count_if(v.begin(), v.end(), [](char c) {
            return !std::isspace(static_cast<unsigned char>(c));
        }));
        bases = cleanPrimerBases(v);
        if (bases.size() != letters)
            bases.clear();
    }
    if (bases.empty())
        return false;
    // One binding site. A Range can't wrap, so a site across the origin keeps
    // the part before it, as the .dna reader does.
    const bool wraps = loc.ranges.size() == 2 && loc.ranges[0].end == n && loc.ranges[1].start == 0;
    if (loc.ranges.size() != 1 && !wraps)
        return false;
    out->name = label.empty() ? "primer" : label;
    out->bases = bases;
    out->binding = loc.ranges[0];
    out->strand = loc.strand == Strand::Reverse ? Strand::Reverse : Strand::Forward;
    return true;
}

std::string firstWord(const std::string &line)
{
    size_t sp = line.find_first_of(" \t");
    return sp == std::string::npos ? line : line.substr(0, sp);
}

} // namespace

bool readGenBank(const std::string &data, Sequence *out, std::string *error)
{
    enum class Mode { Header, Definition, Comment, Features, Origin, Done };
    Mode mode = Mode::Header;
    Sequence seq;
    std::string definition;
    std::vector<RawFeature> raw;
    std::string bases;
    bool sawLocus = false;
    std::string openRaw;     // raw value of a qualifier still being read
    bool inQualifier = false;

    auto finishQualifier = [&]() {
        if (!inQualifier || raw.empty())
            return;
        raw.back().qualifiers.back().value = unquote(openRaw);
        inQualifier = false;
        openRaw.clear();
    };

    std::istringstream in(data);
    std::string line;
    while (mode != Mode::Done && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (startsWith(line, "//")) {
            finishQualifier();
            if (sawLocus)
                mode = Mode::Done;
            continue;
        }
        const bool keywordLine = !line.empty() && !std::isspace(static_cast<unsigned char>(line[0]));

        if (mode == Mode::Origin && !keywordLine) {
            bases += cleanBasesKeepingCase(line);
            continue;
        }
        if (keywordLine) {
            finishQualifier();
            const std::string keyword = firstWord(line);
            if (keyword == "LOCUS") {
                sawLocus = true;
                std::vector<std::string> tok = splitWhitespace(line);
                if (tok.size() > 1)
                    seq.name = tok[1];
                for (const std::string &t : tok)
                    if (toLower(t) == "circular")
                        seq.circular = true;
                mode = Mode::Header;
            } else if (keyword == "DEFINITION") {
                definition = trim(line.substr(10));
                mode = Mode::Definition;
            } else if (keyword == "COMMENT") {
                mode = Mode::Comment;
                const std::string text = trim(line.substr(7));
                if (text.rfind("History: ", 0) == 0)
                    seq.history.push_back(text.substr(9));
            } else if (keyword == "FEATURES") {
                mode = Mode::Features;
            } else if (keyword == "ORIGIN") {
                mode = Mode::Origin;
            } else {
                mode = Mode::Header;
            }
            continue;
        }
        if (mode == Mode::Definition) {
            definition += " " + trim(line);
            continue;
        }
        if (mode == Mode::Comment) {
            const std::string text = trim(line);
            if (text.rfind("History: ", 0) == 0)
                seq.history.push_back(text.substr(9));
            continue;
        }
        const std::string text = trim(line);
        if (mode != Mode::Features || text.empty())
            continue;

        // Feature keys start at column 6; qualifiers and continuations at 22.
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ')
            ++indent;
        if (indent >= 1 && indent <= 8 && !inQualifier) {
            RawFeature f;
            f.type = firstWord(text);
            f.location = trim(text.substr(f.type.size()));
            raw.push_back(f);
            continue;
        }
        if (raw.empty())
            continue;
        if (inQualifier) {
            if (raw.back().qualifiers.back().key != "translation")
                openRaw += ' ';
            openRaw += text;
            if (isQuoteComplete(openRaw))
                finishQualifier();
            continue;
        }
        if (text[0] == '/') {
            Qualifier q;
            size_t eq = text.find('=');
            q.key = eq == std::string::npos ? text.substr(1) : text.substr(1, eq - 1);
            raw.back().qualifiers.push_back(q);
            if (eq != std::string::npos) {
                openRaw = text.substr(eq + 1);
                inQualifier = true;
                if (isQuoteComplete(openRaw))
                    finishQualifier();
            }
            continue;
        }
        if (raw.back().qualifiers.empty())
            raw.back().location += text;   // location continuation
    }
    finishQualifier();

    if (!sawLocus) {
        if (error)
            *error = "not a GenBank file (no LOCUS line)";
        return false;
    }

    seq.setBasesFromText(bases);   // GenBank is usually all lowercase: that means none marked
    definition = trim(definition);
    if (definition == ".")
        definition.clear();
    seq.description = definition;

    const int n = seq.length();
    for (const RawFeature &rf : raw) {
        Location loc = parseLocation(rf.location, n, seq.circular);
        if (!loc.ok)
            continue;
        Primer p;
        if (readPrimer(rf, loc, n, &p)) {
            seq.primers.push_back(p);
            continue;
        }
        Feature f;
        f.type = rf.type;
        f.strand = loc.strand;
        f.ranges = loc.ranges;
        for (const Qualifier &q : rf.qualifiers) {
            if (q.key == "ApEinfo_fwdcolor") {
                f.color = trim(q.value);
                continue;
            }
            if (startsWith(q.key, "ApEinfo_"))
                continue;
            f.qualifiers.push_back(q);
        }
        f.name = defaultFeatureName(f.type, f.qualifiers);
        seq.features.push_back(f);
    }
    *out = seq;
    return true;
}

// ---- writer ----------------------------------------------------------------

namespace {

const int kQualIndent = 21;
const int kLineWidth = 80;

// Wraps text into lines of at most `width` chars. Normally breaks at a space
// and drops it (readers rejoin with one space). With hardBreaks, splits
// anywhere (for /translation, which readers rejoin with no space). With
// breakAfter, breaks just after that character (commas in locations).
// An unbreakable run is left whole rather than corrupted.
std::vector<std::string> wrapText(const std::string &text, size_t width, bool hardBreaks, char breakAfter = 0)
{
    std::vector<std::string> lines;
    std::string rest = text;
    while (rest.size() > width) {
        size_t cut = std::string::npos;
        bool dropSpace = false;
        if (breakAfter) {
            size_t i = rest.rfind(breakAfter, width - 1);
            if (i != std::string::npos && i > 0)
                cut = i + 1;
        } else if (!hardBreaks) {
            size_t i = rest.rfind(' ', width);
            if (i != std::string::npos && i > 0) {
                cut = i;
                dropSpace = true;
            }
        }
        if (cut == std::string::npos) {
            if (!hardBreaks)
                break;
            cut = width;
        }
        lines.push_back(rest.substr(0, cut));
        rest = rest.substr(cut + (dropSpace ? 1 : 0));
    }
    lines.push_back(rest);
    return lines;
}

bool isBareNumberQualifier(const std::string &key, const std::string &value)
{
    if (key != "codon_start" && key != "transl_table" && key != "number")
        return false;
    return !value.empty() && std::all_of(value.begin(), value.end(),
                                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

std::string locusName(const std::string &name)
{
    std::string s;
    for (char c : name)
        s.push_back(std::isspace(static_cast<unsigned char>(c)) ? '_' : c);
    return s.empty() ? "unnamed" : s;
}

std::string pad(std::string s, size_t width, bool right = false)
{
    if (s.size() >= width)
        return s;
    return right ? std::string(width - s.size(), ' ') + s : s + std::string(width - s.size(), ' ');
}

std::string today()
{
    static const char *months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d-%s-%04d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
    return buf;
}

} // namespace

std::string writeGenBank(const Sequence &seq)
{
    std::string out;
    const int n = seq.length();
    out += "LOCUS       " + pad(locusName(seq.name), 16) + " " + pad(std::to_string(n), 11, true)
         + " bp    DNA     " + (seq.circular ? "circular" : "linear  ") + " SYN " + today() + "\n";

    const std::string def = seq.description.empty() ? "." : seq.description;
    const std::vector<std::string> defLines = wrapText(def, kLineWidth - 12, false);
    for (size_t i = 0; i < defLines.size(); ++i)
        out += (i == 0 ? "DEFINITION  " : std::string(12, ' ')) + defLines[i] + "\n";
    out += "ACCESSION   .\nVERSION     .\nKEYWORDS    .\n";
    out += "SOURCE      synthetic DNA construct\n  ORGANISM  synthetic DNA construct\n";
    for (size_t i = 0; i < seq.history.size(); ++i)
        out += (i == 0 ? "COMMENT     " : std::string(12, ' ')) + "History: " + seq.history[i] + "\n";
    out += "FEATURES             Location/Qualifiers\n";

    // Primers go after the features as primer_bind entries: the location is
    // the binding site, and a note holds the whole primer (see readPrimer).
    std::vector<Feature> features = seq.features;
    for (const Primer &p : seq.primers) {
        if (p.binding.length() <= 0 || p.bases.empty())
            continue;
        Feature f;
        f.type = "primer_bind";
        f.name = p.name;
        f.strand = p.strand == Strand::Reverse ? Strand::Reverse : Strand::Forward;
        f.ranges = {p.binding};
        if (!p.name.empty())
            f.qualifiers.push_back({"label", p.name});
        f.qualifiers.push_back({"note", kPrimerNote + p.bases});
        features.push_back(f);
    }

    const std::string indent(kQualIndent, ' ');
    for (const Feature &f : features) {
        // A feature with no type would be written as bare spaces and lost on
        // the way back in.
        const std::string head = "     " + pad(f.type.empty() ? "misc_feature" : f.type, 15) + " ";
        const std::string loc = formatLocation(f.ranges, f.strand, n, seq.circular);
        const std::vector<std::string> locLines = wrapText(loc, kLineWidth - kQualIndent, false, ',');
        for (size_t i = 0; i < locLines.size(); ++i)
            out += (i == 0 ? head : indent) + locLines[i] + "\n";

        std::vector<Qualifier> quals = f.qualifiers;
        if (!f.name.empty() && defaultFeatureName(f.type, quals) != f.name) {
            quals.erase(std::remove_if(quals.begin(), quals.end(),
                                       [](const Qualifier &q) { return q.key == "label"; }),
                        quals.end());
            quals.insert(quals.begin(), {"label", f.name});
        }
        if (!f.color.empty()) {
            quals.push_back({"ApEinfo_fwdcolor", f.color});
            quals.push_back({"ApEinfo_revcolor", f.color});
        }
        for (const Qualifier &q : quals) {
            std::string text;
            if (q.value.empty()) {
                text = "/" + q.key;
            } else if (isBareNumberQualifier(q.key, q.value)) {
                text = "/" + q.key + "=" + q.value;
            } else {
                std::string v;
                for (char c : q.value) {
                    if (c == '\n' || c == '\r')
                        c = ' ';
                    v.push_back(c);
                    if (c == '"')
                        v.push_back('"');
                }
                text = "/" + q.key + "=\"" + v + "\"";
            }
            // Bases may break anywhere: the reader drops the joining space.
            const bool hard = q.key == "translation" || (q.key == "note" && startsWith(q.value, kPrimerNote));
            for (const std::string &l : wrapText(text, kLineWidth - kQualIndent, hard))
                out += indent + l + "\n";
        }
    }

    out += "ORIGIN\n";
    // Lowercase by convention; with marked bases, their case as shown.
    const std::string lower = seq.lower.empty() ? toLower(seq.bases) : seq.shownBases();
    for (int i = 0; i < n; i += 60) {
        std::string l = pad(std::to_string(i + 1), 9, true);
        for (int j = i; j < std::min(i + 60, n); j += 10)
            l += " " + lower.substr(size_t(j), 10);
        out += l + "\n";
    }
    out += "//\n";
    return out;
}

} // namespace oc
