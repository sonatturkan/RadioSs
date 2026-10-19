/* Linked into every RadioS executable (cmake/RadioSWindowsExe.cmake): the CRT's default
 * file translation mode is binary, so fopen()/open() without "b"/O_BINARY never turn
 * "\n" into "\r\n" (journals, JSON, INI and hashed files must keep their bytes).
 * MinGW-w64's binmode.o is an empty stub with UCRT; the CRT start-up code copies this
 * variable into the UCRT before main(). stdin/stdout/stderr keep text mode.
 * radios_platform_init() sets the same mode again with _set_fmode(). */
#include <fcntl.h>
#include <stdlib.h>

#undef _fmode
int _fmode = _O_BINARY;
