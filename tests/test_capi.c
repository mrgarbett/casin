/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercises the C API from C, the way the Swift app uses it.
 * Usage: capi_tests <samples-dir> */
#include "openclone.h"

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

    char *status = oc_document_status_text(doc);
    CHECK(strstr(status, "3,188 bp") != NULL);
    oc_string_free(status);
    oc_document_free(doc);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
