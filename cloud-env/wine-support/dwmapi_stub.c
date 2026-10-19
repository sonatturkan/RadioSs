/* Test-environment-only dwmapi.dll for Wine: Wine's builtin returns E_NOTIMPL for
 * DwmEnableBlurBehindWindow, which makes GDK emit a warning that G_DEBUG=fatal-warnings
 * turns into an abort. Real Windows DWM succeeds; this stub models that. Not shipped. */
#include <windows.h>
typedef struct { DWORD dwFlags; BOOL fEnable; HRGN hRgnBlur; BOOL fTransitionOnMaximized; } RT_DWM_BLURBEHIND;
typedef struct { int l, r, t, b; } RT_MARGINS;
__declspec(dllexport) HRESULT WINAPI DwmIsCompositionEnabled(BOOL *enabled) { if (!enabled) return E_INVALIDARG; *enabled = TRUE; return S_OK; }
__declspec(dllexport) HRESULT WINAPI DwmEnableBlurBehindWindow(HWND hwnd, const RT_DWM_BLURBEHIND *bb) { (void)hwnd; (void)bb; return S_OK; }
__declspec(dllexport) HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hwnd, const RT_MARGINS *m) { (void)hwnd; (void)m; return S_OK; }
__declspec(dllexport) HRESULT WINAPI DwmFlush(void) { return S_OK; }
__declspec(dllexport) HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD attr, LPCVOID data, DWORD size) { (void)hwnd; (void)attr; (void)data; (void)size; return S_OK; }
__declspec(dllexport) HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD attr, PVOID data, DWORD size) { (void)hwnd; (void)attr; if (data && size) memset(data, 0, size); return E_INVALIDARG; }
__declspec(dllexport) BOOL WINAPI DwmDefWindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, LRESULT *res) { (void)hwnd; (void)msg; (void)w; (void)l; if (res) *res = 0; return FALSE; }
__declspec(dllexport) HRESULT WINAPI DwmGetColorizationColor(DWORD *c, BOOL *o) { if (c) *c = 0xff2b579a; if (o) *o = TRUE; return S_OK; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
