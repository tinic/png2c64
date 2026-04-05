#pragma once

#include <cstddef>
#include <utility>

namespace png2c64::vic2 {

enum class Mode : unsigned char {
    hires,
    multicolor,
    sprite_hires,
    sprite_multicolor,
    fli,                // FLI multicolor: per-row colors within 4x8 cells
    afli,               // AFLI hires FLI: per-row colors within 8x8 cells
};

struct ModeParams {
    std::size_t screen_width;
    std::size_t screen_height;
    std::size_t cell_width;
    std::size_t cell_height;
    std::size_t colors_per_cell;
    bool has_shared_background;
};

constexpr ModeParams get_mode_params(Mode mode) noexcept {
    switch (mode) {
    case Mode::hires:
        return {320, 200, 8, 8, 2, false};
    case Mode::multicolor:
        return {160, 200, 4, 8, 4, true};
    case Mode::sprite_hires:
        return {24, 21, 24, 21, 2, false};
    case Mode::sprite_multicolor:
        return {12, 21, 12, 21, 4, true};
    case Mode::fli:
        return {160, 200, 4, 8, 4, true};  // same grid as multicolor
    case Mode::afli:
        return {320, 200, 8, 8, 2, false};  // same grid as hires
    }
    std::unreachable();
}

constexpr ModeParams get_sprite_params(Mode mode, std::size_t sprites_x,
                                       std::size_t sprites_y) noexcept {
    auto p = get_mode_params(mode);
    p.screen_width = p.cell_width * sprites_x;
    p.screen_height = p.cell_height * sprites_y;
    return p;
}

constexpr bool is_sprite_mode(Mode mode) noexcept {
    return mode == Mode::sprite_hires || mode == Mode::sprite_multicolor;
}

constexpr bool is_double_wide(Mode mode) noexcept {
    return mode == Mode::multicolor || mode == Mode::sprite_multicolor
        || mode == Mode::fli;
}

constexpr bool is_fli_mode(Mode mode) noexcept {
    return mode == Mode::fli || mode == Mode::afli;
}

// FLI bug: left 3 character columns show garbage
constexpr std::size_t fli_bug_columns = 3;

constexpr std::size_t cells_x(const ModeParams& p) noexcept {
    return p.screen_width / p.cell_width;
}

constexpr std::size_t cells_y(const ModeParams& p) noexcept {
    return p.screen_height / p.cell_height;
}

} // namespace png2c64::vic2
