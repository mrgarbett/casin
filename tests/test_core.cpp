// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for openclone_core. Usage: core_tests <samples-dir>
#include "core/digest.h"
#include "core/edit.h"
#include "core/enzymes.h"
#include "core/fasta.h"
#include "core/genbank.h"
#include "core/seqio.h"
#include "core/sequence.h"
#include "core/snapgene.h"
#include "core/tm.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace oc;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        ++g_checks;                                                                 \
        auto va_ = (a);                                                             \
        auto vb_ = (b);                                                             \
        if (!(va_ == vb_)) {                                                        \
            ++g_failures;                                                           \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                           \
    } while (0)

static bool sameRanges(const std::vector<Range> &r, std::initializer_list<Range> expect)
{
    return r == std::vector<Range>(expect);
}

static void testBasics()
{
    CHECK_EQ(reverseComplement("ATGCRYKMBDHVNSW"), std::string("WSNBDHVKMRYGCAT"));
    CHECK_EQ(reverseComplement("acgT"), std::string("Acgt"));
    CHECK_EQ(translate("ATGGCCTAAGG"), std::string("MA*"));
    CHECK_EQ(translateCodon("GCN"), 'A');      // fourfold degenerate
    CHECK_EQ(translateCodon("AAY"), 'N');
    CHECK_EQ(translateCodon("AAN"), 'X');      // N or K
    CHECK_EQ(translateCodon("TAR"), '*');
    CHECK_EQ(translateCodon("ATG"), 'M');
    CHECK_EQ(cleanBases("acg u\n12 N-x"), std::string("ACGTN"));
    CHECK(std::fabs(gcFraction("GGCCAATT") - 0.5) < 1e-9);
    CHECK_EQ(defaultFeatureName("CDS", {{"note", "n"}, {"gene", "bla"}}), std::string("bla"));
    CHECK_EQ(defaultFeatureName("CDS", {{"product", "p"}, {"label", "L"}}), std::string("L"));
    CHECK_EQ(defaultFeatureName("rep_origin", {}), std::string("rep_origin"));

    Sequence s;
    s.bases = "AACCGGTTAC";
    s.circular = true;
    CHECK_EQ(s.slice(8, 4), std::string("ACAA"));
    s.circular = false;
    CHECK_EQ(s.slice(8, 4), std::string("AC"));

    // Feature positions on the reverse strand run 5'->3' of that strand.
    s.circular = true;
    Feature f;
    f.strand = Strand::Reverse;
    f.ranges = {{8, 10}, {0, 2}};
    CHECK(s.featurePositions(f) == std::vector<int>({1, 0, 9, 8}));
    CHECK_EQ(s.featureBases(f), std::string("TTGT"));
    const Span sp = f.span(10, true);
    CHECK_EQ(sp.start, 8);
    CHECK_EQ(sp.length, 4);
    CHECK(sp.contains(1, 10));
    CHECK(!sp.contains(5, 10));

    // codon_start honored; marks sit on the middle base.
    Sequence c;
    c.bases = "GATGGCCTAA";
    Feature cds;
    cds.type = "CDS";
    cds.strand = Strand::Forward;
    cds.ranges = {{0, 10}};
    cds.qualifiers = {{"codon_start", "2"}};
    auto marks = codonMarks(c, cds);
    CHECK_EQ(marks.size(), size_t(3));
    CHECK_EQ(marks[0].position, 2);
    CHECK_EQ(marks[0].aminoAcid, 'M');
    CHECK_EQ(marks[2].aminoAcid, '*');
}

