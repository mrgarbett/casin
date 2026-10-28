// SPDX-License-Identifier: GPL-3.0-or-later
// Layout tests for restricto_view, using approximate text metrics.
// Usage: view_tests <samples-dir>
#include "core/abif.h"
#include "core/enzymes.h"
#include "core/seqio.h"
#include "view/document.h"
#include "view/gellayout.h"
#include "view/maplayout.h"
#include "view/seqlayout.h"
#include "view/printlayout.h"
#include "view/tracelayout.h"

#include <cstdio>

using namespace oc;
using namespace oc::view;

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

#define CHECK_EQ(a, b) CHECK((a) == (b))

static void testUndo(const Sequence &seq)
{
    Document doc;
    int histories = 0;
    doc.onHistoryChanged([&] { ++histories; });
    doc.setSequence(seq);
    const std::string original = doc.sequence().bases;
    const size_t features = doc.sequence().features.size();
    CHECK(!doc.isModified() && !doc.canUndo());

    doc.setSelection({10, 0});
    CHECK(doc.typeBases("gg aa"));
    CHECK(!doc.typeBases("123"));
    CHECK(!doc.typeBases("N"));   // only A, C, G and T can be typed
    CHECK(doc.sequence().shown(10, 4) == "ggaa" && doc.sequence().bases.substr(10, 4) == "GGAA");
    CHECK_EQ(doc.sequence().length(), int(original.size()) + 4);
    CHECK(doc.selection().start == 14 && doc.selection().length == 0);
    CHECK(doc.isModified() && doc.canUndo());
    CHECK(doc.undoLabel() == "Typing");
    doc.typeBases("C");                       // more typing joins the same step
    doc.typeBases("C");
    doc.undo();
    CHECK(doc.sequence().bases == original);
    doc.redo();
    CHECK_EQ(doc.sequence().length(), int(original.size()) + 6);
    doc.undo();
    doc.setSelection({10, 0});
    doc.typeBases("ggaa");

    doc.setSelection({0, 5});
    doc.deleteBackward();
    CHECK_EQ(doc.sequence().length(), int(original.size()) - 1);
    doc.reverseComplementAll();
    doc.undo();
    doc.undo();
    CHECK_EQ(doc.sequence().length(), int(original.size()) + 4);
    doc.undo();
    CHECK(doc.sequence().bases == original);
    CHECK(!doc.isModified() && doc.canRedo());
    doc.redo();
    CHECK(doc.isModified());
    doc.markSaved();
    CHECK(!doc.isModified());
    doc.undo();
    CHECK(doc.isModified());

    Feature f;
    f.type = "misc_feature";
    f.name = "new";
    f.ranges = {{0, 30}};
    CHECK_EQ(doc.addFeature(f), int(features));
    CHECK_EQ(doc.activeFeature(), int(features));
    doc.removeFeature(0);
    CHECK_EQ(doc.sequence().features.size(), features);
    doc.setSelection({100, 20});
    CHECK_EQ(doc.addPrimerFromSelection("", Strand::Reverse), int(seq.primers.size()));
    CHECK(doc.sequence().primers.back().bases == reverseComplement(doc.sequence().bases.substr(100, 20)));
    CHECK(histories > 8);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: view_tests <samples-dir>\n");
        return 2;
    }
    Sequence seq;
    std::string err;
    if (!loadSequenceFile(std::string(argv[1]) + "/pOC-demo.gb", &seq, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    ApproxTextMeasurer tm;
    const Theme theme = Theme::light();

    // All cutters: many labels. None may overlap and all stay inside the view.
    const std::vector<CutSite> sites = findSites(seq, commonEnzymes());
    for (auto size : {std::pair<double, double>{900, 700}, {600, 500}, {1400, 900}, {330, 540}}) {
        MapLayout map(seq, sites, theme, tm, size.first, size.second);
        MapState st;
        Scene sc = map.render(st);
        CHECK(!sc.items.empty());
        std::vector<Rect> labels;
        for (const Item &it : sc.items)
            if (auto *t = std::get_if<TextItem>(&it))
                if (t->valign == VAlign::Middle && t->angle == 0 && (t->halign == HAlign::Left || t->halign == HAlign::Right)) {
                    const double w = tm.width(t->text, t->font), h = tm.height(t->font);
                    labels.push_back({t->halign == HAlign::Right ? t->pos.x - w : t->pos.x, t->pos.y - h / 2, w, h});
                }
        CHECK(labels.size() > 10);
        for (size_t i = 0; i < labels.size(); ++i) {
            CHECK(labels[i].y >= 0 && labels[i].y + labels[i].h <= size.second);
            CHECK(labels[i].x >= 0 && labels[i].x + labels[i].w <= size.first);
            for (size_t j = i + 1; j < labels.size(); ++j)
                CHECK(!labels[i].intersects(labels[j]));
        }
        // Clicking the middle of the map's right edge region hits the sequence.
        MapHit h = map.hitTest({size.first / 2, size.second / 2});
        CHECK(h.kind == MapHit::Sequence);
    }

    // Linear layout of the same sequence.
    Sequence lin = seq;
    lin.circular = false;
    const std::vector<CutSite> linSites = findSites(lin, commonEnzymes());
    MapLayout lmap(lin, linSites, theme, tm, 1000, 500);
    CHECK(!lmap.render({}).items.empty());
    CHECK(lmap.positionAt({0, 250}) == 0);
    {
        // Display options: hiding enzymes or a feature type draws less.
        const size_t all = MapLayout(seq, sites, theme, tm, 1000, 800).render({}).items.size();
        MapOptions noEnz;
        noEnz.enzymes = false;
        MapOptions noCds;
        noCds.hiddenTypes = {"CDS"};
        CHECK(MapLayout(seq, sites, theme, tm, 1000, 800, noEnz).render({}).items.size() < all);
        CHECK(MapLayout(seq, sites, theme, tm, 1000, 800, noCds).render({}).items.size() < all);
    }

    // Sequence view: lines are multiples of 10 bp and positions round-trip.
    SequenceLayout sl(seq, sites, theme, tm, 900, true);
    CHECK(sl.basesPerLine() % 10 == 0);
    CHECK(sl.totalHeight() > 0);
    // A click on the first line's first base gives position 0; further right, later.
    const double y0 = sl.lineTopFor(0) + sl.lineHeightFor(0) / 2;
    CHECK(sl.positionAt({0, y0}) == 0);
    // Hovering: the first base under the mouse on the first line is base 0;
    // above the strands (the enzyme labels) is no base.
    int firstBase = -1;
    for (double y = sl.lineTopFor(0); y < sl.lineTopFor(0) + sl.lineHeightFor(0) && firstBase < 0; y += 1)
        for (double x = 0; x < 900 && firstBase < 0; x += 1)
            firstBase = sl.baseAt({x, y});
    CHECK(firstBase == 0);
    CHECK(sl.baseAt({300, sl.lineTopFor(0)}) == -1);
    CHECK(sl.positionAt({300, y0}) > 0);
    CHECK(sl.positionAt({300, sl.lineTopFor(sl.basesPerLine()) + 5}) > sl.basesPerLine());
    Scene ss = sl.render(0, 600, {});
    CHECK(!ss.items.empty());
    {
        // Six-frame translation shows only ORFs: +1 reads ATG AAA TAG; no other
        // frame has one, so the line gets a single frame row.
        Sequence orf;
        orf.bases = "ATGAAATAGC";
        const std::vector<CutSite> none;
        SeqOptions sixOpt;
        sixOpt.frames = 6;
        sixOpt.minOrfCodons = 2;
        SequenceLayout plain(orf, none, theme, tm, 900, true), six(orf, none, theme, tm, 900, true, sixOpt);
        CHECK(six.totalHeight() > plain.totalHeight() + 10);
        CHECK(six.totalHeight() < plain.totalHeight() + 2 * 20);
        // The map draws that ORF as an arrow (a line and a head) with the
        // translation on, linear and circular.
        MapOptions mapOrfs;
        mapOrfs.frames = 6;
        mapOrfs.minOrfCodons = 2;
        for (bool circular : {false, true}) {
            orf.circular = circular;
            CHECK(MapLayout(orf, none, theme, tm, 600, 400, mapOrfs).render({}).items.size()
                  == MapLayout(orf, none, theme, tm, 600, 400).render({}).items.size() + 2);
        }
        orf.circular = false;
        std::string letters;
        bool ends = false;
        for (const Item &it : six.render(0, 600, {}).items)
            if (auto *t = std::get_if<TextItem>(&it)) {
                if (t->text.size() == 1 && t->font.mono)
                    letters += t->text;
                ends = ends || t->text == "5\u2032";
            }
        CHECK(letters.find("MK*") != std::string::npos);
        CHECK(ends);
        // Clicking an amino acid finds its codon.
        int hit = -1;
        for (double y = 0; y < six.totalHeight() && hit < 0; y += 1)
            for (double x = 0; x < 300 && hit < 0; x += 2)
                hit = six.codonAt({x, y});
        CHECK(hit == 0 || hit == 3 || hit == 6);
        // Hovering it outlines the codon: one more shape on that line.
        SeqViewState hovering;
        hovering.hoverCodon = hit;
        CHECK(six.render(0, 400, hovering).items.size() == six.render(0, 400, {}).items.size() + 1);
        // Too short for the default minimum: no frame rows at all.
        CHECK(SequenceLayout(orf, none, theme, tm, 900, true, 6).totalHeight() == plain.totalHeight());
        // Grouping in tens widens a line; hiding enzymes drops their label rows.
        SeqOptions grouped;
        grouped.groupTens = true;
        CHECK(SequenceLayout(seq, sites, theme, tm, 900, true, grouped).basesPerLine() <= sl.basesPerLine());
        // A primer with a 5' tail: its bases are drawn as typed.
        Sequence withPrimer = seq;
        withPrimer.primers.push_back({"tailed", "gaattc" + withPrimer.bases.substr(100, 20), {100, 120}, Strand::Forward});
        std::string drawn;
        for (const Item &it : SequenceLayout(withPrimer, sites, theme, tm, 900, true).render(0, 2000, {}).items)
            if (auto *t = std::get_if<TextItem>(&it))
                if (t->text.size() == 1)
                    drawn += t->text;
        CHECK(drawn.find("gaattc") != std::string::npos);
        // A tail typed in uppercase stays uppercase.
        withPrimer.primers.back().bases = "GAATTC" + withPrimer.bases.substr(100, 20);
        drawn.clear();
        for (const Item &it : SequenceLayout(withPrimer, sites, theme, tm, 900, true).render(0, 2000, {}).items)
            if (auto *t = std::get_if<TextItem>(&it))
                if (t->text.size() == 1)
                    drawn += t->text;
        CHECK(drawn.find("gaattc") == std::string::npos);
        SeqOptions bare;
        bare.enzymes = bare.features = bare.primers = false;
        CHECK(SequenceLayout(seq, sites, theme, tm, 900, true, bare).totalHeight() < sl.totalHeight());
        CHECK(six.positionAt({0, six.lineTopFor(0) + six.lineHeightFor(0) / 2}) == 0);
        // An aligned read adds a row; its mismatch is drawn and the row has a tooltip.
        ReadTrack r;
        r.name = "read1";
        r.refSpan = {0, 10};
        r.bases = "ATGAACTAGC";
        SequenceLayout withRead(orf, none, theme, tm, 900, true, 0, {r});
        CHECK(withRead.totalHeight() > plain.totalHeight());
        bool found = false;
        for (double y = 0; y < withRead.totalHeight() && !found; y += 1)
            found = withRead.readAt({200, y}) == 0;
        CHECK(found);
    }
    {
        // Printing: the map, then the sequence in whole lines across pages.
        PrintSettings ps;
        const std::vector<Scene> pages = printPages(seq, sites, {}, tm, ps);
        CHECK(pages.size() >= 2);
        for (const Scene &pg : pages)
            CHECK(pg.width == ps.width && pg.height == ps.height && !pg.items.empty());
        ps.map = false;
        CHECK_EQ(printPages(seq, sites, {}, tm, ps).size(), pages.size() - 1);
    }
    testUndo(seq);
    {
        GelLayout gel({{"Ladder", {10000, 1000, 100}, {1000}, true, false}, {"Cut", {3000, 500}, {}, false, false}},
                      theme, tm, 500, 400);
        CHECK(gel.yOfSize(10000) < gel.yOfSize(1000) && gel.yOfSize(1000) < gel.yOfSize(100));
        CHECK(!gel.render().items.empty());
    }
    {
        std::string data;
        Trace tr;
        CHECK(readFile(std::string(argv[1]) + "/demo-trace.ab1", &data, &err) && readAbif(data, &tr, &err));
        TraceLayout tl(tr, theme, 190);
        CHECK(tl.totalWidth() > 1000);
        CHECK(tl.baseAt(tl.xOfBase(100)) == 100 && tl.baseAt(tl.xOfBase(100) + 3) == 100);
        CHECK(tl.baseAt(-50) == 0 && tl.baseAt(1e9) == int(tr.bases.size()) - 1);
        Scene ts = tl.render(tl.xOfBase(300) - 200, 400, {300, 1});
        CHECK(ts.items.size() > 30 && ts.width == 400);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
