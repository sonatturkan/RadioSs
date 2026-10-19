/* RadioS otomasyon uçtan uca test düzeneği.
 *
 * Uygulamanın şema yolunu (Yayın › Şema › Şemayı çalıştır) gerçek C
 * kütüphaneleriyle adım adım çalıştırır: kuşak seçimi → kapsam yönlendirmesi →
 * hazırlık yapılandırması → takvim planı → birleştirme → yerel oynatma planı.
 * Backend çağrıları (desktop-playout/prepare) Python tarafında yapılır.
 * Hiçbir adım veritabanına yazmaz ve ses çalmaz. */
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radios/weekly_programme_plan.h"
#include "radios/automation_calendar_plan.h"
#include "radios/automation_scope.h"
#include "radios/playout.h"

static JsonNode *load(const char *path)
{
    JsonParser *p = json_parser_new(); GError *e = NULL;
    if (!json_parser_load_from_file(p, path, &e)) { fprintf(stderr, "parse %s: %s\n", path, e->message); exit(3); }
    JsonNode *n = json_node_copy(json_parser_get_root(p)); g_object_unref(p); return n;
}
static void dump(JsonNode *n, const char *path)
{
    JsonGenerator *g = json_generator_new(); json_generator_set_root(g, n); json_generator_set_pretty(g, TRUE);
    json_generator_to_file(g, path, NULL); g_object_unref(g);
}
static void dump_object(JsonObject *o, const char *path)
{ JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_set_object(n, o); dump(n, path); json_node_free(n); }
static guint parse_decks(const char *text, guint *decks)
{
    gchar **parts = g_strsplit(text, ",", 32); guint n = 0;
    for (guint i = 0; parts[i]; ++i) if (*parts[i]) decks[n++] = (guint)g_ascii_strtoull(parts[i], NULL, 10);
    g_strfreev(parts); return n;
}
static int fail(const char *step, GError *e)
{ printf("%s FAIL: %s\n", step, e ? e->message : "(boş, hata yok)"); return 1; }