static void testLocations()
{
    Location l = parseLocation("complement(join(10..20,30..40))", 100, false);
    CHECK(l.ok);
    CHECK(l.strand == Strand::Reverse);
    CHECK(sameRanges(l.ranges, {{9, 20}, {29, 40}}));

    // Inner complements are stored in top-strand order.
    l = parseLocation("join(complement(30..40),complement(10..20))", 100, false);
    CHECK(l.ok);
    CHECK(l.strand == Strand::Reverse);
    CHECK(sameRanges(l.ranges, {{9, 20}, {29, 40}}));

    // a > b wraps on circular sequences only.
    l = parseLocation("90..10", 100, true);
    CHECK(l.ok);
    CHECK(sameRanges(l.ranges, {{89, 100}, {0, 10}}));
    CHECK(!parseLocation("90..10", 100, false).ok);
    l = parseLocation("complement(90..10)", 100, true);
    CHECK(l.ok && l.strand == Strand::Reverse);
    CHECK(sameRanges(l.ranges, {{89, 100}, {0, 10}}));

    l = parseLocation("<1..>50", 100, false);
    CHECK(l.ok && sameRanges(l.ranges, {{0, 50}}));
    l = parseLocation("5^6", 100, false);
    CHECK(l.ok && sameRanges(l.ranges, {{5, 5}}));
    l = parseLocation("order(1..3, 7)", 100, false);
    CHECK(l.ok && sameRanges(l.ranges, {{0, 3}, {6, 7}}));
    l = parseLocation("join(J00194.1:100..202,1..5)", 100, false);
    CHECK(l.ok && sameRanges(l.ranges, {{0, 5}}));
    CHECK(!parseLocation("join(1..5", 100, false).ok);
    CHECK(!parseLocation("1..500", 100, false).ok);

    CHECK_EQ(formatLocation({{9, 20}, {29, 40}}, Strand::Reverse, 100, false),
             std::string("complement(join(10..20,30..40))"));
    CHECK_EQ(formatLocation({{4, 5}}, Strand::Forward, 100, false), std::string("5"));
    CHECK_EQ(formatLocation({{5, 5}}, Strand::Forward, 100, false), std::string("5^6"));
    // Formatting then parsing gives the same ranges back.
    l = parseLocation(formatLocation({{89, 100}, {0, 10}}, Strand::Forward, 100, true), 100, true);
    CHECK(sameRanges(l.ranges, {{89, 100}, {0, 10}}));
}

static void testGenBankRoundTrip()
{
    Sequence s;
    s.name = "pTest";
    s.description = "A test plasmid with a description long enough that it has to wrap onto a second "
                    "DEFINITION line in the output.";
    s.circular = true;
    for (int i = 0; i < 25; ++i)
        s.bases += "ATGAAACCCGGGTTTAAAGGATCCTAG";
    const int n = s.length();

    Feature a;
    a.type = "CDS";
    a.strand = Strand::Forward;
    a.ranges = {{0, 27}};
    a.qualifiers = {{"gene", "geneA"},
                    {"note", "a \"quoted\" note that is quite long and will need to wrap across several "
                             "lines of the feature table"},
                    {"codon_start", "1"},
                    {"translation", std::string(120, 'M')}};
    a.name = "geneA";
    Feature b;
    b.type = "misc_feature";
    b.strand = Strand::Reverse;
    b.ranges = {{n - 10, n}, {0, 5}};
    b.name = "custom name";   // not derivable from qualifiers -> /label
    b.color = "#ff8800";
    Feature c;
    c.type = "primer_bind";
    c.strand = Strand::Forward;
    c.ranges = {{30, 40}, {50, 60}};
    c.name = "primer_bind";
    s.features = {a, b, c};

    const std::string text = writeGenBank(s);
    // Every line fits in 80 columns.
    size_t lineStart = 0, longest = 0;
    for (size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '\n') {
            longest = std::max(longest, i - lineStart);
            lineStart = i + 1;
        }
    CHECK(longest <= 80);

    Sequence r;
    std::string err;
    CHECK(readGenBank(text, &r, &err));
    CHECK_EQ(r.name, s.name);
    CHECK_EQ(r.description, s.description);
    CHECK_EQ(r.circular, true);
    CHECK_EQ(r.bases, s.bases);
    CHECK_EQ(r.features.size(), size_t(3));
    if (r.features.size() == 3) {
        for (size_t i = 0; i < 3; ++i) {
            CHECK_EQ(r.features[i].type, s.features[i].type);
            CHECK_EQ(r.features[i].name, s.features[i].name);
            CHECK(r.features[i].strand == s.features[i].strand);
            CHECK(r.features[i].ranges == s.features[i].ranges);
        }
        CHECK(r.features[0].qualifiers == s.features[0].qualifiers);
        CHECK_EQ(r.features[1].color, std::string("#ff8800"));
    }
    // Linear too.
    s.circular = false;
    s.features.pop_back();
    s.features.pop_back();
    CHECK(readGenBank(writeGenBank(s), &r));
    CHECK(!r.circular);
}

