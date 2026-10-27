// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sequence.h"

namespace oc {

// A restriction enzyme. Cut positions are counted from the first base of the
// recognition site, in the site's own orientation: the top strand is cut
// after `cutTop` bases and the bottom strand after `cutBottom` bases (both in
// top-strand coordinates). EcoRI G^AATTC = {1, 5}; PstI CTGCA^G = {5, 1};
// BsaI GGTCTC(1/5) = {7, 11}. Enzymes that cut on both sides of their site
// have a second pair: BaeI (10/15)ACNNNNGTAYC(12/7) = {-10, -15, 23, 18}.
struct Enzyme {
    std::string name;
    std::string site;    // IUPAC, uppercase
    int cutTop = 0;
    int cutBottom = 0;
    int cutTop2 = 0;
    int cutBottom2 = 0;
    bool cutsTwice = false;

    int siteLength() const { return int(site.size()); }
    bool isPalindromic() const;
    // Cuts outside its recognition site (Type IIS).
    bool cutsOutsideSite() const;
    // Overhang length; > 0 = 5' overhang, < 0 = 3' overhang, 0 = blunt.
    int overhang() const { return cutBottom - cutTop; }
};

struct CutSite {
    const Enzyme *enzyme = nullptr;
    int matchStart = 0;   // first base of the recognition site on the top strand
    int cutTop = 0;       // boundary index (0..n) of the top-strand cut
    int cutBottom = 0;    // boundary index of the bottom-strand cut
    Strand strand = Strand::Forward;   // orientation the site was found in
};

// About 60 common Type II enzymes plus Type IIS (BsaI, BsmBI, BbsI, SapI,
// AarI, ...). Data from REBASE (http://rebase.neb.com), see docs/REBASE.md.
const std::vector<Enzyme> &commonEnzymes();
const Enzyme *findEnzyme(const std::string &name);

// All cut sites of the given enzymes (two per match for enzymes that cut twice), sorted by top-strand cut position.
// Searches the reverse strand for non-palindromic sites, finds sites that
// span the origin of circular sequences, and drops cuts outside (0, n) on
// linear ones. The returned pointers refer into `enzymes`.
std::vector<CutSite> findSites(const Sequence &seq, const std::vector<Enzyme> &enzymes);

// Commercial suppliers (REBASE codes) and which of them sell an enzyme.
struct Supplier {
    char code;
    const char *name;
};
const std::vector<Supplier> &suppliers();
std::string enzymeSuppliers(const std::string &name);   // e.g. "BN"; "" if unknown

// A named list of enzymes to show.
struct EnzymeSet {
    std::string name;
    std::vector<std::string> enzymes;
};
// Type IIS, then one set per supplier that sells any of commonEnzymes().
std::vector<EnzymeSet> builtinEnzymeSets();
// Saved sets, one per line: "name: EcoRI, BamHI, ...". Unknown enzymes are dropped.
std::vector<EnzymeSet> parseEnzymeSets(const std::string &text);
std::string formatEnzymeSets(const std::vector<EnzymeSet> &sets);

struct PatternMatch {
    int start;       // top-strand start of the match
    int length;
    Strand strand;   // Reverse = the pattern reads on the bottom strand
};
// IUPAC pattern search on both strands (wraps the origin when circular).
std::vector<PatternMatch> findPattern(const Sequence &seq, const std::string &pattern);

} // namespace oc
