// SPDX-License-Identifier: GPL-3.0-or-later
#include "fasta.h"

#include <sstream>

namespace oc {

bool readFasta(const std::string &data, Sequence *out, std::string *error)
{
    Sequence seq;
    std::istringstream in(data);
    std::string line;
    bool sawHeader = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line[0] == '>') {
            if (sawHeader || !seq.bases.empty())
                break;   // first record only
            sawHeader = true;
            std::string header = trim(line.substr(1));
            size_t sp = header.find_first_of(" \t");
            seq.name = header.substr(0, sp);
            if (sp != std::string::npos)
                seq.description = trim(header.substr(sp));
            continue;
        }
        if (!line.empty() && line[0] == ';')
            continue;   // old-style comment
        seq.bases += cleanBasesKeepingCase(line);
    }
    seq.setBasesFromText(seq.bases);
    if (seq.bases.empty() && !sawHeader) {
        if (error)
            *error = "no sequence found";
        return false;
    }
    *out = seq;
    return true;
}

std::string writeFasta(const Sequence &seq)
{
    std::string out = ">" + (seq.name.empty() ? std::string("sequence") : seq.name);
    for (char &c : out)
        if (c == ' ' || c == '\t')
            c = '_';
    if (!seq.description.empty())
        out += " " + seq.description;
    out += "\n";
    const std::string shown = seq.shownBases();
    for (size_t i = 0; i < shown.size(); i += 70)
        out += shown.substr(i, 70) + "\n";
    return out;
}

} // namespace oc
