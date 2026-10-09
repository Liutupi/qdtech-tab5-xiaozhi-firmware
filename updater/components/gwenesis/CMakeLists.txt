# Gwenesis Mega Drive / Genesis core (AGPL-3.0, see LICENSE), from retro-go @ 4ced120.
# Tab5 changes: the M68K opcode/cycle tables are built at start-up (BUILD_TABLES) into
# allocated memory instead of 300 KB of constant tables in flash; see gwenesis_tab5.c.
idf_component_register(
    SRC_DIRS "." "src/bus" "src/cpus/M68K" "src/cpus/Z80" "src/io" "src/savestate" "src/sound" "src/vdp"
    INCLUDE_DIRS "." "src/bus" "src/cpus/M68K" "src/cpus/Z80" "src/io" "src/savestate" "src/sound" "src/vdp"
    PRIV_REQUIRES heap log)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_TABLES)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-incompatible-pointer-types -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-unused-function -Wno-missing-field-initializers -Wno-sign-compare
    -Wno-implicit-fallthrough -Wno-parentheses -Wno-maybe-uninitialized -Wno-char-subscripts)
