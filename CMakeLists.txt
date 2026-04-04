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
    target_include_directories(png2c64 PRIVATE src)
    target_link_libraries(png2c64 PRIVATE stb)
    target_compile_options(png2c64 PRIVATE ${PNG2C64_WARNING_FLAGS})

    # -----------------------------------------------------------------------
    # Example conversions (native only)
    # -----------------------------------------------------------------------

set(EXAMPLES_DIR ${CMAKE_SOURCE_DIR}/examples)

enable_testing()

# add_example(<stem> <ext> GAMMA <g> DITHER_STRENGTH <d>)
function(add_example stem ext)
    cmake_parse_arguments(EX "" "GAMMA;DITHER_STRENGTH" "" ${ARGN})
    if(NOT DEFINED EX_GAMMA)
        set(EX_GAMMA 1.0)
    endif()
    if(NOT DEFINED EX_DITHER_STRENGTH)
        set(EX_DITHER_STRENGTH 1.0)
    endif()

    add_test(
        NAME "convert-${stem}"
        COMMAND png2c64
            --gamma ${EX_GAMMA}
            --dither-strength ${EX_DITHER_STRENGTH}
            "${EXAMPLES_DIR}/${stem}.${ext}"
            "${EXAMPLES_DIR}/${stem}-c64.png"
    )
endfunction()

add_example(golden3 jpeg GAMMA 2.0 DITHER_STRENGTH 1.5)
add_example(monster jpeg GAMMA 3.0 DITHER_STRENGTH 1.5)
add_example(ship    jpeg GAMMA 2.0 DITHER_STRENGTH 1.5)
add_example(dragon  png  GAMMA 1.0 DITHER_STRENGTH 1.5)
add_example(fantasy png  GAMMA 1.0 DITHER_STRENGTH 1.5)
add_example(dog     png  GAMMA 3.0 DITHER_STRENGTH 1.5)
add_example(alien   png  GAMMA 0.8 DITHER_STRENGTH 0.8)
add_example(face    png  GAMMA 3.0 DITHER_STRENGTH 1.0)
add_example(game    png  GAMMA 2.0 DITHER_STRENGTH 1.5)

endif() # NOT EMSCRIPTEN
