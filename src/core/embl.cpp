// SPDX-License-Identifier: GPL-3.0-or-later
#include "embl.h"
#include "genbank.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace oc {

namespace {

std::string field(const std::string &line)
{
    return line.size() > 5 ? line.substr(5) : std::string();
}

} // namespace

bool looksLikeEmbl(const std::string &data)
{
    size_t i = 0;
    while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i])))
        ++i;
    return data.compare(i, 5, "ID   ") == 0;
}

bool readEmbl(const std::string &data, Sequence *out, std::string *error)
{
    // Rewrite the record as GenBank: ID -> LOCUS, DE -> DEFINITION,
    // CC History -> COMMENT, FT -> FEATURES, SQ -> ORIGIN.
    std::istringstream in(data);
    std::string line, gb;
    bool sawId = false, inFeatures = false, inSequence = false, sawDe = false, sawHistory = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 2, "//") == 0) {
            if (sawId)
                break;
            continue;
        }
        if (inSequence && (line.empty() || line[0] == ' ')) {
            gb += line + "\n";
            continue;
        }
        const std::string code = line.substr(0, 2);
        if (code == "ID") {
            sawId = true;
            // "ID   name; SV 1; circular; genomic DNA; STD; SYN; 2686 BP."
            std::string rest = trim(field(line));
            std::string name = rest.substr(0, rest.find(';'));
            name = trim(name);
            if (name.empty())
                name = "untitled";
            std::replace(name.begin(), name.end(), ' ', '_');
            const bool circular = toLower(rest).find("circular") != std::string::npos;
            gb += "LOCUS       " + name + " 0 bp DNA " + (circular ? "circular" : "linear") + "\n";
        } else if (code == "DE") {
            gb += (sawDe ? std::string(12, ' ') : std::string("DEFINITION  ")) + trim(field(line)) + "\n";
            sawDe = true;
        } else if (code == "CC") {
            const std::string text = trim(field(line));
            if (text.rfind("History: ", 0) == 0) {
                gb += (sawHistory ? std::string(12, ' ') : std::string("COMMENT     ")) + text + "\n";
                sawHistory = true;
            }
        } else if (code == "FT") {
            if (!inFeatures) {
                gb += "FEATURES             Location/Qualifiers\n";
                inFeatures = true;
            }
            gb += "  " + line.substr(2) + "\n";   // same columns: key at 6, qualifiers at 22
        } else if (code == "SQ") {
            gb += "ORIGIN\n";
            inSequence = true;
        }
    }
    if (!sawId) {
        if (error)
            *error = "not an EMBL file (no ID line)";
        return false;
    }
    gb += "//\n";
    if (!readGenBank(gb, out, error))
        return false;
    return true;
}

std::string writeEmbl(const Sequence &seq)
{
    const int n = seq.length();
    std::string name = seq.name.empty() ? "untitled" : seq.name;
    std::replace(name.begin(), name.end(), ' ', '_');
    std::replace(name.begin(), name.end(), ';', '_');
    std::string out = "ID   " + name + "; SV 1; " + (seq.circular ? "circular" : "linear")
                    + "; genomic DNA; STD; SYN; " + std::to_string(n) + " BP.\nXX\n";
    if (!seq.description.empty())
        out += "DE   " + seq.description + "\nXX\n";
    for (const std::string &h : seq.history)
        out += "CC   History: " + h + "\n";
    if (!seq.history.empty())
        out += "XX\n";

    // The feature table is the GenBank one with an FT prefix.
    std::istringstream gb(writeGenBank(seq));
    std::string line;
    bool inFeatures = false, any = false;
    while (std::getline(gb, line)) {
        if (line.compare(0, 8, "FEATURES") == 0) {
            inFeatures = true;
            continue;
        }
        if (line.compare(0, 6, "ORIGIN") == 0)
            break;
        if (inFeatures && line.size() > 2) {
            if (!any)
                out += "FH   Key             Location/Qualifiers\nFH\n";
            any = true;
            out += "FT" + line.substr(2) + "\n";
        }
    }
    if (any)
        out += "XX\n";

    int counts[5] = {0, 0, 0, 0, 0};
    for (char c : seq.bases) {
        const char u = char(std::toupper(static_cast<unsigned char>(c)));
        counts[u == 'A' ? 0 : u == 'C' ? 1 : u == 'G' ? 2 : u == 'T' ? 3 : 4]++;
    }
    out += "SQ   Sequence " + std::to_string(n) + " BP; " + std::to_string(counts[0]) + " A; "
         + std::to_string(counts[1]) + " C; " + std::to_string(counts[2]) + " G; " + std::to_string(counts[3])
         + " T; " + std::to_string(counts[4]) + " other;\n";
    const std::string lower = seq.lower.empty() ? toLower(seq.bases) : seq.shownBases();
    for (int i = 0; i < n; i += 60) {
        std::string l = "    ";
        const int end = std::min(i + 60, n);
        for (int j = i; j < end; j += 10)
            l += " " + lower.substr(size_t(j), size_t(std::min(10, end - j)));
        const std::string num = std::to_string(end);
        l += std::string(std::max<size_t>(1, 80 - l.size() - num.size()), ' ') + num;
        out += l + "\n";
    }
    out += "//\n";
    return out;
}

std::vector<std::string> splitRecords(const std::string &data)
{
    std::vector<std::string> records;
    std::istringstream in(data);
    std::string line, cur;
    while (std::getline(in, line)) {
        cur += line + "\n";
        if (line.compare(0, 2, "//") == 0) {
            if (trim(cur).size() > 2)
                records.push_back(cur);
            cur.clear();
        }
    }
    if (!trim(cur).empty())
        records.push_back(cur);
    return records;
}

} // namespace oc
