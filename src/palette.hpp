#pragma once

#include "color_space.hpp"
#include "types.hpp"
#include <array>
#include <string_view>
#include <vector>

namespace png2c64::palette {

// VIC-II color indices:
//  0: Black        4: Purple       8: Orange      12: Medium Grey
//  1: White        5: Green        9: Brown       13: Light Green
//  2: Red          6: Blue        10: Light Red   14: Light Blue
//  3: Cyan         7: Yellow      11: Dark Grey   15: Light Grey

namespace detail {

constexpr auto to_linear_palette(const std::array<std::uint32_t, 16>& hex) {
    std::array<Color3f, 16> colors{};
    for (std::size_t i = 0; i < 16; ++i) {
        colors[i] = color_space::srgb_hex_to_linear(hex[i]);
    }
    return colors;
}

// Pepto palette (Philip "Pepto" Timmermann)
// Source: https://www.pepto.de/projects/colorvic/
constexpr std::array<std::uint32_t, 16> pepto_hex = {{
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
    0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444,
    0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
}};

// VICE emulator default palette
constexpr std::array<std::uint32_t, 16> vice_hex = {{
    0x000000, 0xFDFEFC, 0xBE1A24, 0x30E6C6,
    0xB41AE2, 0x1FD21E, 0x211BAE, 0xDFF60A,
    0xB84104, 0x6A3304, 0xFE4A57, 0x424540,
    0x70746F, 0x59FE59, 0x5F53FE, 0xA4A7A2,
}};

// Colodore palette (considered most accurate measurement)
// Source: https://www.colodore.com/
constexpr std::array<std::uint32_t, 16> colodore_hex = {{
    0x000000, 0xFFFFFF, 0x813338, 0x75CEC8,
    0x8E3C97, 0x56AC4D, 0x2E2C9B, 0xEDF171,
    0x8E5029, 0x553800, 0xC46C71, 0x4A4A4A,
    0x7B7B7B, 0xA9FF9F, 0x706DEB, 0xB2B2B2,
}};

// Deekay palette (by Deekay/Crest, widely used community standard)
constexpr std::array<std::uint32_t, 16> deekay_hex = {{
    0x000000, 0xFFFFFF, 0x882000, 0x68D0A8,
    0xA838A0, 0x50B818, 0x181090, 0xF0E858,
    0xA04800, 0x472B1B, 0xC87870, 0x484848,
    0x808080, 0x98FF98, 0x5090D0, 0xB8B8B8,
}};

// Godot palette (C64 Godot paint program, godot64.de)
constexpr std::array<std::uint32_t, 16> godot_hex = {{
    0x000000, 0xFFFFFF, 0x880000, 0xAAFFEE,
    0xCC44CC, 0x00CC55, 0x0000AA, 0xEEEE77,
    0xDD8855, 0x664400, 0xFF7777, 0x333333,
    0x777777, 0xAAFF66, 0x0088FF, 0xBBBBBB,
}};

// C64 Wiki / "raw" palette (simple, clean values often seen in references)
// Source: https://www.c64-wiki.com/wiki/Color
constexpr std::array<std::uint32_t, 16> c64wiki_hex = {{
    0x000000, 0xFFFFFF, 0x880000, 0xAAFFEE,
    0xCC44CC, 0x00CC55, 0x0000AA, 0xEEEE77,
    0xDD8855, 0x664400, 0xFF7777, 0x333333,
    0x777777, 0xAAFF66, 0x0088FF, 0xBBBBBB,
}};

// Levy palette (adjusted Pepto, used by some modern tools)
constexpr std::array<std::uint32_t, 16> levy_hex = {{
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
    0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444,
    0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
}};

} // namespace detail

inline constexpr auto pepto_colors = detail::to_linear_palette(detail::pepto_hex);
inline constexpr auto vice_colors = detail::to_linear_palette(detail::vice_hex);
inline constexpr auto colodore_colors = detail::to_linear_palette(detail::colodore_hex);
inline constexpr auto deekay_colors = detail::to_linear_palette(detail::deekay_hex);
inline constexpr auto godot_colors = detail::to_linear_palette(detail::godot_hex);
inline constexpr auto c64wiki_colors = detail::to_linear_palette(detail::c64wiki_hex);
inline constexpr auto levy_colors = detail::to_linear_palette(detail::levy_hex);

struct PaletteEntry {
    std::string_view name;
    std::span<const Color3f, 16> colors;
};

// All available palettes, for iteration / lookup
inline constexpr std::array all_palettes = {
    PaletteEntry{"pepto",    pepto_colors},
    PaletteEntry{"vice",     vice_colors},
    PaletteEntry{"colodore", colodore_colors},
    PaletteEntry{"deekay",   deekay_colors},
    PaletteEntry{"godot",    godot_colors},
    PaletteEntry{"c64wiki",  c64wiki_colors},
    PaletteEntry{"levy",     levy_colors},
};

inline Palette by_name(std::string_view name) {
    for (auto& [n, colors] : all_palettes) {
        if (n == name) {
            return {std::string(n), {colors.begin(), colors.end()}};
        }
    }
    return {}; // empty — caller should validate
}

// Return comma-separated list of palette names for help/error messages
inline std::string available_names() {
    std::string result;
    for (auto& [n, colors] : all_palettes) {
        if (!result.empty()) result += ", ";
        result += n;
    }
    return result;
}

} // namespace png2c64::palette
