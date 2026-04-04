#pragma once

#include "quantize.hpp"
#include "types.hpp"

namespace png2c64::dither {

enum class Method : unsigned char {
    none,

    // Square-pixel ordered dithering
    bayer4x4,
    bayer8x8,

    // 2:1 pixel-ratio ordered dithering (multicolor-aware)
    checker,          // 2x1 checkerboard, 2 levels
    bayer2x2,         // 2x2 Bayer, 4 levels
    h2x4,             // 2x4 Bayer, 8 levels -- perceptually square at 2:1
    clustered_dot,    // 4x4 clustered dot shaped for 2:1 display

    // Horizontal line ordered dithering
    line2,            // 1x2 alternating rows, 2 levels
    line_checker,     // 2x2 line-biased: rows differ more than columns
    line4,            // 1x4, 4 levels
    line8,            // 1x8, 8 levels

    // Error diffusion -- standard (square pixel)
    floyd_steinberg,
    atkinson,
    sierra_lite,

    // Error diffusion -- 2:1 pixel-ratio aware
    fs_wide,          // Floyd-Steinberg with weights adjusted for 2:1
    jarvis,           // Jarvis-Judice-Ninke (wider kernel, handles 2:1 naturally)

    // Error diffusion -- horizontal line biased
    line_fs,          // Error only diffuses downward, no horizontal neighbor
};

struct Settings {
    Method method = Method::checker;
    float strength = 1.0f;    // 0.0 = no dithering, 1.0 = full
    float error_clamp = 0.8f; // max error magnitude per channel (linear)
    bool serpentine = true;    // alternate scan direction per row
    float adaptive = 0.0f;    // 0.0 = off, 1.0 = full contrast-adaptive diffusion
};

void apply(const Image& image, quantize::ScreenResult& result,
           const Palette& palette, const vic2::ModeParams& params,
           const Settings& settings);

// Returns true if the method is an ordered (non-error-diffusion) method.
constexpr bool is_ordered(Method m) noexcept {
    switch (m) {
    case Method::none:
    case Method::bayer4x4: case Method::bayer8x8:
    case Method::checker: case Method::bayer2x2:
    case Method::h2x4: case Method::clustered_dot:
    case Method::line2: case Method::line_checker:
    case Method::line4: case Method::line8:
        return true;
    default:
        return false;
    }
}

// Get the raw ordered dither threshold at pixel (x, y) for a given method.
// Returns value in [-0.5, 0.5]. Returns 0 for non-ordered methods.
float ordered_threshold(Method method, std::size_t x, std::size_t y);

} // namespace png2c64::dither