// ---- .dna built byte by byte ------------------------------------------------

static void putPacket(std::string &out, uint8_t type, const std::string &payload)
{
    out.push_back(char(type));
    const uint32_t len = uint32_t(payload.size());
    out.push_back(char((len >> 24) & 0xff));
    out.push_back(char((len >> 16) & 0xff));
    out.push_back(char((len >> 8) & 0xff));
    out.push_back(char(len & 0xff));
    out += payload;
}

static void testSnapGene()
{
    const std::string bases = "GAATTCAAACCCGGGTTTATGAAATAGGGATCCAAAAAAAAAACTGCAG";   // 49 bp
    std::string header = "SnapGene";
    header += std::string("\x00\x01\x00\x0f\x00\x13", 6);   // DNA, versions
    const std::string features =
        "<?xml version=\"1.0\"?><Features nextValidID=\"3\">"
        "<Feature name=\"orfX\" type=\"CDS\" directionality=\"1\">"
        "<Segment range=\"19-27\" color=\"#aabbcc\" type=\"standard\"/>"
        "<Q name=\"note\"><V text=\"&lt;html&gt;&lt;body&gt;hello &amp;amp; &lt;b&gt;bye&lt;/b&gt;&lt;/body&gt;&lt;/html&gt;\"/></Q>"
        "<Q name=\"codon_start\"><V int=\"1\"/></Q>"
        "</Feature>"
        "<Feature name=\"wrap\" type=\"misc_feature\" directionality=\"2\">"
        "<Segment range=\"45-3\" type=\"standard\"/></Feature>"
        "<Feature name=\"split\" type=\"CDS\" directionality=\"1\">"
        "<Segment range=\"1-3\"/><Segment range=\"4-9\" type=\"gap\"/><Segment range=\"10-12\"/>"
        "</Feature>"
        "</Features>";
    const std::string primers =
        "<Primers><Primer name=\"p1\" sequence=\"ctgcag\">"
        "<BindingSite location=\"43-48\" boundStrand=\"1\"/></Primer></Primers>";
    const std::string notes = "<Notes><Description>&lt;html&gt;&lt;body&gt;My plasmid&lt;/body&gt;&lt;/html&gt;</Description></Notes>";

    std::string file;
    putPacket(file, 0x09, header);
    // Features before the DNA: the reader must wait for the sequence length.
    putPacket(file, 0x0A, features);
    putPacket(file, 0x00, std::string(1, '\x01') + bases);
    putPacket(file, 0x05, primers);
    putPacket(file, 0x63, "unknown packet");
    putPacket(file, 0x06, notes);

    Sequence s;
    std::string err;
    CHECK(looksLikeSnapGene(file));
    CHECK(readSnapGene(file, &s, &err));
    CHECK_EQ(s.bases, bases);
    CHECK(s.circular);
    CHECK_EQ(s.description, std::string("My plasmid"));
    CHECK_EQ(s.features.size(), size_t(3));
    if (s.features.size() == 3) {
        const Feature &f = s.features[0];
        CHECK_EQ(f.name, std::string("orfX"));
        CHECK(f.strand == Strand::Forward);
        CHECK(sameRanges(f.ranges, {{18, 27}}));
        CHECK_EQ(f.color, std::string("#aabbcc"));
        CHECK_EQ(f.qualifier("note"), std::string("hello & bye"));
        CHECK_EQ(f.qualifier("codon_start"), std::string("1"));
        CHECK_EQ(translate(s.featureBases(f)), std::string("MK*"));
        const Feature &w = s.features[1];
        CHECK(w.strand == Strand::Reverse);
        CHECK(sameRanges(w.ranges, {{44, 49}, {0, 3}}));
        CHECK(sameRanges(s.features[2].ranges, {{0, 3}, {9, 12}}));
    }
    CHECK_EQ(s.primers.size(), size_t(1));
    if (!s.primers.empty()) {
        CHECK(s.primers[0].binding == (Range{43, 49}));
        CHECK(s.primers[0].strand == Strand::Reverse);
        CHECK_EQ(s.primers[0].bases, std::string("CTGCAG"));
    }

    // Header must come first; truncated files fail cleanly.
    std::string bad;
    putPacket(bad, 0x00, std::string(1, '\x00') + bases);
    putPacket(bad, 0x09, header);
    CHECK(!readSnapGene(bad, &s, &err));
    CHECK(!readSnapGene(file.substr(0, file.size() - 5), &s, &err));
}

