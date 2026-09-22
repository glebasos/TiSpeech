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
set(TISPEECH_SPAN_DLL "" CACHE FILEPATH "Path to an original TISPAN32.DLL")
set(TISPEECH_BASE_DLL "" CACHE FILEPATH "Path to an original TIBASE32.DLL")
option(TISPEECH_VERIFY_ORIGINAL "Enable development-only Unicorn differential tests" OFF)
set(TISPEECH_VERIFY_CASES "20000" CACHE STRING "Cases per original-DLL differential test")

# Extraction needs Python; a build without DLLs needs no Python at all.
if(TISPEECH_ENG_DLL OR TISPEECH_SPAN_DLL OR TISPEECH_BASE_DLL)
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
# Pitch smoothing (TIBASE32 0x1c00cd40) and the two reconstructed generator
# leaves (TIENG32 0x1c203010 and 0x1c203870). Same contract as the renderer:
# both compile with no proprietary data, because the caller hands the tables
# in at the C boundary. Only the extracted table library below is gated.
# ---------------------------------------------------------------------------
add_library(tispeech_smoothing STATIC src/smoothing.c)
target_link_libraries(tispeech_smoothing PUBLIC tispeech_frames)

add_library(tispeech_generator STATIC src/generator.c)
target_include_directories(tispeech_generator PUBLIC include)

# Both test binaries run without any DLL: their fixtures are synthetic. The
# English DLL only adds the real-inventory cases inside test_generator.c.
add_executable(test_smoothing tests/test_smoothing.c)
target_link_libraries(test_smoothing PRIVATE tispeech_smoothing)
add_test(NAME smoothing COMMAND test_smoothing)

# Linked below against tispeech_gen_eng when the English tables are available,
# which carries tispeech_generator with it; against the bare stage otherwise.
add_executable(test_generator tests/test_generator.c)
add_test(NAME generator COMMAND test_generator)

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

    # Generator tables: the 9x16 interpolation-rate table and the two 110-entry
    # phoneme definition tables. Same build-time extraction contract as the
    # rule tables above; nothing proprietary is committed.
    set(_gen_eng "${CMAKE_CURRENT_BINARY_DIR}/eng_gen_data.c")
    add_custom_command(
        OUTPUT "${_gen_eng}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_generator.py"
                "${TISPEECH_ENG_DLL}" "${_gen_eng}"
                --language eng --symbol sv_gen_data_eng
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_generator.py"
                "${TISPEECH_ENG_DLL}"
        COMMENT "Extracting English generator tables from ${TISPEECH_ENG_DLL}")
    add_library(tispeech_gen_eng STATIC "${_gen_eng}")
    target_link_libraries(tispeech_gen_eng PUBLIC tispeech_generator)

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

if(TARGET tispeech_gen_eng)
    target_link_libraries(test_generator PRIVATE tispeech_gen_eng)
    target_compile_definitions(test_generator PRIVATE TISPEECH_HAVE_ENG_GEN_DATA=1)
else()
    target_link_libraries(test_generator PRIVATE tispeech_generator)
endif()

if(TISPEECH_SPAN_DLL)
    set(_span "${CMAKE_CURRENT_BINARY_DIR}/span_lang_data.c")
    add_custom_command(
        OUTPUT "${_span}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_lang.py"
                "${TISPEECH_SPAN_DLL}" "${_span}"
                --language span --symbol sv_lang_data_span
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_lang.py"
                "${TISPEECH_SPAN_DLL}"
        COMMENT "Extracting Spanish rule tables from ${TISPEECH_SPAN_DLL}")
    add_library(tispeech_lang_span STATIC "${_span}")
    target_link_libraries(tispeech_lang_span PUBLIC tispeech_rules)
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
elseif(NOT TISPEECH_SPAN_DLL)
    target_link_libraries(tispeech_shared PRIVATE tispeech_rules)
endif()

if(TISPEECH_SPAN_DLL)
    target_link_libraries(tispeech_shared PRIVATE tispeech_lang_span)
    target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_SPAN=1)
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
    if(TISPEECH_SPAN_DLL)
        target_link_libraries(test_capi PRIVATE tispeech_lang_span)
        target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_SPAN=1)
    endif()
    add_test(NAME capi COMMAND test_capi)
endif()

# Development-only oracles. These separate shared libraries expose internal C
# functions to ctypes; the application never links an emulator or these targets.
if(TISPEECH_VERIFY_ORIGINAL)
    if(NOT (TISPEECH_ENG_DLL OR TISPEECH_SPAN_DLL OR TISPEECH_BASE_DLL))
        message(FATAL_ERROR "TISPEECH_VERIFY_ORIGINAL requires at least one original DLL")
    endif()
    if(NOT TISPEECH_VERIFY_CASES MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "TISPEECH_VERIFY_CASES must be a positive integer")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn, pefile"
        RESULT_VARIABLE _oracle_imports OUTPUT_QUIET ERROR_VARIABLE _oracle_error)
    if(NOT _oracle_imports EQUAL 0)
        message(FATAL_ERROR
            "Differential tests need unicorn and pefile in Python3_EXECUTABLE: ${_oracle_error}")
    endif()

    function(tispeech_oracle_library target)
        add_library(${target} SHARED ${ARGN})
        target_include_directories(${target} PRIVATE include)
        set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endfunction()

    function(tispeech_oracle_test name script target)
        add_test(NAME ${name} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/${script}"
            --library "$<TARGET_FILE:${target}>" ${ARGN})
        set_tests_properties(${name} PROPERTIES LABELS differential TIMEOUT 600)
    endfunction()

    if(TISPEECH_ENG_DLL)
        tispeech_oracle_library(verify_rules_eng src/ruleset.c "${_gen}")
        add_dependencies(verify_rules_eng tispeech_lang_eng)
        tispeech_oracle_test(original_rules_eng verify_ruleset.py verify_rules_eng
            --dll "${TISPEECH_ENG_DLL}" --language eng
            --words= --cases 0 --random-cases "${TISPEECH_VERIFY_CASES}")

        tispeech_oracle_library(verify_generator src/generator.c)
        tispeech_oracle_test(original_generator verify_generator.py verify_generator
            --dll "${TISPEECH_ENG_DLL}" --cases "${TISPEECH_VERIFY_CASES}")
    endif()
    if(TISPEECH_SPAN_DLL)
        tispeech_oracle_library(verify_rules_span src/ruleset.c "${_span}")
        add_dependencies(verify_rules_span tispeech_lang_span)
        tispeech_oracle_test(original_rules_span verify_ruleset.py verify_rules_span
            --dll "${TISPEECH_SPAN_DLL}" --language span
            --words= --cases 0 --random-cases "${TISPEECH_VERIFY_CASES}")
    endif()
    if(TISPEECH_BASE_DLL)
        tispeech_oracle_library(verify_frames src/dsp.c src/frames.c)
        tispeech_oracle_test(original_frames verify_frames.py verify_frames
            --dll "${TISPEECH_BASE_DLL}" --cases "${TISPEECH_VERIFY_CASES}" --check-extract)
        tispeech_oracle_test(original_dsp verify_dsp.py verify_frames
            --dll "${TISPEECH_BASE_DLL}" --cases "${TISPEECH_VERIFY_CASES}")

        tispeech_oracle_library(verify_smoothing src/smoothing.c)
        tispeech_oracle_test(original_smoothing verify_smoothing.py verify_smoothing
            --dll "${TISPEECH_BASE_DLL}" --cases "${TISPEECH_VERIFY_CASES}")
    endif()
endif()
