#ifndef RADIOS_STATION_TIME_H
#define RADIOS_STATION_TIME_H
#include "radios/data.h"
#include "radios/platform.h"

/* The client interns its timezone identifier; it remains valid after unref. */
static inline const gchar *radios_station_timezone(RadioSViewContext *ctx)
{
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    const gchar *zone = client ? radios_data_client_station_timezone(client) : "UTC";
    if (client) radios_data_client_unref(client);
    return zone;
}
static inline GDateTime *radios_station_now(RadioSViewContext *ctx)
{
    /* GLib on Windows cannot load IANA identifiers; radios_time_zone_new()
     * maps them (registry rules or the bundled tzdata).  An unknown id keeps
     * the documented UTC fallback. */
    GTimeZone *zone = radios_time_zone_new(radios_station_timezone(ctx));
    if (!zone) zone = g_time_zone_new_utc();
    GDateTime *now = g_date_time_new_now(zone); g_time_zone_unref(zone); return now;
}
#endif