// ---- enzymes ------------------------------------------------------------------

static const CutSite *siteOf(const std::vector<CutSite> &sites, const std::string &name, int nth = 0)
{
    for (const CutSite &c : sites)
        if (c.enzyme->name == name && nth-- == 0)
            return &c;
    return nullptr;
}

static void testEnzymes()
{
    {
        // BaeI cuts on both sides of its site: (10/15)ACNNNNGTAYC(12/7).
        Sequence two;
        two.bases = std::string(30, 'G') + "ACAAAAGTATC" + std::string(30, 'G');
        const Enzyme *bae = findEnzyme("BaeI");
        CHECK(bae && bae->cutsTwice);
        std::vector<Enzyme> only{*bae};
        const std::vector<CutSite> cuts = findSites(two, only);
        CHECK_EQ(cuts.size(), size_t(2));
        if (cuts.size() == 2) {
            CHECK(cuts[0].cutTop == 20 && cuts[0].cutBottom == 15);
            CHECK(cuts[1].cutTop == 53 && cuts[1].cutBottom == 48);
        }
    }
    const auto &all = commonEnzymes();
    CHECK(all.size() >= 60);
    const Enzyme *ecoRI = findEnzyme("EcoRI");
    CHECK(ecoRI && ecoRI->cutTop == 1 && ecoRI->cutBottom == 5);
    const Enzyme *pstI = findEnzyme("PstI");
    CHECK(pstI && pstI->cutTop == 5 && pstI->cutBottom == 1);
    const Enzyme *bsaI = findEnzyme("BsaI");
    CHECK(bsaI && bsaI->cutTop == 7 && bsaI->cutBottom == 11 && bsaI->cutsOutsideSite());
    CHECK(ecoRI->isPalindromic() && !bsaI->isPalindromic());

    // IUPAC matching: AvaI CYCGRG matches CTCGAG and CCCGGG.
    Sequence s;
    s.bases = "AAAACTCGAGAAAAAACCCGGGAAAAAACTCGTGAAAA";   // CTCGTG does not match
    std::vector<Enzyme> ava = {*findEnzyme("AvaI")};
    auto sites = findSites(s, ava);
    CHECK_EQ(sites.size(), size_t(2));
    if (sites.size() == 2) {
        CHECK_EQ(sites[0].matchStart, 4);
        CHECK_EQ(sites[0].cutTop, 5);
        CHECK_EQ(sites[1].matchStart, 16);
    }
    // N in the sequence never matches a specific base.
    s.bases = "AAAAGAANTCAAAA";
    CHECK(findSites(s, {*ecoRI}).empty());

    // Type IIS on the reverse strand: GAGACC = revcomp(GGTCTC) at p.
    s.bases = std::string(20, 'A') + "GAGACC" + std::string(20, 'T');
    sites = findSites(s, {*bsaI});
    CHECK_EQ(sites.size(), size_t(1));
    if (!sites.empty()) {
        const int p = 20, L = 6;
        CHECK(sites[0].strand == Strand::Reverse);
        CHECK_EQ(sites[0].matchStart, p);
        CHECK_EQ(sites[0].cutTop, p + L - 11);
        CHECK_EQ(sites[0].cutBottom, p + L - 7);
    }
    // Forward BsaI near the end of a linear sequence cuts past it: dropped.
    s.bases = std::string(20, 'A') + "GGTCTC" + "AAAA";
    CHECK(findSites(s, {*bsaI}).empty());
    s.circular = true;
    sites = findSites(s, {*bsaI});
    CHECK_EQ(sites.size(), size_t(1));
    if (!sites.empty()) {
        CHECK_EQ(sites[0].cutTop, (20 + 7) % 30);
        CHECK_EQ(sites[0].cutBottom, (20 + 11) % 30);
    }

    // A site spanning the origin of a circular sequence.
    s.bases = "TTCAAAAAAAAAAAAAAAAAAAAGAA";   // GAA|TTC across the origin
    s.circular = true;
    sites = findSites(s, {*ecoRI});
    CHECK_EQ(sites.size(), size_t(1));
    if (!sites.empty()) {
        CHECK_EQ(sites[0].matchStart, s.length() - 3);
        CHECK_EQ(sites[0].cutTop, s.length() - 2);
        CHECK_EQ(sites[0].cutBottom, 2);
    }
    s.circular = false;
    CHECK(findSites(s, {*ecoRI}).empty());

    // Sorted by cut position across enzymes.
    s.bases = "AAAACTGCAGAAAAGAATTCAAAAGGATCCAAAA";
    sites = findSites(s, all);
    for (size_t i = 1; i < sites.size(); ++i)
        CHECK(sites[i - 1].cutTop <= sites[i].cutTop);
    CHECK(siteOf(sites, "PstI") && siteOf(sites, "PstI")->cutTop == 9);
    CHECK(siteOf(sites, "EcoRI") && siteOf(sites, "EcoRI")->cutTop == 15);
    CHECK(siteOf(sites, "BamHI") && siteOf(sites, "BamHI")->cutTop == 25);

    // Pattern find on both strands.
    s.bases = "AAAAGGTCTCAAAAAGAGACCAAA";
    auto m = findPattern(s, "ggtctc");
    CHECK_EQ(m.size(), size_t(2));
    if (m.size() == 2) {
        CHECK(m[0].strand == Strand::Forward && m[0].start == 4);
        CHECK(m[1].strand == Strand::Reverse && m[1].start == 15);
    }
}

