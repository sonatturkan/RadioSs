# Cloud verification toolchain (NOT part of the repository): builds RadioS for Windows
# with the exact MSYS2 UCRT64 GCC 16 toolchain executed under Wine, and runs test
# executables under Wine via the "wrun" emulator. The native Windows build needs no
# toolchain file.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_C_COMPILER /opt/radios-port/bin/x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER /opt/radios-port/bin/x86_64-w64-mingw32-windres)
set(CMAKE_AR /opt/radios-port/bin/x86_64-w64-mingw32-ar CACHE FILEPATH "")
set(CMAKE_RANLIB /opt/radios-port/bin/x86_64-w64-mingw32-ranlib CACHE FILEPATH "")
set(CMAKE_OBJDUMP /opt/radios-port/bin/x86_64-w64-mingw32-objdump CACHE FILEPATH "")
set(PKG_CONFIG_EXECUTABLE /opt/radios-port/bin/x86_64-w64-mingw32-pkg-config CACHE FILEPATH "")
set(CMAKE_CROSSCOMPILING_EMULATOR /opt/radios-port/bin/wrun)
set(CMAKE_FIND_ROOT_PATH /opt/msys2/ucrt64)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Location of the MSYS2 UCRT64 prefix as seen by the build host (DLL closure, data files)
set(RADIOS_MSYS2_PREFIX /opt/msys2/ucrt64 CACHE PATH "MSYS2 UCRT64 prefix")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND /opt/radios-port/bin/x86_64-w64-mingw32-objdump)
