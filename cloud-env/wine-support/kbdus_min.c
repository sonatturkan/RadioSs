/* Test-environment-only US keyboard layout DLL for Wine (Wine 9 ships no kbd*.dll and
 * GDK warns when it cannot load the layout). Implements the documented KBDTABLES format
 * for the US layout (letters, digits, punctuation, shift/ctrl states). Not shipped. */
#include <windows.h>
#define KBDSHIFT 1
#define KBDCTRL 2
#define KBDALT 4
typedef struct { BYTE Vk; BYTE ModBits; } VK_TO_BIT;
typedef struct { VK_TO_BIT *pVkToBit; WORD wMaxModBits; BYTE ModNumber[]; } MODIFIERS;
typedef struct { BYTE VirtualKey; BYTE Attributes; WCHAR wch[3]; } VK_TO_WCHARS3;
typedef struct { BYTE VirtualKey; BYTE Attributes; WCHAR wch[2]; } VK_TO_WCHARS2;
typedef struct { void *pVkToWchars; BYTE nModifications; BYTE cbSize; } VK_TO_WCHAR_TABLE;
typedef struct { BYTE vsc; WCHAR *pwsz; } VSC_LPWSTR;
typedef struct { BYTE Vsc; USHORT Vk; } VSC_VK;
typedef struct {
  MODIFIERS *pCharModifiers; VK_TO_WCHAR_TABLE *pVkToWcharTable; void *pDeadKey;
  VSC_LPWSTR *pKeyNames; VSC_LPWSTR *pKeyNamesExt; WCHAR **pKeyNamesDead;
  USHORT *pusVSCtoVK; BYTE bMaxVSCtoVK; VSC_VK *pVSCtoVK_E0; VSC_VK *pVSCtoVK_E1;
  DWORD fLocaleFlags; BYTE nLgMax; BYTE cbLgEntry; void *pLigature; DWORD dwType; DWORD dwSubType;
} KBDTABLES;
#define CAPLOK 1
#define WCH_NONE 0xF000
static VK_TO_BIT vk_to_bits[] = { { VK_SHIFT, KBDSHIFT }, { VK_CONTROL, KBDCTRL }, { VK_MENU, KBDALT }, { 0, 0 } };
static MODIFIERS char_modifiers = { vk_to_bits, 3, { 0, 1, 2, 0x0F } };
#define L2(vk, a, b) { vk, CAPLOK, { a, b } }
#define P2(vk, a, b) { vk, 0, { a, b } }
static VK_TO_WCHARS2 vk2[] = {
  P2('1', L'1', L'!'), P2('2', L'2', L'@'), P2('3', L'3', L'#'), P2('4', L'4', L'$'), P2('5', L'5', L'%'),
  P2('7', L'7', L'&'), P2('8', L'8', L'*'), P2('9', L'9', L'('), P2('0', L'0', L')'),
  P2(VK_OEM_1, L';', L':'), P2(VK_OEM_7, L'\'', L'"'), P2(VK_OEM_3, L'`', L'~'), P2(VK_OEM_COMMA, L',', L'<'),
  P2(VK_OEM_PERIOD, L'.', L'>'), P2(VK_OEM_2, L'/', L'?'), P2(VK_OEM_PLUS, L'=', L'+'),
  L2('A', L'a', L'A'), L2('B', L'b', L'B'), L2('C', L'c', L'C'), L2('D', L'd', L'D'), L2('E', L'e', L'E'),
  L2('F', L'f', L'F'), L2('G', L'g', L'G'), L2('H', L'h', L'H'), L2('I', L'i', L'I'), L2('J', L'j', L'J'),
  L2('K', L'k', L'K'), L2('L', L'l', L'L'), L2('M', L'm', L'M'), L2('N', L'n', L'N'), L2('O', L'o', L'O'),
  L2('P', L'p', L'P'), L2('Q', L'q', L'Q'), L2('R', L'r', L'R'), L2('S', L's', L'S'), L2('T', L't', L'T'),
  L2('U', L'u', L'U'), L2('V', L'v', L'V'), L2('W', L'w', L'W'), L2('X', L'x', L'X'), L2('Y', L'y', L'Y'),
  L2('Z', L'z', L'Z'), P2(VK_TAB, L'\t', L'\t'), P2(VK_ADD, L'+', L'+'), P2(VK_DECIMAL, L'.', L'.'),
  P2(VK_DIVIDE, L'/', L'/'), P2(VK_MULTIPLY, L'*', L'*'), P2(VK_SUBTRACT, L'-', L'-'),
  { 0, 0, { 0, 0 } } };
