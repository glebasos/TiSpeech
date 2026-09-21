cmake_minimum_required(VERSION 3.20)
project(tispeech_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
enable_testing()

# ---------------------------------------------------------------------------
# Language data.
#
# The letter-to-sound rules belong to SoftVoice, Inc. They are NOT in this
# repository and must never be committed. Point TISPEECH_ENG_DLL at a copy of
# TIENG32.DLL you already own and the table is extracted into the build tree at
# build time. The DLL is parsed as a file; it is never loaded or executed.
#
#   cmake -B build -DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL
# ---------------------------------------------------------------------------
set(TISPEECH_ENG_DLL "" CACHE FILEPATH "Path to an original TIENG32.DLL")
set(TISPEECH_BASE_DLL "" CACHE FILEPATH "Path to an original TIBASE32.DLL")

# Both extractors are Python; a build with neither DLL needs no Python at all,
# so this stays gated rather than sitting unconditionally at the top.
if(TISPEECH_ENG_DLL OR TISPEECH_BASE_DLL)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

add_library(tispeech_rules STATIC src/ruleset.c)
target_include_directories(tispeech_rules PUBLIC include)

# ---------------------------------------------------------------------------
# Frame-to-PCM renderer and fixed-point sample kernel. Neither requires
# proprietary tables to compile; callers supply the data at the C boundary.
# ---------------------------------------------------------------------------
add_library(tispeech_dsp STATIC src/dsp.c)
target_include_directories(tispeech_dsp PUBLIC include)

add_library(tispeech_frames STATIC src/frames.c)
target_link_libraries(tispeech_frames PUBLIC tispeech_dsp)

# ---------------------------------------------------------------------------
# Base coefficient tables.
#
# Same rule as the language data: the resonator, amplitude, glottal, noise and
# output-curve tables belong to SoftVoice and are NOT in this repository. Point
# TISPEECH_BASE_DLL at a copy of TIBASE32.DLL you already own and they are
# extracted into the build tree at build time. The DLL is parsed as a file; it
# is never loaded or executed.
#
#   cmake -B build -DTISPEECH_BASE_DLL=/path/to/TIBASE32.DLL
#
# tispeech_frames above compiles without any of this; only the data library and
# the frame tests are gated, exactly as test_ruleset is gated on the English DLL.
# ---------------------------------------------------------------------------
if(TISPEECH_BASE_DLL)
    set(_base "${CMAKE_CURRENT_BINARY_DIR}/base_data.c")
    add_custom_command(
        OUTPUT "${_base}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_base.py"
                "${TISPEECH_BASE_DLL}" "${_base}"
                --symbol sv_base_tables
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_base.py"
                "${TISPEECH_BASE_DLL}"
        COMMENT "Extracting base coefficient tables from ${TISPEECH_BASE_DLL}")
    add_library(tispeech_base_data STATIC "${_base}")
    target_link_libraries(tispeech_base_data PUBLIC tispeech_frames)

    add_executable(test_frames tests/test_frames.c)
    target_link_libraries(test_frames PRIVATE tispeech_base_data)
    add_test(NAME frames COMMAND test_frames)
else()
    message(STATUS
        "TISPEECH_BASE_DLL not set — building without base coefficient data. "
        "The frame tests need it; re-run with "
        "-DTISPEECH_BASE_DLL=/path/to/TIBASE32.DLL")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsp.c")
    add_executable(test_dsp tests/test_dsp.c)
    target_link_libraries(test_dsp PRIVATE tispeech_dsp)
    add_test(NAME dsp COMMAND test_dsp)
endif()

if(TISPEECH_ENG_DLL)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/eng_lang_data.c")
    add_custom_command(
        OUTPUT "${_gen}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_lang.py"
                "${TISPEECH_ENG_DLL}" "${_gen}"
                --symbol sv_lang_data_eng
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_lang.py"
                "${TISPEECH_ENG_DLL}"
        COMMENT "Extracting English rule tables from ${TISPEECH_ENG_DLL}")
    add_library(tispeech_lang_eng STATIC "${_gen}")
    target_link_libraries(tispeech_lang_eng PUBLIC tispeech_rules)

    add_executable(svphon tools/svphon.c)
    target_link_libraries(svphon PRIVATE tispeech_lang_eng)

    enable_testing()
    add_executable(test_ruleset tests/test_ruleset.c)
    target_link_libraries(test_ruleset PRIVATE tispeech_lang_eng)
    add_test(NAME ruleset COMMAND test_ruleset)
else()
    message(STATUS
        "TISPEECH_ENG_DLL not set — building without English rule data. "
        "The English CLI and rule tests need language data; re-run with "
        "-DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL")
endif()

# ---------------------------------------------------------------------------
# Shared library for managed (P/Invoke) callers.
#
# Exposes include/tispeech/capi.h — OUR ABI, not SoftVoice's. It always builds,
# with or without language data: tispeech_capabilities() reports what the build
# can actually do, so the .NET side gates on real capability instead of on the
# host operating system. Symbols are hidden unless marked TISPEECH_API.
# ---------------------------------------------------------------------------
add_library(tispeech_shared SHARED src/capi.c)
set_target_properties(tispeech_shared PROPERTIES
    OUTPUT_NAME tispeech
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(tispeech_shared PUBLIC include)

if(TISPEECH_ENG_DLL)
    target_link_libraries(tispeech_shared PRIVATE tispeech_lang_eng)
    target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_ENG=1)
else()
    target_link_libraries(tispeech_shared PRIVATE tispeech_rules)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capi.c")
    enable_testing()
    add_executable(test_capi tests/test_capi.c)
    target_link_libraries(test_capi PRIVATE tispeech_shared)
    target_include_directories(test_capi PRIVATE include)
    if(TISPEECH_ENG_DLL)
        target_link_libraries(test_capi PRIVATE tispeech_lang_eng)
        target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_ENG=1)
    endif()
    add_test(NAME capi COMMAND test_capi)
endif()