static void testTm()
{
    const double tm = meltingTemperature("AGCGGATAACAATTTCACACAGGA");
    CHECK(tm > 55 && tm < 70);
    const double shortTm = meltingTemperature("ACGTACGT");
    CHECK(shortTm < tm);
    CHECK(std::isnan(meltingTemperature("ACGNACGT")));
    CHECK(std::isnan(meltingTemperature("A")));
    // More salt stabilizes the duplex.
    TmConditions hi;
    hi.sodiumMolar = 1.0;
    CHECK(meltingTemperature("AGCGGATAACAATTTCACACAGGA", hi) > tm);
    // Self-complementary path runs.
    const double sc = meltingTemperature("GAATTCGAATTC");
    CHECK(sc > 0 && sc < 60);
}

static void testFastaAndIO()
{
    Sequence s;
    CHECK(readFasta(">seq1 some description\nACGT\nacgu\n>seq2\nGGGG\n", &s));
    CHECK_EQ(s.name, std::string("seq1"));
    CHECK_EQ(s.description, std::string("some description"));
    CHECK_EQ(s.bases, std::string("ACGTACGT"));
    CHECK(readFasta("acgtn\nACGT\n", &s));
    CHECK_EQ(s.bases, std::string("ACGTNACGT"));

    s.name = "x";
    s.description.clear();
    s.bases = std::string(150, 'A');
    const std::string f = writeFasta(s);
    CHECK_EQ(f, ">x\n" + std::string(70, 'A') + "\n" + std::string(70, 'A') + "\n" + std::string(10, 'A') + "\n");

    CHECK(sniffFormat("LOCUS  x") == FileFormat::GenBank);
    CHECK(sniffFormat("\n>x\nAC") == FileFormat::Fasta);
    CHECK(sniffFormat("ACGT ACGT\n") == FileFormat::Fasta);
    CHECK(sniffFormat("hello world, not DNA") == FileFormat::Unknown);
    CHECK(formatFromExtension("a/b/c.GBK") == FileFormat::GenBank);
    CHECK(detectFormat("x.txt", "LOCUS x") == FileFormat::GenBank);
    CHECK_EQ(fileBaseName("/tmp/dir.v2/my file.fasta"), std::string("my file"));
}

