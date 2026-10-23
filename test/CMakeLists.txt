# -----------------------------------------------------------------------
# pcmv4_conformance: decode a packet stream the SERVER's encoder produced
# and hash the samples.
#
# The version 4 predictor is backward adaptive — the two ends derive their
# filter taps independently and never exchange a coefficient — so an
# arithmetic difference between src/pcm_v4.hpp and the server's Go encoder
# returns plausible noise rather than an error.  Comparing the decoded
# samples is the only thing that catches it.
#
# Needs nothing but a C++ compiler: pcm_v4.hpp is header-only.
#
#   cmake --build build --target pcmv4_conformance
#   ctest --test-dir build --output-on-failure
# -----------------------------------------------------------------------
add_executable(pcmv4_conformance pcmv4_conformance.cpp)
target_compile_features(pcmv4_conformance PRIVATE cxx_std_11)

add_test(NAME pcmv4_conformance
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run.sh"
                 "$<TARGET_FILE:pcmv4_conformance>"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------
# ident_facts_test: the plain-English facts shipped with every station
# identification (src/navtex_ident_facts.h).
#
# Header-only and pure, so this needs nothing but a C++ compiler.  Builds the
# queries and results by hand, so it pins the wording rules independently of
# what the identification algorithm decides.
#
#   cmake --build build --target ident_facts_test
#   ctest --test-dir build -R ident_facts --output-on-failure
# -----------------------------------------------------------------------
add_executable(ident_facts_test ident_facts_test.cpp)
target_compile_features(ident_facts_test PRIVATE cxx_std_17)
target_include_directories(ident_facts_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_test(NAME ident_facts COMMAND ident_facts_test)

# -----------------------------------------------------------------------
# ident_regression: the transmitter identification ensemble (src/navtex_ident.h).
#
# Hand-written golden cases (Niton heard at Dalgety Bay in its slot, no
# receiver position, an unreadable B1, an off-slot vital warning, a
# Mediterranean letter collision, an unlisted 490 kHz service, a receiver
# whose lat/lon are swapped) plus a replay of a committed 4000-row sample of
# the held-out synthetic set, asserting wrong <= 1% at level "high", an
# accuracy floor and that replaying reproduces the same results.
#
# Header-only, needs nothing but a C++11 compiler.  The fixture is rebuilt by
# test/ident/make_fixture.py; the full evaluation harness is test/ident/.
#
#   cmake --build build --target ident_regression
#   ctest --test-dir build -R ident_regression --output-on-failure
# -----------------------------------------------------------------------
add_executable(ident_regression ident_regression.cpp)
target_compile_features(ident_regression PRIVATE cxx_std_11)
set_target_properties(ident_regression PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME ident_regression
         COMMAND ident_regression "${CMAKE_CURRENT_SOURCE_DIR}/testdata/ident_fixture.csv"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
