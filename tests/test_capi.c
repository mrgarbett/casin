/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercises the C API from C, the way the Swift app uses it.
 * Usage: capi_tests <samples-dir> */
#include "restricto.h"

#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int changes = 0;
static void on_change(void *ctx, int what) { (void)ctx; changes |= what; }
static double measure(void *ctx, const char *text, double size, bool bold, bool mono)
{
    (void)ctx; (void)bold;
    return (double)strlen(text) * size * (mono ? 0.6 : 0.55);
}

int main(int argc, char **argv)
{
    char path[1024];
    char *err = NULL;
    if (argc < 2) return 2;
    snprintf(path, sizeof path, "%s/pOC-demo.dna", argv[1]);

    CHECK(strcmp(oc_version(), "0.1.0-alpha") == 0);
    OCDocument *doc = oc_document_new();
    oc_document_set_callback(doc, on_change, NULL);
    CHECK(oc_document_is_empty(doc));
    CHECK(!oc_document_load(doc, "/nonexistent/file.gb", &err));
    CHECK(err != NULL);
    oc_string_free(err);
    CHECK(oc_document_load(doc, path, NULL));
    CHECK(changes & OC_CHANGED_SEQUENCE);
    CHECK(strcmp(oc_document_name(doc), "pOC-demo") == 0);
    CHECK(oc_document_length(doc) == 3188);
    CHECK(oc_document_is_circular(doc));

    OCFeatureInfo f;
    CHECK(oc_document_feature_count(doc) == 11);
    CHECK(oc_document_feature(doc, 2, &f));
    CHECK(strcmp(f.name, "demoA") == 0 && strcmp(f.type, "CDS") == 0 && f.strand == OC_STRAND_FORWARD);
    oc_document_activate_feature(doc, 2);
    CHECK(oc_document_active_feature(doc) == 2);
    CHECK(oc_document_selection_start(doc) == f.start && oc_document_selection_length(doc) == f.length);
    char *bases = oc_document_selected_bases(doc);
    CHECK(strncmp(bases, "ATG", 3) == 0);
    oc_string_free(bases);

    /* Enzymes and the filter. */
    int unique = oc_document_visible_site_count(doc);
    oc_document_set_max_cuts(doc, 0);
    CHECK(oc_document_visible_site_count(doc) > unique);
    CHECK(changes & OC_CHANGED_ENZYMES);
    oc_document_set_max_cuts(doc, 1);
    oc_document_set_enzyme_hidden(doc, "EcoRI", true);
    CHECK(oc_document_visible_site_count(doc) == unique - 1);
    for (int i = 0; i < oc_document_enzyme_count(doc); ++i) {
        OCEnzymeInfo e;
        oc_document_enzyme(doc, i, &e);
        if (strcmp(e.name, "EcoRI") == 0)
            CHECK(e.hidden && e.cuts == 1);
    }

    /* Find on both strands. */
    int count = 0;
    CHECK(oc_document_find_next(doc, "GGTCTC", &count) >= 1);
    CHECK(count >= 2);   /* at least the two BsaI sites of the cassette */
    CHECK(oc_document_find_next(doc, "hello", &count) == -1);

    /* Newer tools: map options, ORFs, enzyme sets, printing. */
    {
        OCMapLayout *full = oc_map_layout_new(doc, 900, 700, false, measure, NULL);
        OCMapLayout *bare = oc_map_layout_new_options(doc, 900, 700, false, false, true, "CDS, misc_feature", measure, NULL);
        OCScene *a = oc_map_layout_render(full, doc), *b = oc_map_layout_render(bare, doc);
        CHECK(oc_scene_item_count(b) < oc_scene_item_count(a));
        oc_scene_free(a);
        oc_scene_free(b);
        oc_map_layout_free(full);
        oc_map_layout_free(bare);
        char *orfs = oc_find_orfs(doc, 100);
        CHECK(orfs && strchr(orfs, '\t') != NULL);
        oc_string_free(orfs);
        oc_document_activate_feature(doc, 6);   /* demoR, on the reverse strand */
        OCProteinInfo pinfo;
        char *protein = oc_document_selection_protein(doc, &pinfo);
        CHECK(protein && strncmp(protein, "MARCVG", 6) == 0 && pinfo.length == 180 && pinfo.pI > 11);
        oc_string_free(protein);
        oc_document_clear_selection(doc);
        CHECK(oc_document_selection_protein(doc, NULL) == NULL);
        char *sets = oc_builtin_enzyme_sets();
        CHECK(sets && strstr(sets, "BsaI") != NULL);
        oc_string_free(sets);
        char *sup = oc_enzyme_suppliers("EcoRI");
        CHECK(sup && strchr(sup, 'N') != NULL);
        oc_string_free(sup);
        const int pages = oc_print_page_count(doc, 760, 1060, measure, NULL);
        CHECK(pages >= 2);
        OCScene *page = oc_print_page(doc, 760, 1060, 1, measure, NULL);
        CHECK(page && oc_scene_width(page) == 760 && oc_scene_item_count(page) > 10);
        oc_scene_free(page);
        CHECK(oc_print_page(doc, 760, 1060, pages, measure, NULL) == NULL);
    }

    /* Layouts and scenes. */
    OCMapLayout *map = oc_map_layout_new(doc, 900, 700, false, measure, NULL);
    CHECK(oc_map_layout_is_circular(map));
    OCScene *scene = oc_map_layout_render(map, doc);
    int paths = 0, texts = 0;
    for (int i = 0; i < oc_scene_item_count(scene); ++i) {
        if (oc_scene_item_kind(scene, i) == OC_ITEM_PATH) {
            OCPathInfo p;
            CHECK(oc_scene_path(scene, i, &p) && p.point_count >= 2);
            ++paths;
        } else {
            OCTextInfo t;
            CHECK(oc_scene_text(scene, i, &t) && t.text != NULL);
            ++texts;
        }
    }
    CHECK(paths > 10 && texts > 10);
    CHECK(oc_scene_width(scene) == 900);
    oc_scene_free(scene);
    int kind = -1, index = -1, pos = -1;
    oc_map_layout_hit(map, 450, 350, &kind, &index, &pos);
    CHECK(kind == OC_HIT_SEQUENCE);
    oc_map_layout_free(map);

    OCSeqLayout *seq = oc_seq_layout_new(doc, 800, true, true, measure, NULL);
    CHECK(oc_seq_layout_total_height(seq) > 1000);
    scene = oc_seq_layout_render(seq, doc, 0, 600);
    CHECK(oc_scene_item_count(scene) > 50);
    oc_scene_free(scene);
    oc_seq_layout_free(seq);
    OCSeqOptions so = {6, true, false, true, false, "CDS"};
    seq = oc_seq_layout_new_options(doc, 800, true, &so, false, measure, NULL);
    CHECK(oc_seq_layout_total_height(seq) > 1000);
    oc_seq_layout_free(seq);

    /* Editing, undo and primers. */
    const int len = oc_document_length(doc);
    changes = 0;
    oc_document_set_selection(doc, 5, 0);
    CHECK(oc_document_type_bases(doc, "acgt"));
    CHECK(oc_document_length(doc) == len + 4);
    CHECK(oc_document_is_modified(doc) && oc_document_can_undo(doc));
    CHECK(changes & OC_CHANGED_HISTORY);
    char *label = oc_document_undo_label(doc);
    CHECK(strcmp(label, "Typing") == 0);
    oc_string_free(label);
    oc_document_undo(doc);
    CHECK(oc_document_length(doc) == len && !oc_document_is_modified(doc));
    int features = oc_document_feature_count(doc);
    int fi = oc_document_add_feature(doc, "tag", "misc_feature", OC_STRAND_REVERSE, len - 5, 10);
    CHECK(fi == features);
    OCFeatureInfo fin;
    CHECK(oc_document_feature(doc, fi, &fin) && fin.part_count == 2 && fin.length == 10);
    CHECK(strcmp(fin.name, "tag") == 0 && fin.strand == OC_STRAND_REVERSE);
    oc_document_update_feature(doc, fi, "tag2", "CDS", OC_STRAND_FORWARD, 100, 30);
    CHECK(oc_document_feature(doc, fi, &fin) && fin.part_count == 1 && fin.start == 100);
    oc_document_remove_feature(doc, fi);
    CHECK(oc_document_feature_count(doc) == features);
    int primers = oc_document_primer_count(doc);
    oc_document_set_selection(doc, 200, 20);
    CHECK(oc_document_add_primer_from_selection(doc, "p1", OC_STRAND_FORWARD) == primers);
    OCPrimerInfo pin;
    CHECK(oc_document_primer(doc, primers, &pin) && pin.start == 200 && pin.length == 20 && strlen(pin.bases) == 20);

    /* Milestone 3: digest, gel, primers, enzyme choice and cloning. */
    const char *eb[] = {"EcoRI", "BamHI"};
    OCFragmentInfo frags[8];
    CHECK(oc_digest(doc, eb, 2, frags, 8) == 2);
    CHECK(frags[0].length + frags[1].length == oc_document_length(doc));
    CHECK(strcmp(frags[0].right_enzyme, "EcoRI") == 0 || strcmp(frags[0].right_enzyme, "BamHI") == 0);
    CHECK(oc_ladder_count() >= 3 && strlen(oc_ladder_name(0)) > 0);
    OCScene *gel = oc_gel_render(doc, eb, 2, 0, 300, 300, false, measure, NULL);
    CHECK(gel && oc_scene_item_count(gel) > 10);
    oc_scene_free(gel);
    OCPrimerCheck pc;
    char *warn = oc_primer_check("GTAAAACGACGGCCAGTGAG", &pc);
    CHECK(pc.length == 20 && pc.tm > 50 && strcmp(warn, "") == 0);
    oc_string_free(warn);
    oc_document_set_selection(doc, 300, 400);
    CHECK(oc_document_design_primers(doc, 60, NULL));
    CHECK(oc_document_primer_count(doc) == primers + 3);
    OCDocument *pcr = oc_document_new();
    CHECK(oc_pcr_product(doc, primers + 1, primers + 2, pcr, NULL));
    CHECK(oc_document_length(pcr) == 400 && !oc_document_is_circular(pcr));
    oc_document_set_selection(doc, 1012, 60);   /* the MCS */
    char *chosen = oc_document_choose_enzymes(doc, 1, true, -1, OC_ENDS_ANY);
    CHECK(strstr(chosen, "EcoRI") != NULL && strstr(chosen, "BamHI") != NULL);
    oc_string_free(chosen);
    oc_document_show_only_enzymes(doc, eb, 2, 1);
    CHECK(oc_document_visible_site_count(doc) == 2);
    /* Put the PCR product between EcoRI and BamHI: it has no sites, so expect a clear error. */
    OCDocument *product = oc_document_new();
    char *cerr = NULL;
    CHECK(!oc_restriction_clone(doc, "EcoRI", "BamHI", pcr, "EcoRI", "BamHI", false, product, &cerr));
    CHECK(cerr != NULL);
    oc_string_free(cerr);
    /* Re-cloning the vector's own small EcoRI-BamHI piece gives the vector back. */
    CHECK(oc_restriction_clone(doc, "EcoRI", "BamHI", doc, "EcoRI", "BamHI", false, product, NULL));
    CHECK(oc_document_length(product) == oc_document_length(doc) && oc_document_is_modified(product));
    oc_document_set_selection(doc, 211, 3);
    CHECK(oc_mutagenesis(doc, "GCG", 60, product, NULL));
    CHECK(oc_document_length(product) == oc_document_length(doc) && oc_document_primer_count(product) >= 2);
    CHECK(oc_document_history_count(product) == 1 && strstr(oc_document_history(product, 0), "Mutagenesis") != NULL);
    /* The demo trace aligns with one point mutation. */
    OCDocument *trace = oc_document_new();
    char tpath[1024];
    snprintf(tpath, sizeof tpath, "%s/demo-trace.ab1", argv[1]);
    CHECK(oc_document_load(trace, tpath, NULL));
    OCAlignmentInfo ai;
    char *diffs = oc_align_read(doc, trace, &ai, NULL);
    CHECK(diffs && ai.diff_count == 1 && ai.ref_start == 950 && !ai.reverse);
    CHECK(diffs && strncmp(diffs, "1251\t", 5) == 0);
    oc_string_free(diffs);
    CHECK(oc_document_show_read(doc, trace, NULL) && oc_document_read_count(doc) == 1);
    CHECK(oc_document_show_read(doc, trace, NULL) && oc_document_read_count(doc) == 1);   /* replaced */
    oc_document_clear_reads(doc);
    CHECK(oc_document_read_count(doc) == 0);
    CHECK(oc_document_path(trace)[0] == '\0');   /* a trace isn't saved back over itself */
    /* Overlap extension PCR joins two fragments that share 25 bases. */
    OCDocument *fa = oc_document_new(), *fb = oc_document_new();
    CHECK(oc_document_new_sequence(fa, "fa", "ACGTTGCATGCCTAGGATCCGTAACCCCCCCCCCTTGACCGGTACCATGCAAGCTTGCA", false));
    CHECK(oc_document_new_sequence(fb, "fb", "TTGACCGGTACCATGCAAGCTTGCAGGGGGGGGGG", false));
    const OCDocument *soeFrags[] = {fa, fb};
    CHECK(oc_overlap_extension(soeFrags, 2, 15, product, NULL));
    CHECK(oc_document_length(product) == 59 + 35 - 25 && !oc_document_is_circular(product));
    CHECK(!oc_overlap_extension(soeFrags, 1, 15, product, NULL));
    oc_document_free(fa);
    oc_document_free(fb);
    /* Insertion by PCR: the PCR product into the vector at base 100. */
    oc_document_set_selection(doc, 100, 0);
    char *ins_primers = NULL;
    CHECK(oc_insertion_by_pcr(doc, pcr, 60, 20, product, &ins_primers, NULL));
    CHECK(oc_document_length(product) == oc_document_length(doc) + oc_document_length(pcr));
    CHECK(ins_primers && strchr(ins_primers, '\n') != NULL);
    oc_string_free(ins_primers);
    /* A primer added by sequence binds where it came from. */
    oc_document_set_selection(doc, 500, 22);
    char *pb = oc_document_selected_bases(doc);
    const int before = oc_document_primer_count(doc);
    const int pi = oc_document_add_primer_sequence(doc, "by-seq", pb, NULL);
    OCPrimerInfo pinf;
    CHECK(pi == before && oc_document_primer(doc, pi, &pinf) && pinf.start == 500 && pinf.length == 22);
    /* A tail typed in lowercase keeps its case. */
    char tailed[64];
    snprintf(tailed, sizeof tailed, "gaattc%s", pb);
    const int ti = oc_document_add_primer_sequence(doc, "tailed", tailed, NULL);
    CHECK(ti >= 0 && oc_document_primer(doc, ti, &pinf) && strncmp(pinf.bases, "gaattc", 6) == 0
          && pinf.length == 22);
    oc_string_free(pb);
    CHECK(oc_document_add_primer_sequence(doc, "short", "ACGT", NULL) == -1);
    char *hits = oc_search_folder(argv[1], "GAATTC");
    CHECK(hits && strstr(hits, "pOC-demo") != NULL);
    oc_string_free(hits);
    oc_document_free(trace);
    oc_document_free(product);
    oc_document_free(pcr);

    char *status = oc_document_status_text(doc);
    CHECK(strstr(status, "3,188 bp") != NULL);
    oc_string_free(status);
    oc_document_free(doc);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