static void testSamples(const std::string &dir)
{
    Sequence gb, dna;
    std::string err;
    CHECK(loadSequenceFile(dir + "/pOC-demo.gb", &gb, &err));
    if (!err.empty())
        std::fprintf(stderr, "  %s\n", err.c_str());
    CHECK(loadSequenceFile(dir + "/pOC-demo.dna", &dna, &err));
    CHECK_EQ(dna.name, std::string("pOC-demo"));   // .dna has no name: base name
    CHECK_EQ(gb.bases, dna.bases);
    CHECK(gb.circular && dna.circular);
    CHECK_EQ(gb.features.size(), dna.features.size() + 1);   // + source in GenBank
    CHECK_EQ(dna.primers.size(), size_t(2));
    CHECK(!dna.description.empty() && dna.description.find("SYNTHETIC") != std::string::npos);

    // Every CDS translates to its /translation, including the reverse and split ones.
    int cds = 0;
    for (const Feature &f : gb.features) {
        if (f.type != "CDS")
            continue;
        ++cds;
        std::string t = translate(gb.featureBases(f));
        if (!t.empty() && t.back() == '*')
            t.pop_back();
        CHECK_EQ(t, f.qualifier("translation"));
        CHECK_EQ(codonMarks(gb, f).size(), size_t(f.totalLength() / 3));
    }
    CHECK_EQ(cds, 3);
    // The origin-spanning feature, and the NotI site split across the origin.
    bool sawWrap = false;
    for (const Feature &f : dna.features)
        if (f.name == "origin tag") {
            sawWrap = true;
            const Span sp = f.span(dna.length(), true);
            CHECK_EQ(sp.start, dna.length() - 30);
            CHECK_EQ(sp.length, 70);
            CHECK(sp.wraps(dna.length()));
        }
    CHECK(sawWrap);
    auto sites = findSites(gb, {*findEnzyme("NotI")});
    CHECK_EQ(sites.size(), size_t(1));
    if (!sites.empty())
        CHECK_EQ(sites[0].matchStart, gb.length() - 4);
    // Round trip the sample through our writer.
    Sequence again;
    CHECK(readGenBank(writeGenBank(gb), &again));
    CHECK_EQ(again.bases, gb.bases);
    CHECK_EQ(again.features.size(), gb.features.size());
    for (size_t i = 0; i < std::min(again.features.size(), gb.features.size()); ++i) {
        CHECK(again.features[i].ranges == gb.features[i].ranges);
        CHECK_EQ(again.features[i].name, gb.features[i].name);
    }
}

static Feature feat(const char *name, std::initializer_list<Range> ranges, Strand strand = Strand::Forward)
{
    Feature f;
    f.type = "misc_feature";
    f.name = name;
    f.strand = strand;
    f.ranges = ranges;
    return f;
}

