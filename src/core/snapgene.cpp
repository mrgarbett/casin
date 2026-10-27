// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapgene.h"
#include "xml.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace oc {

namespace {

enum PacketType : unsigned char {
    kDna = 0x00,
    kPrimers = 0x05,
    kNotes = 0x06,
    kHeader = 0x09,
    kFeatures = 0x0A,
};

uint32_t readU32BE(const std::string &d, size_t pos)
{
    return (uint32_t(uint8_t(d[pos])) << 24) | (uint32_t(uint8_t(d[pos + 1])) << 16)
         | (uint32_t(uint8_t(d[pos + 2])) << 8) | uint32_t(uint8_t(d[pos + 3]));
}

uint16_t readU16BE(const std::string &d, size_t pos)
{
    return uint16_t((uint8_t(d[pos]) << 8) | uint8_t(d[pos + 1]));
}

// "a-b", both ints. Returns false when malformed.
bool parsePair(const std::string &s, int *a, int *b)
{
    size_t dash = s.find('-');
    if (dash == std::string::npos || dash == 0)
        return false;
    char *endA = nullptr, *endB = nullptr;
    std::string sa = s.substr(0, dash), sb = s.substr(dash + 1);
    long va = std::strtol(sa.c_str(), &endA, 10);
    long vb = std::strtol(sb.c_str(), &endB, 10);
    if (*endA || *endB || sb.empty())
        return false;
    *a = int(va);
    *b = int(vb);
    return true;
}

// Orders pieces so the feature reads in top-strand order starting just after
// its largest uncovered gap (so origin-crossing pieces come out end-then-start).
void sortTopStrand(std::vector<Range> &ranges, int n, bool circular)
{
    Feature tmp;
    tmp.ranges = ranges;
    const Span s = tmp.span(n, circular);
    std::sort(ranges.begin(), ranges.end(), [&](const Range &x, const Range &y) {
        auto key = [&](const Range &r) { return n > 0 ? ((r.start - s.start) % n + n) % n : r.start; };
        return key(x) < key(y);
    });
}

void parseFeatures(const std::string &xmlText, Sequence &seq)
{
    auto root = xml::parse(xmlText);
    if (!root)
        return;
    const int n = seq.length();
    for (const xml::Element *fe : root->childrenNamed("Feature")) {
        Feature f;
        f.type = fe->attr("type", "misc_feature");
        const std::string dir = fe->attr("directionality");
        f.strand = dir == "1" ? Strand::Forward : dir == "2" ? Strand::Reverse : Strand::None;
        for (const xml::Element *seg : fe->childrenNamed("Segment")) {
            if (seg->attr("type") == "gap")
                continue;
            int a = 0, b = 0;
            if (!parsePair(seg->attr("range"), &a, &b) || a < 1 || b < 1 || a > n || b > n)
                continue;
            if (f.color.empty())
                f.color = seg->attr("color");
            if (a <= b) {
                f.ranges.push_back({a - 1, b});
            } else {   // wraps the origin
                f.ranges.push_back({a - 1, n});
                f.ranges.push_back({0, b});
            }
        }
        if (f.ranges.empty())
            continue;
        sortTopStrand(f.ranges, n, seq.circular);
        for (const xml::Element *q : fe->childrenNamed("Q")) {
            const std::string key = q->attr("name");
            if (key.empty())
                continue;
            for (const xml::Element *v : q->childrenNamed("V")) {
                std::string value;
                if (const std::string *t = v->attribute("text"))
                    value = xml::stripHtml(*t);
                else if (const std::string *i = v->attribute("int"))
                    value = *i;
                else if (const std::string *p = v->attribute("predef"))
                    value = *p;
                else
                    value = xml::stripHtml(v->text);
                f.qualifiers.push_back({key, value});
            }
        }
        const std::string name = trim(fe->attr("name"));
        f.name = name.empty() ? defaultFeatureName(f.type, f.qualifiers) : name;
        seq.features.push_back(f);
    }
}

void parsePrimers(const std::string &xmlText, Sequence &seq)
{
    auto root = xml::parse(xmlText);
    if (!root)
        return;
    const int n = seq.length();
    for (const xml::Element *pe : root->childrenNamed("Primer")) {
        Primer p;
        p.name = pe->attr("name");
        p.bases = cleanBases(pe->attr("sequence"));
        const xml::Element *site = pe->child("BindingSite");
        if (!site)
            continue;
        int a = 0, b = 0;
        // Binding site locations are 0-based and inclusive, unlike Segment ranges.
        if (!parsePair(site->attr("location"), &a, &b) || a < 0 || a >= n || b < 0 || b >= n)
            continue;
        if (a <= b)
            p.binding = {a, b + 1};
        else
            p.binding = {a, n};   // Range can't wrap; keep the part before the origin
        p.strand = site->attr("boundStrand") == "1" ? Strand::Reverse : Strand::Forward;
        seq.primers.push_back(p);
    }
}

void parseNotes(const std::string &xmlText, Sequence &seq)
{
    auto root = xml::parse(xmlText);
    if (!root)
        return;
    if (const xml::Element *d = root->child("Description"))
        seq.description = xml::stripHtml(d->text);
}

} // namespace

bool looksLikeSnapGene(const std::string &data)
{
    return data.size() >= 13 && uint8_t(data[0]) == kHeader && data.compare(5, 8, "SnapGene") == 0;
}

bool readSnapGene(const std::string &data, Sequence *out, std::string *error)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!looksLikeSnapGene(data))
        return fail("not a SnapGene file (missing header packet)");

    Sequence seq;
    bool haveDna = false;
    std::string featuresXml, primersXml, notesXml;
    size_t pos = 0;
    bool first = true;
    while (pos < data.size()) {
        if (data.size() - pos < 5)
            return fail("truncated packet header");
        const uint8_t type = uint8_t(data[pos]);
        const uint32_t len = readU32BE(data, pos + 1);
        pos += 5;
        if (len > data.size() - pos)
            return fail("truncated packet");
        const std::string payload = data.substr(pos, len);
        pos += len;

        if (first) {
            first = false;
            if (type != kHeader || payload.size() < 10 || payload.compare(0, 8, "SnapGene") != 0)
                return fail("not a SnapGene file");
            if (readU16BE(payload, 8) != 1)
                return fail("only DNA .dna files are supported");
            continue;
        }
        switch (type) {
        case kDna:
            if (payload.empty())
                return fail("empty DNA packet");
            seq.circular = (uint8_t(payload[0]) & 0x01) != 0;
            seq.bases = cleanBases(payload.substr(1));
            haveDna = true;
            break;
        case kFeatures: featuresXml = payload; break;
        case kPrimers: primersXml = payload; break;
        case kNotes: notesXml = payload; break;
        default: break;   // unknown packets are ignored
        }
    }
    if (!haveDna)
        return fail("no DNA packet");

    // Features and primers need the sequence length, so parse them last.
    if (!featuresXml.empty())
        parseFeatures(featuresXml, seq);
    if (!primersXml.empty())
        parsePrimers(primersXml, seq);
    if (!notesXml.empty())
        parseNotes(notesXml, seq);
    *out = seq;
    return true;
}

} // namespace oc
