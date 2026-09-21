cmake_minimum_required(VERSION 3.20)
project(tispeech_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Language data.
#
# The letter-to-sound rules belong to SoftVoice, Inc. They are NOT in this
# repository and must never be committed. Point TISPEECH_ENG_DLL at a copy of
# TIENG32.DLL you already own and the table is extracted into the build tree at
# configure time. The DLL is parsed as a file; it is never loaded or executed.
#
#   cmake -B build -DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL
# ---------------------------------------------------------------------------
set(TISPEECH_ENG_DLL "" CACHE FILEPATH "Path to an original TIENG32.DLL")

add_library(tispeech_rules STATIC src/ruleset.c)
target_include_directories(tispeech_rules PUBLIC include)

# ---------------------------------------------------------------------------
# DSP sample kernel (reconstruction of TIBASE32 0x1C004120..0x1C004492).
# Owned separately; the guard keeps the tree building before it lands. It needs
# no language data, so it is deliberately outside the TISPEECH_ENG_DLL branch.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp.c")
    add_library(tispeech_dsp STATIC src/dsp.c)
    target_include_directories(tispeech_dsp PUBLIC include)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsp.c")
        enable_testing()
        add_executable(test_dsp tests/test_dsp.c)
        target_link_libraries(test_dsp PRIVATE tispeech_dsp)
        add_test(NAME dsp COMMAND test_dsp)
    endif()
endif()

if(TISPEECH_ENG_DLL)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
        "TISPEECH_ENG_DLL not set — building the rule engine only. "
        "The CLI and tests need language data; re-run with "
        "-DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL")
endif()
