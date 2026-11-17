cmake_minimum_required(VERSION 3.20)
project(navtex)

# -----------------------------------------------------------------------
# Station and country tables, regenerated on every build from
# tools/navtex_stations.wiki (Wikipedia) and tools/YAND_STATIONS.TXT (YaND).
# The generators rewrite src/navtex_stations.h and src/navtex_countries.h only
# when their content changes, so an unchanged table recompiles nothing.  The
# generated files stay committed: the station diff shows up in review, and
# the test/ident Python tools read src/navtex_stations.h directly.
# Anything that includes either header depends on navtex_tables.
# -----------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NAVTEX_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set(NAVTEX_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_custom_target(navtex_tables ALL
    COMMAND "${Python3_EXECUTABLE}" -I "${NAVTEX_TOOLS}/gen_navtex_stations.py"
            "${NAVTEX_TOOLS}/navtex_stations.wiki" "${NAVTEX_TOOLS}/YAND_STATIONS.TXT"
            -o "${NAVTEX_SRC}/navtex_stations.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${NAVTEX_TOOLS}/gen_navtex_countries.py"
            "${NAVTEX_SRC}/navtex_stations.h" -o "${NAVTEX_SRC}/navtex_countries.h"
    COMMENT "Regenerating NAVTEX station and country tables"
    VERBATIM)

add_subdirectory(src)

# Protocol conformance test for src/pcm_v4.hpp:
#   cmake --build build --target pcmv4_conformance
#   ctest --test-dir build --output-on-failure
enable_testing()
add_subdirectory(test)
