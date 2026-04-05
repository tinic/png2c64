cmake_minimum_required(VERSION 3.28)
project(exomizer C)

set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_CROSSCOMPILING)
    # --- Native build: generate sfxdecr.c from assembly ---

    add_executable(exoraw
        src/exo_raw.c src/match.c src/search.c src/optimal.c src/output.c
        src/buf_io.c src/chunkpool.c src/radix.c src/exo_helper.c
        src/exodec.c src/progress.c src/getflag.c src/log.c src/buf.c
        src/table.c src/perf.c src/vec.c src/exo_util.c
        src/named_buffer.c src/map.c src/areatrace.c
    )
    target_compile_options(exoraw PRIVATE -O3 -ffast-math -fomit-frame-pointer
        -Wno-sign-compare -Wno-unused-parameter)

    add_executable(b2buf src/b2buf.c)

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/sfxdecr
        COMMAND exoraw -q ${CMAKE_CURRENT_SOURCE_DIR}/src/sfxdecr.s
                -o ${CMAKE_CURRENT_BINARY_DIR}/sfxdecr
        DEPENDS exoraw ${CMAKE_CURRENT_SOURCE_DIR}/src/sfxdecr.s
    )

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/sfxdecr.c
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}
                $<TARGET_FILE:b2buf> sfxdecr > ${CMAKE_CURRENT_BINARY_DIR}/sfxdecr.c
        DEPENDS b2buf ${CMAKE_CURRENT_BINARY_DIR}/sfxdecr
    )

    set(SFXDECR_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/sfxdecr.c)
else()
    # --- Cross-compile (WASM): use pre-generated sfxdecr ---
    set(SFXDECR_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/sfxdecr_generated.c)
endif()

# --- libexomizer (static library) ---

add_library(libexomizer STATIC
    src/match.c src/search.c src/optimal.c src/output.c
    src/buf_io.c src/chunkpool.c src/radix.c src/exo_helper.c
    src/exodec.c src/progress.c src/asm.tab.c src/lex.yy.c
    src/parse.c src/expr.c src/pc.c src/vec.c src/named_buffer.c
    src/map.c src/desfx.c src/6502emu.c src/exo_util.c
    src/areatrace.c src/getflag.c src/log.c src/buf.c
    src/table.c src/perf.c
    ${SFXDECR_SOURCE}
)
target_include_directories(libexomizer PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(libexomizer PRIVATE -O3 -ffast-math -fomit-frame-pointer
    -Wno-sign-compare -Wno-unused-parameter -Wno-implicit-fallthrough)
