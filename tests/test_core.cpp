// SPDX-License-Identifier: GPL-3.0-or-later
// Headless tests for openclone_core. Usage: core_tests <samples-dir>
#include "core/abif.h"
#include "core/align.h"
#include "core/embl.h"
#include "core/orf.h"
#include "core/cloning.h"
#include "core/digest.h"
#include "core/edit.h"
#include "core/primercheck.h"
#include "core/enzymes.h"
#include "core/fasta.h"
#include "core/genbank.h"
#include "core/seqio.h"
#include "core/sequence.h"
#include "core/snapgene.h"
#include "core/tm.h"

#include <algorithm>
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

static void testEnzymeSets()
{
    CHECK(enzymeSuppliers("EcoRI").find('N') != std::string::npos);   // NEB sells EcoRI
    CHECK(enzymeSuppliers("NoSuchEnzyme").empty());
    const std::vector<EnzymeSet> builtin = builtinEnzymeSets();
    CHECK(builtin.size() > 3);
    bool bsa = false;
    for (const std::string &e : builtin.front().enzymes)
        bsa = bsa || e == "BsaI";
    CHECK(bsa);
    const std::vector<EnzymeSet> sets = parseEnzymeSets("# comment\nMCS: EcoRI, bamhi, Nope ,PstI\n\nEmpty:\n");
    CHECK_EQ(sets.size(), size_t(2));
    CHECK(sets.size() == 2 && sets[0].name == "MCS" && sets[0].enzymes.size() == 3 && sets[0].enzymes[1] == "BamHI");
    CHECK_EQ(formatEnzymeSets(sets), "MCS: EcoRI, BamHI, PstI\nEmpty:\n");
    CHECK_EQ(parseEnzymeSets(formatEnzymeSets(sets)).size(), size_t(2));
}

