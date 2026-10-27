// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapgene.h"
#include "edit.h"
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
        sortRangesTopStrand(f.ranges, n, seq.circular);
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
    // Our cloning history, one "History: " line per step.
    if (const xml::Element *c = root->child("Comments")) {
        std::string html = c->text;
        size_t pos = 0;
        while (pos <= html.size()) {
            size_t br = html.find("<br>", pos);
            if (br == std::string::npos)
                br = html.size();
            const std::string line = trim(xml::stripHtml(html.substr(pos, br - pos)));
            if (line.rfind("History: ", 0) == 0)
                seq.history.push_back(line.substr(9));
            pos = br + 4;
        }
    }
}

void putU32BE(std::string &out, uint32_t v)
{
    out.push_back(char((v >> 24) & 0xFF));
    out.push_back(char((v >> 16) & 0xFF));
    out.push_back(char((v >> 8) & 0xFF));
    out.push_back(char(v & 0xFF));
}

void putPacket(std::string &out, uint8_t type, const std::string &payload)
{
    out.push_back(char(type));
    putU32BE(out, uint32_t(payload.size()));
    out += payload;
}

std::string escapeXml(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

bool isInteger(const std::string &s)
{
    if (s.empty() || s.size() > 9)
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string featuresXml(const Sequence &seq)
{
    const int n = seq.length();
    std::string x = "<Features nextValidID=\"" + std::to_string(seq.features.size()) + "\">";
    int id = 0;
    for (const Feature &f : seq.features) {
        x += "<Feature recentID=\"" + std::to_string(id++) + "\" name=\"" + escapeXml(f.name) + "\"";
        if (f.strand != Strand::None)
            x += std::string(" directionality=\"") + (f.strand == Strand::Forward ? "1" : "2") + "\"";
        x += " type=\"" + escapeXml(f.type) + "\">";
        // Pieces that meet at the origin become one wrapping segment ("a-b", a > b).
        for (size_t i = 0; i < f.ranges.size(); ++i) {
            const Range &r = f.ranges[i];
            int a = r.start + 1, b = r.end;
            if (seq.circular && r.end == n && i + 1 < f.ranges.size() && f.ranges[i + 1].start == 0)
                b = f.ranges[++i].end;
            x += "<Segment range=\"" + std::to_string(a) + "-" + std::to_string(b) + "\"";
            if (!f.color.empty())
                x += " color=\"" + escapeXml(f.color) + "\"";
            x += " type=\"standard\"/>";
        }
        // Repeated keys go into one <Q> with several values. Text values are
        // HTML inside the XML, so they are escaped twice.
        std::vector<std::string> keys;
        for (const Qualifier &q : f.qualifiers)
            if (std::find(keys.begin(), keys.end(), q.key) == keys.end())
                keys.push_back(q.key);
        for (const std::string &key : keys) {
            x += "<Q name=\"" + escapeXml(key) + "\">";
            for (const Qualifier &q : f.qualifiers)
                if (q.key == key)
                    x += isInteger(q.value) ? "<V int=\"" + q.value + "\"/>" : "<V text=\"" + escapeXml(escapeXml(q.value)) + "\"/>";
            x += "</Q>";
        }
        x += "</Feature>";
    }
    return x + "</Features>";
}

std::string primersXml(const Sequence &seq)
{
    std::string x = "<Primers nextValidID=\"" + std::to_string(seq.primers.size()) + "\">";
    int id = 0;
    for (const Primer &p : seq.primers) {
        x += "<Primer recentID=\"" + std::to_string(id++) + "\" name=\"" + escapeXml(p.name) + "\" sequence=\""
           + escapeXml(p.bases) + "\">";
        if (p.binding.length() > 0)
            x += "<BindingSite location=\"" + std::to_string(p.binding.start) + "-" + std::to_string(p.binding.end - 1)
               + "\" boundStrand=\"" + (p.strand == Strand::Reverse ? "1" : "0") + "\"/>";
        x += "</Primer>";
    }
    return x + "</Primers>";
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

std::string writeSnapGene(const Sequence &seq)
{
    std::string out;
    // Header: magic, file type 1 (DNA), then export and import format versions.
    std::string header = "SnapGene";
    for (uint16_t v : {uint16_t(1), uint16_t(15), uint16_t(19)}) {
        header.push_back(char(v >> 8));
        header.push_back(char(v & 0xFF));
    }
    putPacket(out, kHeader, header);
    putPacket(out, kDna, std::string(1, char(seq.circular ? 0x01 : 0x00)) + seq.bases);
    putPacket(out, kPrimers, primersXml(seq));
    std::string notes = "<Notes><Type>Synthetic</Type>";
    if (!seq.description.empty())
        notes += "<Description>" + escapeXml("<html><body>" + escapeXml(seq.description) + "</body></html>")
               + "</Description>";
    if (!seq.history.empty()) {
        std::string lines;
        for (const std::string &h : seq.history)
            lines += (lines.empty() ? "" : "<br>") + escapeXml("History: " + h);
        notes += "<Comments>" + escapeXml("<html><body>" + lines + "</body></html>") + "</Comments>";
    }
    putPacket(out, kNotes, notes + "</Notes>");
    putPacket(out, kFeatures, featuresXml(seq));
    return out;
}

} // namespace oc
