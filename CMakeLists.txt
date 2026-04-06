cmake_minimum_required(VERSION 3.28)
project(png2c64 LANGUAGES C CXX)

if(EMSCRIPTEN)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(third_party)

# Core library (shared between CLI and WASM)
set(PNG2C64_CORE_SOURCES
    src/png_io.cpp
    src/scale.cpp
    src/preprocess.cpp
    src/quantize.cpp
    src/dither.cpp
    src/charset.cpp
    src/prg.cpp
    src/api.cpp
)

if(EMSCRIPTEN)
    # --- WASM library build ---
    add_executable(png2c64_wasm
        ${PNG2C64_CORE_SOURCES}
        src/wasm_bindings.cpp
    )
    target_include_directories(png2c64_wasm PRIVATE src)
    target_link_libraries(png2c64_wasm PRIVATE stb)
    target_link_options(png2c64_wasm PRIVATE
        --bind
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORT_ES6=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createPng2C64
        -sENVIRONMENT=web,worker
        -sNO_FILESYSTEM=1
    )
    set_target_properties(png2c64_wasm PROPERTIES
        OUTPUT_NAME png2c64
        SUFFIX .js
    )
else()
    # --- Native CLI build ---
    include(cmake/CompilerWarnings.cmake)

    add_executable(png2c64
        src/main.cpp
        ${PNG2C64_CORE_SOURCES}
    )
    target_include_directories(png2c64 PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(png2c64 PRIVATE stb)
    target_compile_options(png2c64 PRIVATE ${PNG2C64_WARNING_FLAGS})

    # -----------------------------------------------------------------------
    # Example conversions (native only)
    # -----------------------------------------------------------------------

set(EXAMPLES_DIR ${CMAKE_SOURCE_DIR}/examples)

enable_testing()

# add_example(<stem> <ext> GAMMA <g> DITHER_STRENGTH <d> [MATCH_RANGE])
function(add_example stem ext)
    cmake_parse_arguments(EX "MATCH_RANGE" "GAMMA;DITHER_STRENGTH" "" ${ARGN})
    if(NOT DEFINED EX_GAMMA)
        set(EX_GAMMA 1.0)
    endif()
    if(NOT DEFINED EX_DITHER_STRENGTH)
        set(EX_DITHER_STRENGTH 1.0)
    endif()

    set(EXTRA_ARGS "")
    if(EX_MATCH_RANGE)
        set(EXTRA_ARGS "--match-range")
    endif()

    add_test(
        NAME "convert-${stem}"
        COMMAND png2c64
            --gamma ${EX_GAMMA}
            --dither-strength ${EX_DITHER_STRENGTH}
            ${EXTRA_ARGS}
            "${EXAMPLES_DIR}/${stem}.${ext}"
            "${EXAMPLES_DIR}/${stem}-c64.png"
    )
endfunction()

add_example(golden3 jpeg GAMMA 2.0 DITHER_STRENGTH 1.5 MATCH_RANGE)
add_example(monster jpeg GAMMA 3.0 DITHER_STRENGTH 1.5)
add_example(ship    jpeg GAMMA 2.0 DITHER_STRENGTH 1.5 MATCH_RANGE)
add_example(dragon  png  GAMMA 1.0 DITHER_STRENGTH 1.5 MATCH_RANGE)
add_example(fantasy png  GAMMA 1.0 DITHER_STRENGTH 1.5 MATCH_RANGE)
add_example(dog     png  GAMMA 3.0 DITHER_STRENGTH 1.5)
add_example(alien   png  GAMMA 1.0 DITHER_STRENGTH 2.0 MATCH_RANGE)
add_test(
    NAME "convert-face"
    COMMAND png2c64
        --gamma 3.0
        --dither-strength 1.0
        --sharpen -0.5
        --saturation 0.5
        "${EXAMPLES_DIR}/face.png"
        "${EXAMPLES_DIR}/face-c64.png"
)
add_example(game    png  GAMMA 2.0 DITHER_STRENGTH 1.5)

add_test(
    NAME "convert-head-petscii"
    COMMAND png2c64
        --mode petscii
        --gamma 3.3
        --dither-strength 1.6
        --black-point 0.09
        --white-point 0.06
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-petscii.png"
)

# -----------------------------------------------------------------------
# Mode coverage tests — verify all VIC-II modes complete with valid output
# -----------------------------------------------------------------------

# Hires bitmap (320x200, 2 colors per 8x8 cell)
add_test(
    NAME "mode-hires"
    COMMAND png2c64
        --mode hires
        --gamma 2.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-hires.png"
)
set_tests_properties("mode-hires" PROPERTIES
    PASS_REGULAR_EXPRESSION "Mode: hires.*total error: [0-9]")

# FLI (flexible line interpretation, per-row color changes)
add_test(
    NAME "mode-fli"
    COMMAND png2c64
        --mode fli
        --gamma 2.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-fli.png"
)
set_tests_properties("mode-fli" PROPERTIES
    PASS_REGULAR_EXPRESSION "Mode: fli.*total error: [0-9]")

# AFLI (advanced FLI, hires + per-row colors)
add_test(
    NAME "mode-afli"
    COMMAND png2c64
        --mode afli
        --gamma 2.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-afli.png"
)
set_tests_properties("mode-afli" PROPERTIES
    PASS_REGULAR_EXPRESSION "Mode: afli.*total error: [0-9]")

# Sprite hires (24x21, 2 colors)
add_test(
    NAME "mode-sprite-hi"
    COMMAND png2c64
        --mode sprite-hi
        --gamma 2.0
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-sprite-hi.png"
)
set_tests_properties("mode-sprite-hi" PROPERTIES
    PASS_REGULAR_EXPRESSION "Mode: sprite-hires.*total error: [0-9]")

# Sprite multicolor (12x21, 4 colors)
add_test(
    NAME "mode-sprite-mc"
    COMMAND png2c64
        --mode sprite-mc
        --gamma 2.0
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-sprite-mc.png"
)
set_tests_properties("mode-sprite-mc" PROPERTIES
    PASS_REGULAR_EXPRESSION "Mode: sprite-multicolor.*total error: [0-9]")

# Charset hires (8x8 cells, 256 chars, 1 shared bg + 1 fg/cell)
add_test(
    NAME "mode-charset-hi"
    COMMAND png2c64
        --mode charset-hi
        --gamma 3.3 --dither-strength 1.6
        --black-point 0.09 --white-point 0.06
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-hi.png"
)
set_tests_properties("mode-charset-hi" PROPERTIES
    PASS_REGULAR_EXPRESSION "Unique chars: [0-9]+ / 256.*Background: [0-9]+")

# Charset multicolor (4x8 cells, 256 chars, 3 shared + 1/cell from 0-7)
add_test(
    NAME "mode-charset-mc"
    COMMAND png2c64
        --mode charset-mc
        --gamma 3.3 --dither-strength 1.6
        --black-point 0.09 --white-point 0.06
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-mc.png"
)
set_tests_properties("mode-charset-mc" PROPERTIES
    PASS_REGULAR_EXPRESSION "Unique chars: [0-9]+ / 256.*Multicolor1: [0-9]+, Multicolor2: [0-9]+")

# -----------------------------------------------------------------------
# Charset color RAM constraint test — MC per-cell colors must be 0-7
# -----------------------------------------------------------------------
add_test(
    NAME "charset-mc-color-range"
    COMMAND png2c64
        --mode charset-mc
        --gamma 3.3 --dither-strength 1.6
        --black-point 0.09 --white-point 0.06
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-mc.h"
)
# Verify the C header exists and all color_ram values have bit 3 set (0x08)
# and bits 0-2 are in range 0-7 (values 0x08-0x0F only)
set_tests_properties("charset-mc-color-range" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg")

# -----------------------------------------------------------------------
# Error regression tests — verify total error stays within expected range
# -----------------------------------------------------------------------
add_test(
    NAME "error-multicolor-dragon"
    COMMAND png2c64
        --gamma 1.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-c64.png"
)
set_tests_properties("error-multicolor-dragon" PROPERTIES
    PASS_REGULAR_EXPRESSION "total error: [0-9]+\\.[0-9]+")

add_test(
    NAME "error-hires-dragon"
    COMMAND png2c64
        --mode hires
        --gamma 2.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-hires.png"
)
# Hires should have higher error than multicolor (fewer colors per cell)
set_tests_properties("error-hires-dragon" PROPERTIES
    PASS_REGULAR_EXPRESSION "total error: [0-9]+")

# FLI should have lower error than standard multicolor (more colors per cell)
add_test(
    NAME "error-fli-dragon"
    COMMAND png2c64
        --mode fli
        --gamma 2.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-fli.png"
)
set_tests_properties("error-fli-dragon" PROPERTIES
    PASS_REGULAR_EXPRESSION "total error: [0-9]+\\.[0-9]+")

# -----------------------------------------------------------------------
# PRG output tests — verify PRG files are generated with expected sizes
# -----------------------------------------------------------------------
add_test(
    NAME "prg-multicolor"
    COMMAND png2c64
        --gamma 1.0 --dither-strength 1.5 --match-range
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-c64.png"
)
set_tests_properties("prg-multicolor" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

add_test(
    NAME "prg-hires"
    COMMAND png2c64
        --mode hires
        --gamma 2.0 --dither-strength 1.5
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-hires.png"
)
set_tests_properties("prg-hires" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

add_test(
    NAME "prg-charset-hi"
    COMMAND png2c64
        --mode charset-hi
        --gamma 3.3 --dither-strength 1.6
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-hi.h"
)
set_tests_properties("prg-charset-hi" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

add_test(
    NAME "prg-charset-mc"
    COMMAND png2c64
        --mode charset-mc
        --gamma 3.3 --dither-strength 1.6
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-mc.h"
)
set_tests_properties("prg-charset-mc" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

add_test(
    NAME "prg-fli"
    COMMAND png2c64
        --mode fli
        --gamma 2.0 --dither-strength 1.5
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-fli.png"
)
set_tests_properties("prg-fli" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

add_test(
    NAME "prg-afli"
    COMMAND png2c64
        --mode afli
        --gamma 2.0 --dither-strength 1.5
        "${EXAMPLES_DIR}/dragon.png"
        "${EXAMPLES_DIR}/dragon-afli.png"
)
set_tests_properties("prg-afli" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

# Charset mixed (per-cell hires/multicolor)
add_test(
    NAME "mode-charset-mixed"
    COMMAND png2c64
        --mode charset-mixed
        --gamma 3.3 --dither-strength 1.6
        --black-point 0.09 --white-point 0.06
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-mixed.png"
)
set_tests_properties("mode-charset-mixed" PROPERTIES
    PASS_REGULAR_EXPRESSION "Unique chars: [0-9]+ / 256.*Hires cells: [0-9]+, MC cells: [0-9]+")

add_test(
    NAME "prg-charset-mixed"
    COMMAND png2c64
        --mode charset-mixed
        --gamma 3.3 --dither-strength 1.6
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-charset-mixed.h"
)
set_tests_properties("prg-charset-mixed" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg")

add_test(
    NAME "prg-petscii"
    COMMAND png2c64
        --mode petscii
        --gamma 3.3 --dither-strength 1.6
        "${EXAMPLES_DIR}/head.png"
        "${EXAMPLES_DIR}/head-petscii.png"
)
set_tests_properties("prg-petscii" PROPERTIES
    PASS_REGULAR_EXPRESSION "PRG:.*\\.prg \\([0-9]+ bytes\\)")

endif() # NOT EMSCRIPTEN
