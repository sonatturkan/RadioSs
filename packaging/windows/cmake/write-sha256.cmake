# RadioS - write "<sha256>  <file name>\n" next to RADIOS_FILE (cmake -DRADIOS_FILE=<path> -P ...).
if(NOT RADIOS_FILE OR NOT EXISTS "${RADIOS_FILE}")
    message(FATAL_ERROR "write-sha256.cmake: dosya yok / file missing: '${RADIOS_FILE}'")
endif()
file(SHA256 "${RADIOS_FILE}" digest)
cmake_path(GET RADIOS_FILE FILENAME name)
file(WRITE "${RADIOS_FILE}.sha256.tmp" "${digest}  ${name}\n")
file(RENAME "${RADIOS_FILE}.sha256.tmp" "${RADIOS_FILE}.sha256")
message(STATUS "${name}: sha256 ${digest}")