static void testEditing()
{
    // 0123456789
    // AAAACCCCGG, feature A = [2,6), feature B = [6,8), primer on [4,8)
    Sequence s;
    s.bases = "AAAACCCCGG";
    s.features = {feat("A", {{2, 6}}), feat("B", {{6, 8}})};
    s.primers = {{"p", "ACCC", {3, 7}, Strand::Forward}};

    Sequence t = s;
    insertBases(t, 4, "TT");                         // inside A, inside the primer
    CHECK_EQ(t.bases, std::string("AAAATTCCCCGG"));
    CHECK(sameRanges(t.features[0].ranges, {{2, 8}}));
    CHECK(sameRanges(t.features[1].ranges, {{8, 10}}));
    CHECK(t.primers.empty());

    t = s;
    insertBases(t, 2, "T");                          // at A's start: A shifts
    CHECK(sameRanges(t.features[0].ranges, {{3, 7}}));
    CHECK_EQ(t.primers.size(), size_t(1));
    CHECK(t.primers[0].binding == Range({4, 8}));

    t = s;
    deleteBases(t, {5, 2});                          // [5,7): end of A, start of B
    CHECK_EQ(t.bases, std::string("AAAACCGG"));
    CHECK(sameRanges(t.features[0].ranges, {{2, 5}}));
    CHECK(sameRanges(t.features[1].ranges, {{5, 6}}));
    CHECK(t.primers.empty());

    t = s;
    deleteBases(t, {6, 2});                          // all of B
    CHECK_EQ(t.features.size(), size_t(1));

    // A split feature whose gap is deleted becomes one piece.
    t = s;
    t.features = {feat("J", {{0, 2}, {4, 6}})};
    deleteBases(t, {2, 2});
    CHECK(sameRanges(t.features[0].ranges, {{0, 4}}));

    // Circular: a feature across the origin, stored as two ranges.
    Sequence c;
    c.circular = true;
    c.bases = "ACGTACGTAC";                          // n = 10
    c.features = {feat("O", {{8, 10}, {0, 2}})};
    t = c;
    insertBases(t, 0, "GG");                         // at the origin: grows
    CHECK(sameRanges(t.features[0].ranges, {{10, 12}, {0, 4}}));
    t = c;
    deleteBases(t, {9, 2});                          // wraps: deletes base 9 and base 0
    CHECK_EQ(t.bases, std::string("CGTACGTA"));
    CHECK(sameRanges(t.features[0].ranges, {{7, 8}, {0, 1}}));
    t = c;
    CHECK_EQ(replaceBases(t, {9, 2}, "TTT"), 8);
    CHECK_EQ(t.bases, std::string("CGTACGTATTT"));

    t = c;
    rotateOrigin(t, 8);                              // the feature no longer wraps
    CHECK_EQ(t.bases, std::string("ACACGTACGT"));
    CHECK(sameRanges(t.features[0].ranges, {{0, 4}}));
    t.features = {feat("P", {{1, 3}})};            // a plain feature that will wrap
    rotateOrigin(t, 2);
    CHECK(sameRanges(t.features[0].ranges, {{9, 10}, {0, 1}}));

    t = s;
    reverseComplementSequence(t);
    CHECK_EQ(t.bases, std::string("CCGGGGTTTT"));
    CHECK(sameRanges(t.features[0].ranges, {{4, 8}}));
    CHECK(t.features[0].strand == Strand::Reverse);
    CHECK(t.primers[0].binding == Range({3, 7}));
    CHECK(t.primers[0].strand == Strand::Reverse);
    t = c;
    reverseComplementSequence(t);
    CHECK(sameRanges(t.features[0].ranges, {{8, 10}, {0, 2}}));

    // Fragments carry the features and primers inside them.
    Sequence frag = extractFragment(s, {3, 5});        // "ACCCC", A is [2,6), B is [6,8)
    CHECK_EQ(frag.bases, std::string("ACCCC"));
    CHECK_EQ(frag.features.size(), size_t(2));
    CHECK(sameRanges(frag.features[0].ranges, {{0, 3}}));
    CHECK(sameRanges(frag.features[1].ranges, {{3, 5}}));
    CHECK_EQ(frag.primers.size(), size_t(1));          // the primer on [3,7) lies inside
    CHECK(frag.primers[0].binding == Range({0, 4}));
    frag = extractFragment(c, {8, 4});                 // wraps: "AC" + "AC"
    CHECK_EQ(frag.bases, std::string("ACAC"));
    CHECK(sameRanges(frag.features[0].ranges, {{0, 4}}));
    t = s;
    CHECK_EQ(insertFragment(t, {0, 0}, frag), 0);
    CHECK_EQ(t.bases, std::string("ACACAAAACCCCGG"));
    CHECK(sameRanges(t.features.back().ranges, {{0, 4}}));
    CHECK(sameRanges(t.features[0].ranges, {{6, 10}}));
}

