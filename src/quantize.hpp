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

// Per-cell error metric driving the brute-force glyph + (fg, bg) search.
//
//   mse  — Sum of squared OKLab distances per sub-pixel. Optimised with
//          the precomputed-distance + decomposition trick (independent
//          argmins). Default for hires/multicolor/sprite modes; pairs
//          naturally with pixel-level dithering.
//
//   blur — Pappas-Neuhoff perceptual halftoning. Both source and the
//          rendered cell are convolved with a 3×3 binomial low-pass
//          (≈ HVS contrast sensitivity) before MSE is taken. Encoder
//          naturally prefers checker glyphs that *look* right after
//          eye-blur, replacing pixel-level dither for the metric's
//          purposes. PETSCII default.
//
//   ssim — Wang-Bovik-Sheikh-Simoncelli structural similarity, summed
//          over L/a/b. Per-channel SSIM in closed form for a binary
//          rendering: σ_xy = S_fg/n · (fg − bg). 8×8 cells = 64 pixels
//          which is Wang's canonical window size, so variance estimates
//          are statistically meaningful here (unlike at 16-pixel cells).
enum class Metric : std::uint8_t { mse, blur, ssim };

Result<ScreenResult> quantize(const Image& image, const Palette& palette,
                              vic2::Mode mode,
                              const vic2::ModeParams& params,
                              ThresholdFn threshold = {},
                              float threshold_strength = 0.0f,
                              Metric metric = Metric::mse,
                              bool graphics_only = false);

} // namespace png2c64::quantize
