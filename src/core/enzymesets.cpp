// SPDX-License-Identifier: GPL-3.0-or-later
#include "enzymes.h"

#include <sstream>

namespace oc {

std::vector<EnzymeSet> builtinEnzymeSets()
{
    std::vector<EnzymeSet> sets;
    EnzymeSet typeIIS{"Type IIS (Golden Gate)", {}};
    for (const Enzyme &e : commonEnzymes())
        if (e.cutsOutsideSite())
            typeIIS.enzymes.push_back(e.name);
    sets.push_back(typeIIS);
    for (const Supplier &s : suppliers()) {
        EnzymeSet set{std::string("Sold by ") + s.name, {}};
        for (const Enzyme &e : commonEnzymes())
            if (enzymeSuppliers(e.name).find(s.code) != std::string::npos)
                set.enzymes.push_back(e.name);
        if (!set.enzymes.empty())
            sets.push_back(set);
    }
    return sets;
}

std::vector<EnzymeSet> parseEnzymeSets(const std::string &text)
{
    std::vector<EnzymeSet> sets;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t colon = line.rfind(':');
        if (colon == std::string::npos || trim(line).empty() || trim(line)[0] == '#')
            continue;
        EnzymeSet set{trim(line.substr(0, colon)), {}};
        std::istringstream names(line.substr(colon + 1));
        std::string name;
        while (std::getline(names, name, ','))
            if (const Enzyme *e = findEnzyme(trim(name)))
                set.enzymes.push_back(e->name);
        if (!set.name.empty())
            sets.push_back(set);
    }
    return sets;
}

std::string formatEnzymeSets(const std::vector<EnzymeSet> &sets)
{
    std::string out;
    for (const EnzymeSet &s : sets) {
        out += s.name + ":";
        for (size_t i = 0; i < s.enzymes.size(); ++i)
            out += (i ? ", " : " ") + s.enzymes[i];
        out += "\n";
    }
    return out;
}

} // namespace oc
