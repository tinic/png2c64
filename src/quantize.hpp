#pragma once

#include "types.hpp"
#include "vic2.hpp"

#include <cstdint>
#include <vector>

namespace png2c64::quantize {

struct CellResult {
    std::vector<std::uint8_t> pixel_indices;  // per-pixel index into cell_colors
    std::vector<std::uint8_t> cell_colors;    // palette indices used by this cell
    float error{};
};

struct ScreenResult {
    std::vector<CellResult> cells;     // row-major, 40x25
    std::uint8_t background_color{};   // for multicolor mode
    vic2::Mode mode{};
    float total_error{};
};

Result<ScreenResult> quantize(const Image& image, const Palette& palette,
                              vic2::Mode mode,
                              const vic2::ModeParams& params);

} // namespace png2c64::quantize
