# alac: vendored David Hammerton ALAC decoder (pure C, verbatim — see NOTICE) plus
# a pure fmtp->config mapper written for this project. No ESP-IDF dependencies.
# Buffers are small at the RAOP default frameLength=352 (a few KB), so the
# decoder's internal malloc lands in internal RAM — do NOT PSRAM-cap.
idf_component_register(
    SRCS "src/alac.c" "src/alac_config.c"
    INCLUDE_DIRS "include"
    REQUIRES ""
)

# The vendored decoder is upstream code kept byte-for-byte; silence its benign
# warnings for THIS translation unit only rather than editing the vendored source.
set_source_files_properties("src/alac.c"
    PROPERTIES COMPILE_FLAGS
    "-Wno-unused-but-set-variable -Wno-unused-variable -Wno-sign-compare")
