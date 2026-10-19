/* RadioS medya içe aktarma yardımcısı: bir ses dosyasını uygulamanın kendi C analizörüyle
 * özel kopyaya alıp çözümler; backend'e gidecek import gövdesini stdout'a JSON yazar.
 * Kullanım: usb_analiz <proje_kökü> <config.ini> <dosya> */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include "radios/config.h"
#include "radios/media.h"

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "kullanım: %s kök ini dosya\n", argv[0]); return 2; }
    RadioSConfig config = {0}; GError *error = NULL;
    if (!radios_config_load(argv[1], argv[2], &config, &error)) { fprintf(stderr, "config: %s\n", error->message); return 3; }
    RadioSMediaAnalysis *a = radios_media_stage_analyze(&config, argv[3], NULL, NULL, &error);
    if (!a) { fprintf(stderr, "analiz: %s\n", error ? error->message : "bilinmeyen"); return 4; }
    JsonObject *o = json_object_new();
    gchar *base = g_path_get_basename(a->path);
    json_object_set_string_member(o, "import_path", base);
    json_object_set_string_member(o, "source_name", a->source_name ? a->source_name : "");
    json_object_set_string_member(o, "title", a->title ? a->title : "");
    json_object_set_string_member(o, "artist", a->artist ? a->artist : "");
    json_object_set_string_member(o, "album", a->album ? a->album : "");
    json_object_set_string_member(o, "genre", a->genre ? a->genre : "");
    json_object_set_string_member(o, "sha256", a->sha256 ? a->sha256 : "");
    json_object_set_member(o, "analysis", radios_media_analysis_to_json(a));
    JsonNode *root = json_node_new(JSON_NODE_OBJECT); json_node_take_object(root, o);
    JsonGenerator *g = json_generator_new(); json_generator_set_root(g, root);
    gchar *text = json_generator_to_data(g, NULL); puts(text);
    g_free(text); g_object_unref(g); json_node_free(root); g_free(base);
    radios_media_analysis_free(a); radios_config_clear(&config);
    return 0;
}
