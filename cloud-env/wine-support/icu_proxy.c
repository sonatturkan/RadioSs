/* Test-environment-only icu.dll for Wine: Windows 10 1903+ ships the ICU C API with
 * unversioned exports in System32\icu.dll; Wine has none. This proxy exports the calls
 * RadioS uses, statically linked against ICU 78. Not shipped. */
#define U_STATIC_IMPLEMENTATION 1
#include <unicode/ucal.h>
#include <unicode/uenum.h>
int32_t rt_getWindowsTimeZoneID(const UChar *id, int32_t len, UChar *winid, int32_t cap, UErrorCode *st)
{ return ucal_getWindowsTimeZoneID(id, len, winid, cap, st); }
int32_t rt_getTimeZoneIDForWindowsID(const UChar *winid, int32_t len, const char *region, UChar *id, int32_t cap, UErrorCode *st)
{ return ucal_getTimeZoneIDForWindowsID(winid, len, region, id, cap, st); }
int32_t rt_getCanonicalTimeZoneID(const UChar *id, int32_t len, UChar *res, int32_t cap, UBool *sys, UErrorCode *st)
{ return ucal_getCanonicalTimeZoneID(id, len, res, cap, sys, st); }
UEnumeration *rt_openTimeZoneIDEnumeration(USystemTimeZoneType t, const char *region, const int32_t *off, UErrorCode *st)
{ return ucal_openTimeZoneIDEnumeration(t, region, off, st); }
const char *rt_uenum_next(UEnumeration *e, int32_t *len, UErrorCode *st) { return uenum_next(e, len, st); }
void rt_uenum_close(UEnumeration *e) { uenum_close(e); }
