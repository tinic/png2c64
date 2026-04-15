#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace png2c64::api {

struct Options {
    std::string mode = "multicolor";
    std::string palette = "colodore";
    std::string dither = "checker";
    float gamma = 1.5f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float dither_strength = 1.0f;
    float error_clamp = 0.1f;
    float adaptive = 0.0f;
    float hue_shift = 0.0f;
    float sharpen = 0.0f;
    float black_point = 0.0f;
    float white_point = 0.0f;
    bool serpentine = true;
    bool match_range = false;
    int width = 0;
    int height = 0;

    // Per-cell error metric for charset modes (PETSCII, charset-hi,
    // charset-mc) and any other mode that exposes a metric selector:
    // "mse" (default; pairs with dither), "blur" (Pappas-Neuhoff
    // perceptual halftoning), or "ssim" (structural similarity).
    std::string metric = "mse";

    // PETSCII only: restrict the candidate glyph set to graphic
    // characters only — skip alphabet, digits, punctuation and their
    // reverse-video forms. Useful for "demo-style" halftone output
    // where letter-shaped artefacts in smooth regions look out of
    // place. See petscii::is_graphic_char for the exact subset.
    bool graphics_only = false;
};

struct ConvertResult {
    std::vector<std::uint8_t> png_data;
    int width{};
    int height{};
    std::string error;
};

// Convert raw image data (PNG/JPEG/BMP bytes) to C64 format.
// Returns PNG bytes of the converted image.
ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options);

// Convert raw image data and return raw RGBA pixel data instead of PNG.
ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options);

// Convert and return a per-pixel OKLab-distance error heatmap (RGBA),
// black → red → yellow → white. Same dimensions as convert_rgba output.
ConvertResult convert_error_map_rgba(const std::uint8_t* input_data,
                                     std::size_t input_size,
                                     const Options& options);

// Convert raw image data and return PRG bytes (Koala/Hires bitmap).
// Only works for multicolor and hires modes.
ConvertResult convert_prg(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert and return raw Koala Paint .koa file (multicolor only).
ConvertResult convert_koa(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert and return raw Art Studio .hir file (hires only).
ConvertResult convert_hir(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options);

// Convert raw image data and return C header text (charset modes only).
ConvertResult convert_header(const std::uint8_t* input_data,
                             std::size_t input_size,
                             const Options& options,
                             const std::string& name);

} // namespace png2c64::api
