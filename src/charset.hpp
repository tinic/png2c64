#pragma once

#include "dither.hpp"
#include "quantize.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace png2c64::charset {

enum class CharsetMode : unsigned char { hires, multicolor, mixed };

struct CharsetResult {
    std::array<std::uint8_t, 2048> charset_data{}; // 256 chars * 8 bytes
    std::vector<std::uint8_t> screen_map;           // cols*rows: charset index per cell
    std::vector<std::uint8_t> color_ram;            // cols*rows: per-cell color
    std::vector<bool> cell_is_hires;                // cols*rows: per-cell mode (mixed only)
    std::uint8_t background{};
    std::uint8_t mc1{};
    std::uint8_t mc2{};
    std::size_t cols{};
    std::size_t rows{};
    bool multicolor{};
    bool mixed{};
    std::size_t unique_before_merge{}; // unique patterns after dedup, before merge
    std::size_t chars_used{};          // final unique patterns in charset
    std::size_t empty_cells{};         // cells that are all-background
    std::size_t hires_cells{};         // cells using hires mode (mixed only)
    std::size_t mc_cells{};            // cells using multicolor mode (mixed only)
};

// Convert an image to a 256-character charset.
// Works with any image size that's a multiple of cell dimensions.
// Deduplicates identical patterns, merges closest pairs if > 256 unique.
//
// `metric` selects the per-cell color-selection error metric. Only
// charset-mc currently honors anything other than `mse`.
Result<CharsetResult> convert(const Image& image, const Palette& palette,
                              CharsetMode mode,
                              const dither::Settings& dither_settings,
                              quantize::Metric metric = quantize::Metric::mse);

Result<void> write_header(std::string_view path,
                          const CharsetResult& result,
                          std::string_view name);

// Generate C header as a string (for WASM/web export)
std::string generate_header(const CharsetResult& result,
                            std::string_view name);

Image render(const CharsetResult& result, const Palette& palette);

} // namespace png2c64::charset
