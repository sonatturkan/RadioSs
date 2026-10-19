#ifndef RADIOS_IMPORT_PROFILES_UI_H
#define RADIOS_IMPORT_PROFILES_UI_H
#include "radios/views.h"
G_BEGIN_DECLS
/* Ayarlar › Import › "Rekordbox içe aktarma profilleri": the station's import
 * profiles (GET/POST /v1/import-profiles, POST /v1/import-profiles/<id>) with
 * their library key, cue policy, source → local path rules and allowed media
 * roots, and the profile-based rekordbox XML import (POST
 * /v1/imports/rekordbox/preview, then /apply with the previewed hash).
 * Widget names: import-profiles (section), import-profiles-list,
 * import-profiles-add, import-profiles-edit, import-profiles-xml,
 * import-profiles-status; dialogs import-profile-dialog and
 * import-profile-xml-dialog. */
GtkWidget *radios_import_profiles_section_new(RadioSViewContext *ctx);
G_END_DECLS
#endif
