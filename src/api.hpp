#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace png2c64::api {

struct Options {
    std::string mode = "multicolor";
    std::string palette = "pepto";
    std::string dither = "checker";
    float gamma = 1.0f;
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float dither_strength = 1.0f;
    float error_clamp = 0.8f;
    float adaptive = 0.0f;
    bool serpentine = true;
    int width = 0;   // 0 = auto (mode default)
    int height = 0;
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
