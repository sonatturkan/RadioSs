# RadioS - refuse to package anything but a Release build (cmake -DRADIOS_BUILD_TYPE=<cfg> -P ...).
# Packages ship optimised, CodeView-symbolised Release binaries only (build-system 7.2, D11).
if(NOT RADIOS_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Paketler yalnız Release derlemesinden üretilir (bulunan: '${RADIOS_BUILD_TYPE}'); "
                        "'cmake --preset windows-release' kullanın / packages are built from a Release build only "
                        "(found '${RADIOS_BUILD_TYPE}'); use 'cmake --preset windows-release'.")
endif()