static void testEmbl(const std::string &dir)
{
    // A record in the ENA layout.
    const std::string text =
        "ID   X12345; SV 1; circular; genomic DNA; STD; SYN; 30 BP.\n"
        "XX\nAC   X12345;\nXX\nDE   Test vector,\nDE   two lines.\nXX\n"
        "FH   Key             Location/Qualifiers\nFH\n"
        "FT   CDS             complement(4..27)\n"
        "FT                   /gene=\"abc\"\n"
        "FT                   /note=\"a note that\n"
        "FT                   wraps\"\n"
        "XX\nSQ   Sequence 30 BP; 8 A; 7 C; 8 G; 7 T; 0 other;\n"
        "     atgcatgcaa ccggttaacc ggttaaccgg                                 30\n//\n";
    CHECK(looksLikeEmbl(text));
    Sequence e;
    std::string err;
    CHECK(readEmbl(text, &e, &err));
    CHECK_EQ(e.name, "X12345");
    CHECK(e.circular);
    CHECK_EQ(e.bases, "ATGCATGCAACCGGTTAACCGGTTAACCGG");
    CHECK_EQ(e.description, "Test vector, two lines.");
    CHECK(e.features.size() == 1 && e.features[0].strand == Strand::Reverse && e.features[0].ranges[0].start == 3);
    CHECK(e.features.size() == 1 && e.features[0].name == "abc");
    CHECK(e.features.size() == 1 && e.features[0].qualifiers.size() == 2 &&
          e.features[0].qualifiers[1].value == "a note that wraps");

    // Our own writer round-trips the demo plasmid.
    Sequence gb;
    CHECK(loadSequenceFile(dir + "/pOC-demo.gb", &gb, &err));
    addHistory(gb, "test step");
    Sequence back;
    const std::string out = writeEmbl(gb);
    CHECK(out.compare(0, 5, "ID   ") == 0);
    CHECK(readEmbl(out, &back, &err));
    CHECK_EQ(back.name, gb.name);
    CHECK_EQ(back.bases, gb.bases);
    CHECK(back.circular == gb.circular);
    CHECK_EQ(back.history.size(), gb.history.size());
    CHECK_EQ(back.features.size(), gb.features.size());
    for (size_t i = 0; i < std::min(back.features.size(), gb.features.size()); ++i) {
        CHECK_EQ(back.features[i].name, gb.features[i].name);
        CHECK(back.features[i].ranges.size() == gb.features[i].ranges.size());
    }
    CHECK(detectFormat("x.embl", out) == FileFormat::Embl && detectFormat("x.txt", out) == FileFormat::Embl);

    // Several records in one file.
    const std::string two = writeGenBank(gb) + writeGenBank(back);
    CHECK_EQ(splitRecords(two).size(), size_t(2));
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

    // A Sanger trace (made by scripts/make_demo_trace.py from pOC-demo).
    std::string ab1;
    CHECK(readFile(dir + "/demo-trace.ab1", &ab1, &err) && looksLikeAbif(ab1));
    Trace tr;
    CHECK(readAbif(ab1, &tr, &err));
    CHECK_EQ(tr.name, std::string("pOC-demo_fwd_read"));
    CHECK_EQ(tr.bases.size(), size_t(720));
    CHECK(tr.quality.size() == 720 && tr.peaks.size() == 720 && tr.scans() > 720 * 10);
    CHECK(tr.channels[0].size() == size_t(tr.scans()) && tr.channels[3].size() == size_t(tr.scans()));
    // The called base has the tallest channel at its peak.
    int agree = 0;
    for (size_t i = 0; i < tr.bases.size(); ++i) {
        const int slot = int(std::string("ACGT").find(tr.bases[i]));
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (tr.channels[size_t(k)][size_t(tr.peaks[i])] > tr.channels[size_t(best)][size_t(tr.peaks[i])])
                best = k;
        agree += best == slot ? 1 : 0;
    }
    CHECK(agree > 700);
    const Span good = goodRegion(tr);
    CHECK(good.start > 0 && good.start < 30 && good.start + good.length > 690 && good.start + good.length < 720);
    CHECK_EQ(gb.bases.substr(950, 300), tr.bases.substr(0, 300));
    Sequence asSeq;
    FileFormat fmt = FileFormat::Unknown;
    CHECK(loadSequenceFile(dir + "/demo-trace.ab1", &asSeq, &err, &fmt) && fmt == FileFormat::Abif);
    CHECK(asSeq.bases == tr.bases && !asSeq.circular);

    // The read aligns to the plasmid at 951.. and shows the one point mutation.
    ReadAlignment al;
    CHECK(alignRead(gb, tr.bases, &al, &err));
    CHECK(!al.reverse && al.refSpan.start == 950 && al.refSpan.length == 720);
    CHECK_EQ(al.diffs.size(), size_t(1));
    if (al.diffs.size() == 1)
        CHECK(al.diffs[0].refPos == 1250 && al.diffs[0].ref.size() == 1 && al.diffs[0].read.size() == 1);
    // The same read reversed aligns to the bottom strand.
    CHECK(alignRead(gb, reverseComplement(tr.bases), &al, &err) && al.reverse && al.refSpan.start == 950);

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

static void testPcr()
{
    Sequence t;
    t.name = "t";
    t.bases = "AAAAATTTGGGCCCAAATTTTTTTTTTTGGGAAACCCTTTTAAAAA";   // 46 bp
    t.features = {feat("mid", {{10, 30}})};
    Primer f{"f", "GAATTCTTTGGGCCC", {5, 14}, Strand::Forward};             // tail GAATTC + TTTGGGCCC
    Primer r{"r", "GGATCCAAAGGGTTTCCC", {28, 40}, Strand::Reverse};       // tail GGATCC + rc of [28,40)
    Sequence p;
    std::string err;
    CHECK(pcrProduct(t, f, r, &p, &err));
    CHECK_EQ(p.length(), 6 + 35 + 6);
    CHECK(startsWith(p.bases, "GAATTCTTTGGG"));
    CHECK(endsWith(p.bases, "GGATCC"));
    CHECK(sameRanges(p.features[0].ranges, {{11, 31}}));
    CHECK_EQ(p.primers.size(), size_t(2));
    CHECK(!pcrProduct(t, r, f, &p, &err));
}

static void testPrimerChecks()
{
    // A well-behaved 20-mer.
    PrimerCheck good = checkPrimer("GTAAAACGACGGCCAGTGAG");
    CHECK_EQ(good.length, 20);
    CHECK(good.tm > 52 && good.tm < 65);
    CHECK(good.hairpinStem < 4);
    CHECK(good.warnings.empty());
    // Palindromic: pairs with itself end to end.
    PrimerCheck pal = checkPrimer("GCGAATTCGCGAATTCGC");
    CHECK(pal.selfDimer >= 16 && pal.selfDimer3 >= 16);
    CHECK(!pal.warnings.empty());
    // GGGGC ... GCCCC folds back with a 5 bp stem and a 4 base loop.
    PrimerCheck hp = checkPrimer("ATGGGGCAAAAGCCCCATTA");
    CHECK(hp.hairpinStem >= 5);
    CHECK(checkPrimer("ATATATATTTAAAT").warnings.size() >= 2);   // short, low GC, no clamp
    // A pair whose 3' ends anneal.
    PairCheck pr = checkPrimerPair("ACGTTGCAAGTCAGGATCCG", "TTTTTTTTTTTTTTCGGATC");
    CHECK(pr.dimer3 >= 6);
    CHECK(!pr.warnings.empty());
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

    // EcoRI at 10, BamHI at 30, PstI at 50 on the circle; an "insert" spans 25..40.
    const std::vector<CutSite> sites = findSites(s, use);
    EnzymeCriteria crit;
    crit.inside = true;
    crit.region = {5, 30};   // 5..35 holds EcoRI and BamHI
    CHECK(chooseEnzymes(s, use, sites, crit) == std::vector<std::string>({"BamHI", "EcoRI"}));
    crit.spare = {{25, 15}};
    CHECK(chooseEnzymes(s, use, sites, crit) == std::vector<std::string>({"EcoRI"}));
    crit = {};
    crit.ends = EnzymeCriteria::Ends::ThreePrime;
    CHECK(chooseEnzymes(s, use, sites, crit) == std::vector<std::string>({"PstI"}));
    crit = {};
    crit.minCuts = 0;
    crit.maxCuts = 0;
    crit.region = {70, 20};   // across the origin: 70..76, 0..14 holds EcoRI's cut at 11
    crit.inside = true;
    CHECK(chooseEnzymes(s, use, sites, crit) == std::vector<std::string>({"EcoRI"}));
}

static void testCloning()
{
    // Vector: EcoRI at 10, BamHI at 40, circular 70 bp. Insert: EcoRI ... BamHI.
    Sequence v;
    v.name = "vec";
    v.circular = true;
    v.bases = std::string(10, 'A') + "GAATTC" + std::string(24, 'A') + "GGATCC" + std::string(30, 'A');
    v.features = {feat("ori", {{50, 60}})};
    Sequence ins;
    ins.name = "ins";
    ins.bases = "TTGAATTC" + std::string(20, 'C') + "GGATCCTT";   // 36 bp
    ins.features = {feat("gene", {{10, 26}})};
    std::vector<Enzyme> use{*findEnzyme("EcoRI"), *findEnzyme("BamHI")};
    auto sitesOf = [&](const Sequence &s) {
        std::vector<CutSite> all = findSites(s, use);
        CutSite eco, bam;
        for (const CutSite &c : all)
            (c.enzyme->name == "EcoRI" ? eco : bam) = c;
        return std::make_pair(eco, bam);
    };
    auto [vEco, vBam] = sitesOf(v);
    auto [iEco, iBam] = sitesOf(ins);
    ClonePiece backbone, insert;
    std::string err;
    CHECK(cutPiece(v, vBam, vEco, &backbone, &err));   // across the origin
    CHECK(cutPiece(ins, iEco, iBam, &insert, &err));
    CHECK_EQ(backbone.left.bases, std::string("GATC"));
    CHECK_EQ(backbone.right.bases, std::string("AATT"));
    CHECK_EQ(insert.left.overhang, 4);
    Sequence product;
    CHECK(ligateCircular({backbone, insert}, &product, &err));
    CHECK(product.circular);
    CHECK_EQ(product.length(), 76 - 41 + 11 + 29 - 3);   // backbone + insert between the top-strand cuts
    CHECK((product.bases + product.bases).find("GAATTC" + std::string(20, 'C') + "GGATCC") != std::string::npos);
    bool hasGene = false, hasOri = false;
    for (const Feature &f : product.features) {
        hasGene = hasGene || f.name == "gene";
        hasOri = hasOri || f.name == "ori";
    }
    CHECK(hasGene && hasOri);
    CHECK_EQ(backbone.originOffset(), 76 - 41);   // the vector's base 1 sits 35 bases into the backbone
    CHECK_EQ(insert.originOffset(), -1);
    CHECK(!ligateCircular({backbone, flipPiece(insert)}, &product, &err));
    CHECK(!err.empty());
    // A blunt piece closes on itself; a sticky piece with mismatched ends doesn't.
    Sequence b;
    b.bases = "AAAAGATATCAAAAAAAAGATATCAAAA";
    const std::vector<Enzyme> ecoRV{*findEnzyme("EcoRV")};   // sites point into this vector
    std::vector<CutSite> ev = findSites(b, ecoRV);
    CHECK_EQ(ev.size(), size_t(2));
    if (ev.size() == 2) {
        ClonePiece p;
        CHECK(cutPiece(b, ev[0], ev[1], &p));
        CHECK_EQ(p.length(), 14);
        CHECK(ligateCircular({p}, &product));
        CHECK(ligateCircular({flipPiece(p)}, &product));
    }
    CHECK(!ligateCircular({insert}, &product));
    // pieceBetween picks the backbone and the insert without caring about order.
    ClonePiece bb, in;
    CHECK(pieceBetween(v, vEco, vBam, true, &bb) && bb.length() == backbone.length());
    CHECK(pieceBetween(ins, iBam, iEco, false, &in) && in.length() == insert.length());
    CHECK(pieceBetween(v, vEco, vEco, true, &bb) && bb.length() == v.length());   // linearized
    // The whole workflow by enzyme names, with the vector's numbering kept.
    CHECK(restrictionClone(v, "EcoRI", "BamHI", ins, "EcoRI", "BamHI", false, &product, &err));
    CHECK_EQ(product.length(), 76 - 41 + 11 + 29 - 3);
    CHECK_EQ(product.name, std::string("vec-ins"));
    CHECK_EQ(product.history.size(), size_t(1));
    CHECK(product.history.size() == 1 && product.history[0].find("Restriction cloning: ins (EcoRI, BamHI) into vec") != std::string::npos);
    // The history survives GenBank and .dna round trips.
    product.history.insert(product.history.begin(), "2026-01-02 PCR with a & b <x>");
    Sequence back;
    CHECK(readGenBank(writeGenBank(product), &back, &err) && back.history == product.history);
    back = {};
    CHECK(readSnapGene(writeSnapGene(product), &back, &err) && back.history == product.history);
    CHECK(startsWith(product.bases, std::string(10, 'A') + "GAATTC"));
    CHECK(!restrictionClone(v, "EcoRI", "BamHI", ins, "EcoRI", "BamHI", true, &product, &err));
    CHECK(!restrictionClone(v, "EcoRI", "NotI", ins, "EcoRI", "BamHI", false, &product, &err));
}

static void testGoldenGate()
{
    const Enzyme &bsa = *findEnzyme("BsaI");
    // Vector: AATG <GAGACC dropout GGTCTC> GCTT; the backbone keeps both overhangs.
    Sequence vec;
    vec.name = "dest";
    vec.circular = true;
    vec.bases = std::string(20, 'A') + "AATGAGAGACC" + std::string(30, 'G') + "GGTCTCAGCTT" + std::string(40, 'T');
    // Part A AATG..GGAG, part B GGAG..GCTT, both in plasmids with inward sites.
    auto part = [](const std::string &name, const std::string &left, const std::string &body, const std::string &right) {
        Sequence p;
        p.name = name;
        p.circular = true;
        p.bases = "CCCCC" "GGTCTCA" + left + body + right + "TGAGACC" "CCCCCCCCCC";
        return p;
    };
    Sequence a = part("A", "AATG", "ACGTACGTAC", "GGAG");
    Sequence b = part("B", "GGAG", "TTGACCAACC", "GCTT");
    ClonePiece pa;
    std::string err;
    CHECK(goldenGatePiece(a, bsa, &pa, &err));
    CHECK_EQ(pa.left.bases, std::string("AATG"));
    CHECK_EQ(pa.right.bases, std::string("GGAG"));
    CHECK_EQ(pa.length(), 4 + 10);
    // B given as its reverse complement and listed before A: the order and orientation are found.
    Sequence bRc = b;
    reverseComplementSequence(bRc);
    Sequence product;
    std::vector<AssemblyStep> steps;
    CHECK(goldenGate({vec, bRc, a}, bsa, &product, &steps, &err));
    CHECK_EQ(product.length(), 20 + 40 + 4 + 14 + 14);
    CHECK_EQ(steps.size(), size_t(3));
    if (steps.size() == 3) {
        CHECK(steps[1].input == 2 && !steps[1].flipped);
        CHECK(steps[2].input == 1 && steps[2].flipped);
    }
    CHECK(startsWith(product.bases, std::string(20, 'A') + "AATGACGTACGTACGGAGTTGACCAACCGCTT"));
    CHECK(product.bases.find("GGTCTC") == std::string::npos && product.bases.find("GAGACC") == std::string::npos);
    // Missing part B: the circle can't close.
    CHECK(!goldenGate({vec, a}, bsa, &product, nullptr, &err));
    CHECK(!err.empty());
}

static void testGibson()
{
    const std::string h1 = "ACGTTGCATGCCTAGGATCCGTAAC";   // 25 bp junctions
    const std::string h2 = "TTGACCGGTACCATGCAAGCTTGCA";
    const std::string h3 = "GGCATCAAGTCGACTTCAGCGTAGC";
    Sequence vec, a, b;
    vec.name = "vec";
    vec.bases = h3 + std::string(60, 'A') + h1;
    vec.features = {feat("bb", {{30, 70}})};
    a.name = "a";
    a.bases = h1 + std::string(40, 'C') + h2;
    a.features = {feat("geneA", {{25, 65}})};
    b.name = "b";
    b.bases = h2 + std::string(30, 'G') + h3;
    Sequence bRc = b;
    reverseComplementSequence(bRc);
    Sequence product;
    std::vector<GibsonJoin> joins;
    std::string err;
    CHECK(gibsonAssemble({vec, bRc, a}, 15, &product, &joins, &err));
    const int expect = 25 + 60 + 25 + 40 + 25 + 30;   // each junction once
    CHECK_EQ(product.length(), expect);
    CHECK(product.circular);
    CHECK(startsWith(product.bases, h3 + std::string(60, 'A') + h1 + std::string(40, 'C') + h2 + std::string(30, 'G')));
    CHECK_EQ(joins.size(), size_t(3));
    if (joins.size() == 3) {
        CHECK(joins[1].input == 2 && !joins[1].flipped && joins[1].overlap == 25);
        CHECK(joins[2].input == 1 && joins[2].flipped);
        CHECK_EQ(joins[0].overlap, 25);
    }
    bool hasA = false;
    for (const Feature &f : product.features)
        hasA = hasA || (f.name == "geneA" && sameRanges(f.ranges, {{110, 150}}));
    CHECK(hasA);
    CHECK(!gibsonAssemble({vec, a}, 15, &product, nullptr, &err));   // b missing: no closing overlap
    CHECK(!gibsonAssemble({vec, a, b}, 30, &product, nullptr, &err));   // overlaps too short
}

static void testPrimerDesign()
{
    Sequence s;
    s.name = "lacZ";
    s.bases = "TTTTTTTTTT" "ATGACCATGATTACGCCAAGCTTGCATGCCTGCAGGTCGACTCTAGAGGATCCCCGGGTACCGAGCTCGAATTCACTGGCCGTCGTTTTACAACG"
              "TCGTGACTGGGAAAACCCTGGCGTTACCCAACTTAATCGCCTTGCAGCACATCC" "TTTTTTTTTT";
    const Span target{10, s.length() - 20};
    PrimerDesign d;
    std::string err;
    CHECK(designPrimers(s, target, 60, &d, &err));
    CHECK(startsWith(d.forward.bases, "ATGACCATGATTACG"));
    CHECK_EQ(d.forward.binding.start, 10);
    CHECK_EQ(d.reverse.binding.end, s.length() - 10);
    CHECK(startsWith(d.reverse.bases, reverseComplement(s.bases.substr(size_t(s.length() - 25), 15))));
    CHECK(d.forward.bases.size() >= 18 && d.forward.bases.size() <= 32);
    CHECK(std::fabs(d.forwardCheck.tm - 60) < 6 && std::fabs(d.reverseCheck.tm - 60) < 6);
    Sequence p;
    CHECK(pcrProduct(s, d.forward, d.reverse, &p, &err));
    CHECK_EQ(p.length(), target.length);
    CHECK(!designPrimers(s, {0, 10}, 60, &d, &err));
    // With a search window, a 5 bp target in the middle gets flanking primers.
    const Span snp{80, 5};
    CHECK(designPrimers(s, snp, 60, &d, &err, 60));
    CHECK(d.forward.binding.start >= 20 && d.forward.binding.start <= 80);
    CHECK(d.reverse.binding.end >= 85 && d.reverse.binding.end <= 145);
    CHECK(pcrProduct(s, d.forward, d.reverse, &p, &err));
    CHECK(p.bases.find(s.bases.substr(80, 5)) != std::string::npos);
    CHECK_EQ(findPrimerSites(s, d.reverse.bases).size(), size_t(1));

    // Binding sites: the designed primers bind where they were made, a tailed
    // primer binds with its 3' part, and one mismatch is tolerated.
    std::vector<PrimerSite> fs = findPrimerSites(s, d.forward.bases);
    CHECK(!fs.empty() && fs[0].binding == d.forward.binding && fs[0].strand == Strand::Forward && fs[0].mismatches == 0);
    std::vector<PrimerSite> rs = findPrimerSites(s, "GGATCC" + d.reverse.bases);
    CHECK(!rs.empty() && rs[0].binding == d.reverse.binding && rs[0].strand == Strand::Reverse);
    std::string mutated = d.forward.bases;
    mutated[3] = mutated[3] == 'A' ? 'C' : 'A';
    std::vector<PrimerSite> ms = findPrimerSites(s, mutated);
    CHECK(!ms.empty() && ms[0].mismatches == 1 && ms[0].binding == d.forward.binding);
    CHECK(findPrimerSites(s, "GGGGGGGGGGGGGGGGGGGG").empty());
}

static void testMutagenesis()
{
    Sequence s;
    s.name = "p";
    s.circular = true;
    s.bases = "GCGTTGCATGCCTAGGATCCGTAACGGCATCAAGTCGACTTCAGCGTAGCATGAAACCCGGGTTTTAGTTGACCGGTACCATGCAAGCTTGCAGCAGGTCAG"
              "CATCGATCGGCTAGCTAGGCGCGCCATCGAT";
    s.features = {feat("orf", {{50, 68}})};
    const int n = s.length();
    // Substitute AAA (Lys) at 53..56 with GCG (Ala).
    MutagenesisDesign d;
    std::string err;
    CHECK(designMutagenesis(s, {53, 3}, "GCG", 60, &d, &err));
    CHECK_EQ(d.mutant.length(), n);
    CHECK_EQ(d.mutant.bases.substr(50, 9), std::string("ATGGCGCCC"));
    CHECK(startsWith(d.forward.bases, "GCGCCCGGG"));
    CHECK_EQ(d.forward.binding.start, 56);
    CHECK_EQ(d.reverse.binding.end, 53);
    CHECK(d.forwardTm > 52 && d.reverseTm > 52);
    CHECK(sameRanges(d.mutant.features[0].ranges, {{50, 68}}));
    CHECK_EQ(d.mutant.primers.size(), size_t(2));
    // The primers amplify the whole mutant from the change, back to back.
    Sequence lin;
    CHECK(pcrProduct(d.mutant, d.mutant.primers[0], d.mutant.primers[1], &lin, &err));
    CHECK_EQ(lin.length(), n);
    // Deletion and insertion.
    CHECK(designMutagenesis(s, {53, 3}, "", 60, &d, &err) && d.mutant.length() == n - 3);
    CHECK(designMutagenesis(s, {53, 0}, "CATCAT", 60, &d, &err) && d.mutant.length() == n + 6);
    CHECK(!designMutagenesis(s, {5, 3}, "GCG", 60, &d, &err));   // too close to the origin
}

static void testAlign()
{
    Sequence ref;
    ref.circular = true;
    ref.bases = "ATGACCATGATTACGCCAAGCTTGCATGCCTGCAGGTCGACTCTAGAGGATCCCCGGGTACCGAGCTCGAATTCACTGGCCGTCGTTTTACAACG"
                "TCGTGACTGGGAAAACCCTGGCGTTACCCAACTTAATCGCCTTGCAGCACATCC";   // 150
    // A read across the origin with a deletion of 2 and an insertion of 1.
    std::string read = ref.bases.substr(110) + ref.bases.substr(0, 40);
    read.erase(20, 2);                // deletes ref 130..131
    read.insert(50, "T");             // inserts before ref ... (read pos 50)
    ReadAlignment al;
    std::string err;
    CHECK(alignRead(ref, read, &al, &err));
    CHECK_EQ(al.refSpan.start, 110);
    CHECK_EQ(al.refSpan.length, 149 - 110 + 40);
    CHECK_EQ(al.gaps, 3);   // two deleted bases and one inserted
    bool del = false, ins = false;
    for (const AlignmentDiff &d : al.diffs) {
        del = del || (d.ref.size() == 2 && d.read.empty());
        ins = ins || (d.ref.empty() && d.read == "T");
    }
    CHECK(del && ins);
    // Laid over the reference: one character per reference base, gaps as '-'.
    const ReadTrack track = readTrack(al, ref.length(), "r1");
    CHECK_EQ(int(track.bases.size()), al.refSpan.length);
    CHECK_EQ(int(std::count(track.bases.begin(), track.bases.end(), '-')), 2);
    CHECK_EQ(int(track.insertions.size()), 1);
    CHECK(track.insertions.size() == 1 && track.insertions[0] < 40);   // after the origin
    CHECK_EQ(track.bases.substr(0, 10), ref.bases.substr(110, 10));
    CHECK(!alignRead(ref, "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG", &al, &err));
}

static void testOrfs()
{
    // A 12-codon ORF on the top strand, and one on the bottom strand across the origin.
    const std::string orfTop = "ATG" + std::string("GCT") + "AAAGCTGCTGCTGCTGCTGCTGCTGCT" + "TAA";   // 11 codons + stop
    Sequence s;
    s.circular = true;
    const std::string bottomOrf = reverseComplement("ATGCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCTAG");   // 12 codons + stop
    s.bases = bottomOrf.substr(20) + "CCCCC" + orfTop + "CCCCCCCCCC" + bottomOrf.substr(0, 20);
    const std::vector<Orf> orfs = findOrfs(s, 10);
    CHECK(orfs.size() >= 2);   // the filler happens to hold a longer bottom-strand ORF too
    bool top = false, bottom = false;
    for (const Orf &o : orfs) {
        const Feature f = orfFeature(s, o, "orf");
        std::string t = translate(s.featureBases(f));
        CHECK(!t.empty() && t.back() == '*');
        if (!t.empty())
            t.pop_back();
        CHECK_EQ(t, o.protein);
        top = top || (o.strand == Strand::Forward && o.protein == "MAKAAAAAAAA");
        bottom = bottom || (o.strand == Strand::Reverse && f.ranges.size() == 2 && o.protein.size() == 12);
    }
    CHECK(top && bottom);
    CHECK(findOrfs(s, 20).empty());
    CHECK(findOrfs(s, 12).size() == orfs.size() - 1);   // the 11-codon one drops out
    s.circular = false;
    for (const Orf &o : findOrfs(s, 10))
        CHECK(o.protein != "MPPPPPPPPPPP");   // the one across the origin is gone
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
    testPrimerChecks();
    testPcr();
    testCloning();
    testGoldenGate();
    testGibson();
    testPrimerDesign();
    testMutagenesis();
    testAlign();
    testOrfs();
    testEnzymeSets();
    if (argc > 1) {
        testEmbl(argv[1]);
        testSamples(argv[1]);
    } else
        std::fprintf(stderr, "note: no samples directory given, skipping sample tests\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