int main(int argc, char **argv)
{
    GError *e = NULL;
    /* occ weekly.json now_us decks prefix -> prefix-event.json prefix-config.json */
    if (argc >= 6 && !strcmp(argv[1], "occ")) {
        JsonNode *wp = load(argv[2]); gint64 now = g_ascii_strtoll(argv[3], NULL, 10);
        RadioSWeeklyProgrammeOccurrence *o = radios_weekly_programme_next(json_node_get_object(wp), now, &e);
        if (!o) return fail("OCC", e);
        RadioSAutomationBinding b; memset(&b, 0, sizeof b);
        b.count = parse_decks(argv[4], b.decks); b.source = b.decks[0]; b.autodj_target = b.count > 1 ? (gint)b.decks[1] : -1;
        b.scope.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER;
        JsonNode *routed = radios_automation_scope_route_event(&b, json_node_get_object(o->event), &e);
        if (!routed) return fail("ROUTE", e);
        JsonNode *cfg = radios_automation_calendar_prepare_config(json_node_get_object(routed), NULL, &e);
        if (!cfg) return fail("PREPARE_CONFIG", e);
        gchar *a = g_strconcat(argv[5], "-event.json", NULL), *c = g_strconcat(argv[5], "-config.json", NULL);
        dump(routed, a); dump(cfg, c);
        printf("OCC ok active=%d title=%s start=%" G_GINT64_FORMAT " end=%" G_GINT64_FORMAT " key=%s\n", o->active,
               json_object_get_string_member(json_node_get_object(o->event), "title"), o->start_utc_us, o->end_utc_us, o->occurrence_key);
        return 0;
    }
    /* plan event.json prepared.json station at_us semi seed out.json */
    if (argc >= 9 && !strcmp(argv[1], "plan")) {
        JsonNode *ev = load(argv[2]), *pr = load(argv[3]); gint64 at = g_ascii_strtoll(argv[5], NULL, 10);
        JsonNode *sel = radios_automation_calendar_prepared_programme_at_mode(json_node_get_object(ev), json_node_get_object(pr),
            argv[4], argv[7], 200, at, atoi(argv[6]) != 0, &e);
        if (!sel) { printf("PLAN %s: %s\n", e && e->code == G_IO_ERROR_WOULD_BLOCK ? "WOULD_BLOCK" : "FAIL", e ? e->message : "?"); return 1; }
        dump(sel, argv[8]); printf("PLAN ok\n"); return 0;
    }
    /* merge result.json selection.json out.json */
    if (argc >= 5 && !strcmp(argv[1], "merge")) {
        JsonNode *res = load(argv[2]), *sel = load(argv[3]);
        JsonNode *m = radios_automation_calendar_merge_selection(json_node_get_object(res), json_node_get_object(sel), &e);
        if (!m) return fail("MERGE", e);
        dump(m, argv[4]); printf("MERGE ok\n"); return 0;
    }
    /* native prepared.json now_us single_player trim decks database_file out.json */
    if (argc >= 9 && !strcmp(argv[1], "native")) {
        JsonNode *pr = load(argv[2]);
        RadioSPlayoutRequest rq = {.station_id = json_object_get_string_member(json_node_get_object(pr), "station_id"),
            .database_file = argv[7], .now_utc_us = g_ascii_strtoll(argv[3], NULL, 10), .mode = RADIOS_AUDIO_RUN_EOF,
            .live = TRUE, .allow_single_routed = TRUE, .single_player = atoi(argv[4]) != 0, .trim_calendar_end = atoi(argv[5]) != 0};
        RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(pr), &rq, &e);
        if (!plan) return fail("NATIVE", e);
        JsonObject *out = json_object_new(); JsonArray *items = json_array_new();
        for (guint i = 0; i < plan->entries->len; ++i) {
            RadioSPlayoutEntry *en = g_ptr_array_index(plan->entries, i); RadioSAudioRunItem *r = &en->run;
            JsonObject *it = json_object_new();
            json_object_set_string_member(it, "title", en->title ? en->title : "");
            json_object_set_string_member(it, "kind", en->kind ? en->kind : "");
            json_object_set_string_member(it, "source", en->source_name ? en->source_name : "");
            json_object_set_int_member(it, "planned_utc_us", en->planned_utc_us);
            json_object_set_int_member(it, "mode", r->mode);
            json_object_set_double_member(it, "overlap_s", r->overlap_frames / 48000.0);
            json_object_set_boolean_member(it, "beat_sync", r->beat_sync);
            json_object_set_int_member(it, "effect", r->effect);
            json_object_set_double_member(it, "tempo", r->tempo_ratio);
            json_object_set_double_member(it, "cue_s", (r->media.cue_out_frame - r->media.cue_in_frame) / 48000.0);
            json_object_set_boolean_member(it, "adjunct", r->adjunct.enabled);
            if (r->adjunct.enabled) {
                json_object_set_int_member(it, "adjunct_player", r->adjunct.player);
                json_object_set_double_member(it, "adjunct_at_s", r->adjunct.start_output_frame / 48000.0);
                json_object_set_double_member(it, "adjunct_duck_db", r->adjunct.duck_db);
                json_object_set_string_member(it, "adjunct_title", r->adjunct.title ? r->adjunct.title : "");
            }
            JsonArray *rd = json_array_new();
            for (guint d = 0; d < en->routing_count; ++d) json_array_add_int_element(rd, en->routing_decks[d]);
            json_object_set_array_member(it, "decks", rd);
            json_array_add_object_element(items, it);
        }
        json_object_set_array_member(out, "entries", items);
        json_object_set_int_member(out, "pool_count", plan->options.pool_count);
        if (plan->display) json_object_set_member(out, "display", json_node_copy(plan->display));
        dump_object(out, argv[8]);
        printf("NATIVE ok entries=%u\n", plan->entries->len);
        radios_playout_plan_free(plan); return 0;
    }
    fprintf(stderr, "kullanım: occ|plan|merge|native ...\n"); return 2;
}