static VK_TO_WCHARS3 vk3[] = {
  { VK_OEM_4, 0, { L'[', L'{', 0x001b } }, { VK_OEM_6, 0, { L']', L'}', 0x001d } }, { VK_OEM_5, 0, { L'\\', L'|', 0x001c } },
  { VK_BACK, 0, { L'\b', L'\b', 0x007f } }, { VK_ESCAPE, 0, { 0x001b, 0x001b, 0x001b } }, { VK_RETURN, 0, { L'\r', L'\r', L'\n' } },
  { VK_SPACE, 0, { L' ', L' ', L' ' } }, { VK_CANCEL, 0, { 0x0003, 0x0003, 0x0003 } },
  { '6', 0, { L'6', L'^', 0x001e } }, { VK_OEM_MINUS, 0, { L'-', L'_', 0x001f } },
  { 0, 0, { 0, 0, 0 } } };
static VK_TO_WCHARS2 vknum[] = {
  P2(VK_NUMPAD0, L'0', L'0'), P2(VK_NUMPAD1, L'1', L'1'), P2(VK_NUMPAD2, L'2', L'2'), P2(VK_NUMPAD3, L'3', L'3'),
  P2(VK_NUMPAD4, L'4', L'4'), P2(VK_NUMPAD5, L'5', L'5'), P2(VK_NUMPAD6, L'6', L'6'), P2(VK_NUMPAD7, L'7', L'7'),
  P2(VK_NUMPAD8, L'8', L'8'), P2(VK_NUMPAD9, L'9', L'9'), { 0, 0, { 0, 0 } } };
static VK_TO_WCHAR_TABLE vk_tables[] = {
  { vk3, 3, sizeof(VK_TO_WCHARS3) }, { vk2, 2, sizeof(VK_TO_WCHARS2) }, { vknum, 2, sizeof(VK_TO_WCHARS2) }, { NULL, 0, 0 } };
static USHORT vsc_to_vk[] = {
  0xff, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL, 'A', 'S',
  'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5, 'Z', 'X', 'C', 'V',
  'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_RSHIFT, VK_MULTIPLY, VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5,
  VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_NUMLOCK, VK_SCROLL, VK_HOME, VK_UP, VK_PRIOR, VK_SUBTRACT, VK_LEFT, VK_CLEAR, VK_RIGHT, VK_ADD, VK_END,
  VK_DOWN, VK_NEXT, VK_INSERT, VK_DELETE, VK_SNAPSHOT, 0xff, VK_OEM_102, VK_F11, VK_F12 };
static VSC_VK e0[] = { { 0x1c, VK_RETURN }, { 0x1d, VK_RCONTROL }, { 0x35, VK_DIVIDE }, { 0x38, VK_RMENU }, { 0x47, VK_HOME }, { 0x48, VK_UP },
  { 0x49, VK_PRIOR }, { 0x4b, VK_LEFT }, { 0x4d, VK_RIGHT }, { 0x4f, VK_END }, { 0x50, VK_DOWN }, { 0x51, VK_NEXT }, { 0x52, VK_INSERT },
  { 0x53, VK_DELETE }, { 0x5b, VK_LWIN }, { 0x5c, VK_RWIN }, { 0x5d, VK_APPS }, { 0, 0 } };
static VSC_VK e1[] = { { 0x1d, VK_PAUSE }, { 0, 0 } };
static VSC_LPWSTR key_names[] = { { 0x01, L"Esc" }, { 0x0e, L"Backspace" }, { 0x0f, L"Tab" }, { 0x1c, L"Enter" }, { 0x39, L"Space" }, { 0, NULL } };
static KBDTABLES tables = { &char_modifiers, vk_tables, NULL, key_names, NULL, NULL, vsc_to_vk, (BYTE)(sizeof(vsc_to_vk) / sizeof(vsc_to_vk[0])), e0, e1, 0, 0, 0, NULL, 4, 0 };
__declspec(dllexport) KBDTABLES *WINAPI KbdLayerDescriptor(void) { return &tables; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
