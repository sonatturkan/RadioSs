/* Test-environment-only: keeps one hidden window alive in the Wine session so the
 * Wine desktop (and the display device registry keys patched by test-env.reg) are not
 * re-created for every test process. Not shipped. */
#include <windows.h>
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
  (void)prev; (void)cmd; (void)show;
  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc = DefWindowProcW; wc.hInstance = inst; wc.lpszClassName = L"RadioSWineKeeper";
  RegisterClassW(&wc);
  CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"keeper", WS_POPUP, -32000, -32000, 1, 1, NULL, NULL, inst, NULL);
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  return 0;
}
