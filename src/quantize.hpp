#pragma once

#include "types.hpp"
#include "vic2.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace png2c64::quantize {

// Optional per-pixel threshold function for dither-aware quantization.
// Called as threshold(x, y) returning a value in [-0.5, 0.5].
// When set, the quantizer biases color selection by the threshold,
// picking colors that dither well together rather than just nearest.
using ThresholdFn = std::function<float(std::size_t, std::size_t)>;

struct CellResult {
    std::vector<std::uint8_t> pixel_indices;  // per-pixel index into cell_colors
    std::vector<std::uint8_t> cell_colors;    // palette indices used by this cell
    float error{};
    std::uint8_t char_index{};                // PETSCII: character ROM index
};

struct ScreenResult {
    std::vector<CellResult> cells;     // row-major, 40x25
    std::uint8_t background_color{};   // for multicolor mode
    vic2::Mode mode{};
    float total_error{};
};

Result<ScreenResult> quantize(const Image& image, const Palette& palette,
                              vic2::Mode mode,
                              const vic2::ModeParams& params,
                              ThresholdFn threshold = {},
                              float threshold_strength = 0.0f);

} // namespace png2c64::quantize