static void testSnapGeneWriter()
{
    Sequence s;
    s.name = "w";
    s.description = "A <test> & more";
    s.circular = true;
    s.bases = "ATGAAACCCGGGTTTTAGCATGCATG";   // 26
    Feature cds = feat("orf & co", {{0, 18}});
    cds.type = "CDS";
    cds.color = "#3366cc";
    cds.qualifiers = {{"codon_start", "1"}, {"note", "first"}, {"note", "x < y"}};
    s.features = {cds, feat("wrap", {{22, 26}, {0, 3}}, Strand::Reverse), feat("plain", {{5, 9}}, Strand::None)};
    s.primers = {{"fwd", "ATGAAACCC", {0, 9}, Strand::Forward}, {"rev", "CATGCATGC", {17, 26}, Strand::Reverse}};

    const std::string data = writeSnapGene(s);
    CHECK(looksLikeSnapGene(data));
    Sequence r;
    std::string err;
    CHECK(readSnapGene(data, &r, &err));
    CHECK_EQ(r.bases, s.bases);
    CHECK(r.circular);
    CHECK_EQ(r.description, s.description);
    CHECK_EQ(r.features.size(), size_t(3));
    if (r.features.size() == 3) {
        CHECK_EQ(r.features[0].name, std::string("orf & co"));
        CHECK_EQ(r.features[0].type, std::string("CDS"));
        CHECK_EQ(r.features[0].color, std::string("#3366cc"));
        CHECK(r.features[0].qualifiers == cds.qualifiers);
        CHECK(sameRanges(r.features[1].ranges, {{22, 26}, {0, 3}}));
        CHECK(r.features[1].strand == Strand::Reverse);
        CHECK(r.features[2].strand == Strand::None);
    }
    CHECK_EQ(r.primers.size(), size_t(2));
    if (r.primers.size() == 2) {
        CHECK(r.primers[1].binding == Range({17, 26}));
        CHECK(r.primers[1].strand == Strand::Reverse);
        CHECK_EQ(r.primers[0].bases, std::string("ATGAAACCC"));
    }
}

static void testDigest()
{
    // EcoRI G^AATTC at 10, BamHI G^GATCC at 30, PstI CTGCA^G (3' overhang) at 50.
    Sequence s;
    s.bases = std::string(10, 'T') + "GAATTC" + std::string(14, 'T') + "GGATCC" + std::string(14, 'T') + "CTGCAG"
            + std::string(20, 'T');   // 76 bp
    std::vector<Enzyme> use{*findEnzyme("EcoRI"), *findEnzyme("BamHI"), *findEnzyme("PstI")};
    DigestResult lin = digest(s, use);
    CHECK(!lin.uncut);
    CHECK_EQ(lin.fragments.size(), size_t(4));
    if (lin.fragments.size() == 4) {
        CHECK(lin.fragments[0].start == 0 && lin.fragments[0].length == 11);
        CHECK(lin.fragments[0].leftEnzyme.empty() && lin.fragments[0].rightEnzyme == "EcoRI");
        CHECK_EQ(lin.fragments[1].length, 20);
        CHECK_EQ(lin.fragments[2].rightOverhang, -4);   // PstI leaves a 3' overhang
        CHECK_EQ(lin.fragments[1].leftOverhang, 4);     // EcoRI leaves a 5' overhang
        int total = 0;
        for (const DigestFragment &f : lin.fragments)
            total += f.length;
        CHECK_EQ(total, s.length());
    }
    s.circular = true;
    DigestResult circ = digest(s, use);
    CHECK_EQ(circ.fragments.size(), size_t(3));
    if (circ.fragments.size() == 3) {
        CHECK_EQ(circ.fragments[2].length, 76 - 55 + 11);   // PstI to EcoRI, across the origin
        CHECK_EQ(circ.fragments[2].rightEnzyme, std::string("EcoRI"));
    }
    std::vector<Enzyme> none{*findEnzyme("NotI")};
    DigestResult uncut = digest(s, none);
    CHECK(uncut.uncut && uncut.fragments.size() == 1 && uncut.fragments[0].length == 76);
    CHECK(!commonLadders().empty() && commonLadders()[0].bands.front() > commonLadders()[0].bands.back());
}

int main(int argc, char **argv)
{
    testBasics();
    testLocations();
    testGenBankRoundTrip();
    testSnapGene();
    testEnzymes();
    testTm();
    testFastaAndIO();
    testEditing();
    testSnapGeneWriter();
    testDigest();
    if (argc > 1)
        testSamples(argv[1]);
    else
        std::fprintf(stderr, "note: no samples directory given, skipping sample tests\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
