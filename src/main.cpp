#include "charset.hpp"
#include "dither.hpp"
#include "palette.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "prg.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"
#include "version.hpp"
#include "vic2.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <csignal>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif
#include <vector>

namespace {

using namespace png2c64;

// Build a ThresholdFn for dither-aware quantization (ordered dithers only)
quantize::ThresholdFn make_threshold_fn(const dither::Settings& ds) {
    if (dither::is_ordered(ds.method) && ds.method != dither::Method::none) {
        auto method = ds.method;
        auto strength = ds.strength;
        return [method, strength](std::size_t x, std::size_t y) {
            return dither::ordered_threshold(method, x, y) * strength;
        };
    }
    return {};
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Config {
    std::string input_path;
    std::string output_path;
    vic2::Mode mode = vic2::Mode::multicolor;
    std::string palette_name = "colodore";
    dither::Settings dither_settings{};
    preprocess::Settings preprocess{};
    std::optional<std::size_t> width;
    std::optional<std::size_t> height;
    std::string gallery;
    std::size_t sprites_x = 1;
    std::size_t sprites_y = 1;
    bool charset = false;
    bool charset_mc = false;
    bool charset_mixed = false;
    bool interactive = false;
    bool match_range = false;
    std::string error_map_path;
    quantize::Metric metric = quantize::Metric::mse;
    bool graphics_only = false;

    charset::CharsetMode get_charset_mode() const {
        if (charset_mixed) return charset::CharsetMode::mixed;
        if (charset_mc) return charset::CharsetMode::multicolor;
        return charset::CharsetMode::hires;
    }
};

void print_version() {
    std::println("png2c64 {}", png2c64::version);
}

void print_usage() {
    std::println(stderr,
        "png2c64 {}\n"
        "\n"
        "Usage: png2c64 [options] input.[png|jpg|bmp|tga] [output.png]\n"
        "\n"
        "Options:\n"
        "  --mode hires|multicolor|fli|afli|petscii|sprite-hi|sprite-mc|charset-hi|charset-mc|charset-mixed\n"
        "                                  VIC-II mode (default: multicolor)\n"
        "  --palette <name>                Color palette (default: colodore)\n"
        "     pepto, vice, colodore, deekay, godot, c64wiki, levy\n"
        "  --dither <method>               Dithering method (default: checker)\n"
        "     Square:  none, bayer4, bayer8, fs, atkinson, sierra\n"
        "     2:1 MC:  checker, bayer2x2, h2x4, clustered, fs-wide, jarvis\n"
        "     Lines:   line2, line-checker, line4, line8, line-fs\n"
        "     Dots:    halftone8, diagonal8, spiral5, hex8, hex5, blue-noise\n"
        "     Analy:   ign, r2, white-noise, crosshatch, radial, value-noise\n"
        "     Advanced: ostromoukhov\n"
        "  --dither-strength <float>       Dithering strength 0.0-2.0 (default: 0.7)\n"
        "  --error-clamp <float>           Max error per channel 0.1-2.0 (default: 0.1)\n"
        "  --adaptive <float>              Contrast-adaptive diffusion 0.0-1.0 (default: 0.0)\n"
        "  --no-serpentine                  Disable serpentine scanning\n"
        "  --match-range                    Enable palette range matching (default: off)\n"
        "  --brightness <float>            Brightness -1.0 to 1.0 (default: 0.0)\n"
        "  --contrast <float>              Contrast 0.0-2.0 (default: 1.0)\n"
        "  --saturation <float>            Saturation 0.0-2.0 (default: 1.0)\n"
        "  --gamma <float>                 Gamma 0.1-8.0 (default: 1.5)\n"
        "  --hue-shift <float>             Hue rotation -180 to 180 degrees (default: 0)\n"
        "  --sharpen <float>               Unsharp mask 0.0-2.0 (default: 0.0)\n"
        "  --black-point <float>           Black point clip 0.0-0.4 (default: 0.0)\n"
        "  --white-point <float>           White point clip 0.0-0.4 (default: 0.0)\n"
        "  --width <int>                   Override output width\n"
        "  --height <int>                  Override output height\n"
        "  --sprites-x <int>              Sprite sheet columns (default: 1)\n"
        "  --sprites-y <int>              Sprite sheet rows (default: 1)\n"
        "  --gallery <param>               Preview parameter variations in terminal\n"
        "     dither, brightness, contrast, saturation, gamma,\n"
        "     error-clamp, dither-strength, adaptive\n"
        "  --interactive                    Interactive mode (live parameter tuning)\n"
        "  --metric mse|blur|ssim          Per-cell error metric (PETSCII / charset-mc).\n"
        "                                    mse  = per-pixel OKLab MSE (default;\n"
        "                                           pairs with --dither <method>).\n"
        "                                    blur = Pappas-Neuhoff perceptual blur.\n"
        "                                    ssim = Wang structural similarity.\n"
        "  --error-map <path>               Save a per-pixel OKLab error\n"
        "                                    heatmap PNG (black→red→yellow→\n"
        "                                    white) at the mode's logical\n"
        "                                    resolution. Useful for spotting\n"
        "                                    fidelity hotspots and overlay\n"
        "                                    decisions.\n"
        "  --version                        Print version and exit\n",
        png2c64::version);
}

Result<Config> parse_args(int argc, char* argv[]) {
    Config config;
    int positional = 0;

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view(argv[i]);

        if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        }
        if (arg == "--version" || arg == "-v") {
            print_version();
            std::exit(0);
        }

        if (arg == "--no-serpentine") {
            config.dither_settings.serpentine = false;
            continue;
        }
        if (arg == "--match-range") {
            config.match_range = true;
            continue;
        }
        if (arg == "--no-match-range") {
            config.match_range = false;
            continue;
        }
        if (arg == "--interactive") {
            config.interactive = true;
            continue;
        }
        if (arg == "--graphics-only") {
            config.graphics_only = true;
            continue;
        }
        if (arg == "--metric" && i + 1 < argc) {
            auto val = std::string_view(argv[++i]);
            if (val == "mse")  config.metric = quantize::Metric::mse;
            else if (val == "blur") config.metric = quantize::Metric::blur;
            else if (val == "ssim") config.metric = quantize::Metric::ssim;
            else return std::unexpected{Error{ErrorCode::invalid_dimensions,
                "Unknown metric: " + std::string(val)}};
            continue;
        }

        if (arg.starts_with("--") && i + 1 < argc) {
            auto val = std::string_view(argv[++i]);

            if (arg == "--mode") {
                if (val == "hires") config.mode = vic2::Mode::hires;
                else if (val == "multicolor") config.mode = vic2::Mode::multicolor;
                else if (val == "fli") config.mode = vic2::Mode::fli;
                else if (val == "afli") config.mode = vic2::Mode::afli;
                else if (val == "petscii") config.mode = vic2::Mode::petscii;
                else if (val == "sprite-hi") config.mode = vic2::Mode::sprite_hires;
                else if (val == "sprite-mc") config.mode = vic2::Mode::sprite_multicolor;
                else if (val == "charset-hi" || val == "charset-mc" || val == "charset-mixed") {
                    config.charset_mc = (val == "charset-mc");
                    config.charset_mixed = (val == "charset-mixed");
                    config.charset = true;
                }
                else return std::unexpected{Error{ErrorCode::invalid_dimensions, "Unknown mode: " + std::string(val)}};
            } else if (arg == "--palette") {
                auto test = palette::by_name(val);
                if (test.colors.empty()) {
                    return std::unexpected{Error{ErrorCode::invalid_dimensions,
                        "Unknown palette '" + std::string(val) +
                        "'. Available: " + palette::available_names()}};
                }
                config.palette_name = std::string(val);
            } else if (arg == "--gallery") {
                config.gallery = std::string(val);
            } else if (arg == "--error-map") {
                config.error_map_path = std::string(val);
            } else if (arg == "--dither") {
                if (val == "gallery") { config.gallery = "dither"; }
                else if (val == "none") config.dither_settings.method = dither::Method::none;
                else if (val == "bayer4") config.dither_settings.method = dither::Method::bayer4x4;
                else if (val == "bayer8") config.dither_settings.method = dither::Method::bayer8x8;
                else if (val == "checker") config.dither_settings.method = dither::Method::checker;
                else if (val == "bayer2x2") config.dither_settings.method = dither::Method::bayer2x2;
                else if (val == "h2x4") config.dither_settings.method = dither::Method::h2x4;
                else if (val == "clustered") config.dither_settings.method = dither::Method::clustered_dot;
                else if (val == "fs") config.dither_settings.method = dither::Method::floyd_steinberg;
                else if (val == "atkinson") config.dither_settings.method = dither::Method::atkinson;
                else if (val == "sierra") config.dither_settings.method = dither::Method::sierra_lite;
                else if (val == "fs-wide") config.dither_settings.method = dither::Method::fs_wide;
                else if (val == "jarvis") config.dither_settings.method = dither::Method::jarvis;
                else if (val == "line2") config.dither_settings.method = dither::Method::line2;
                else if (val == "line-checker") config.dither_settings.method = dither::Method::line_checker;
                else if (val == "line4") config.dither_settings.method = dither::Method::line4;
                else if (val == "line8") config.dither_settings.method = dither::Method::line8;
                else if (val == "line-fs") config.dither_settings.method = dither::Method::line_fs;
                else if (val == "halftone8") config.dither_settings.method = dither::Method::halftone8x8;
                else if (val == "diagonal8") config.dither_settings.method = dither::Method::diagonal8x8;
                else if (val == "spiral5") config.dither_settings.method = dither::Method::spiral5x5;
                else if (val == "hex8") config.dither_settings.method = dither::Method::hex8x8;
                else if (val == "hex5") config.dither_settings.method = dither::Method::hex5x5;
                else if (val == "blue-noise") config.dither_settings.method = dither::Method::blue_noise;
                else if (val == "ign") config.dither_settings.method = dither::Method::ign;
                else if (val == "r2") config.dither_settings.method = dither::Method::r2_sequence;
                else if (val == "white-noise") config.dither_settings.method = dither::Method::white_noise;
                else if (val == "crosshatch") config.dither_settings.method = dither::Method::crosshatch;
                else if (val == "radial") config.dither_settings.method = dither::Method::radial;
                else if (val == "value-noise") config.dither_settings.method = dither::Method::value_noise;
                else if (val == "ostromoukhov") config.dither_settings.method = dither::Method::ostromoukhov;
                else return std::unexpected{Error{ErrorCode::invalid_dimensions, "Unknown dither: " + std::string(val)}};
            } else if (arg == "--dither-strength") {
                config.dither_settings.strength = std::stof(std::string(val));
            } else if (arg == "--error-clamp") {
                config.dither_settings.error_clamp = std::stof(std::string(val));
            } else if (arg == "--adaptive") {
                config.dither_settings.adaptive = std::stof(std::string(val));
            } else if (arg == "--brightness") {
                config.preprocess.brightness = std::stof(std::string(val));
            } else if (arg == "--contrast") {
                config.preprocess.contrast = std::stof(std::string(val));
            } else if (arg == "--saturation") {
                config.preprocess.saturation = std::stof(std::string(val));
            } else if (arg == "--gamma") {
                config.preprocess.gamma = std::stof(std::string(val));
            } else if (arg == "--hue-shift") {
                config.preprocess.hue_shift = std::stof(std::string(val));
            } else if (arg == "--sharpen") {
                config.preprocess.sharpen = std::stof(std::string(val));
            } else if (arg == "--black-point") {
                config.preprocess.black_point = std::stof(std::string(val));
            } else if (arg == "--white-point") {
                config.preprocess.white_point = std::stof(std::string(val));
            } else if (arg == "--width") {
                config.width = static_cast<std::size_t>(std::stoul(std::string(val)));
            } else if (arg == "--height") {
                config.height = static_cast<std::size_t>(std::stoul(std::string(val)));
            } else if (arg == "--sprites-x") {
                config.sprites_x = static_cast<std::size_t>(std::stoul(std::string(val)));
            } else if (arg == "--sprites-y") {
                config.sprites_y = static_cast<std::size_t>(std::stoul(std::string(val)));
            } else {
                return std::unexpected{Error{ErrorCode::invalid_dimensions, "Unknown option: " + std::string(arg)}};
            }
        } else {
            if (positional == 0) config.input_path = std::string(arg);
            else if (positional == 1) config.output_path = std::string(arg);
            ++positional;
        }
    }

    if (config.input_path.empty()) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions, "Input path required"}};
    }
    if (config.gallery.empty() && !config.interactive && config.output_path.empty()) {
        return std::unexpected{Error{ErrorCode::invalid_dimensions, "Output path required (or use --gallery / --interactive)"}};
    }

    return config;
}

// ---------------------------------------------------------------------------
// Base64 encoding
// ---------------------------------------------------------------------------

constexpr auto base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 2 < data.size()) {
        auto a = data[i], b = data[i + 1], c = data[i + 2];
        out += base64_chars[(a >> 2) & 0x3F];
        out += base64_chars[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out += base64_chars[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        out += base64_chars[c & 0x3F];
        i += 3;
    }

    if (i < data.size()) {
        auto a = data[i];
        out += base64_chars[(a >> 2) & 0x3F];
        if (i + 1 < data.size()) {
            auto b = data[i + 1];
            out += base64_chars[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
            out += base64_chars[((b & 0x0F) << 2)];
        } else {
            out += base64_chars[((a & 0x03) << 4)];
            out += '=';
        }
        out += '=';
    }

    return out;
}

// ---------------------------------------------------------------------------
// iTerm2 inline image display
// ---------------------------------------------------------------------------

void iterm2_display_bytes(std::span<const std::uint8_t> png_data,
                          std::size_t display_w, std::size_t display_h) {
    auto encoded = base64_encode(png_data);
    std::print("\033]1337;File=inline=1;size={};width={}px;height={}px:{}\a\n",
               png_data.size(), display_w, display_h, encoded);
}

void iterm2_display_image(const Image& image, unsigned display_scale) {
    auto png = png_io::encode(image);
    if (!png) return;
    iterm2_display_bytes(*png, image.width() * display_scale,
                         image.height() * display_scale);
}

// ---------------------------------------------------------------------------
// Render screen result to image
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Error-map output: per-pixel OKLab distance between the preprocessed
// source and the rendered output, colour-mapped (black → red → yellow →
// white). At the mode's logical resolution; rendered output is sampled
// at every pixel_stretch step so multicolor (2:1) maps 1:1 to source.
// ---------------------------------------------------------------------------

Image build_error_map(const Image& source, const Image& rendered,
                      const vic2::ModeParams& params, vic2::Mode mode) {
    auto w = params.screen_width;
    auto h = params.screen_height;
    std::size_t pixel_stretch = vic2::is_double_wide(mode) ? 2 : 1;

    std::vector<float> err(w * h);
    float max_err = 0.0f;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto s = color_space::linear_to_oklab(source[x, y]);
            auto r = color_space::linear_to_oklab(
                rendered[x * pixel_stretch, y]);
            float dL = s.L - r.L, da = s.a - r.a, db = s.b - r.b;
            float e = std::sqrt(dL * dL + da * da + db * db);
            err[y * w + x] = e;
            if (e > max_err) max_err = e;
        }
    }

    // Black → red gradient. Single-channel intensity is easier to read
    // than a multi-stop ramp; yellow/white stops added contrast that
    // made the actually-worst regions blend with the moderate ones.
    Image out(w * pixel_stretch, h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float t = (max_err > 0) ? err[y * w + x] / max_err : 0.0f;
            Color3f c = color_space::srgb_to_linear({t, 0.0f, 0.0f});
            for (std::size_t s = 0; s < pixel_stretch; ++s)
                out[x * pixel_stretch + s, y] = c;
        }
    }
    return out;
}

Image render_screen(const quantize::ScreenResult& screen,
                    const Palette& pal, const vic2::ModeParams& params) {
    auto w = params.screen_width;
    auto h = params.screen_height;
    auto cx = vic2::cells_x(params);

    std::size_t pixel_stretch = vic2::is_double_wide(screen.mode) ? 2 : 1;
    Image output(w * pixel_stretch, h);

    for (std::size_t cell_idx = 0; cell_idx < screen.cells.size();
         ++cell_idx) {
        auto& cell = screen.cells[cell_idx];
        auto cell_x = cell_idx % cx;
        auto cell_y = cell_idx / cx;
        auto px = cell_x * params.cell_width * pixel_stretch;
        auto py = cell_y * params.cell_height;

        bool fli = vic2::is_fli_mode(screen.mode);
        std::size_t pi = 0;
        for (std::size_t dy = 0; dy < params.cell_height; ++dy) {
            for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
                std::uint8_t color_idx;
                if (fli && screen.mode == vic2::Mode::fli) {
                    // FLI: [bg, colorram, r0_hi, r0_lo, ...]
                    std::array<std::uint8_t, 4> rc = {
                        cell.cell_colors[0], cell.cell_colors[2 + dy * 2],
                        cell.cell_colors[3 + dy * 2], cell.cell_colors[1]};
                    color_idx = rc[cell.pixel_indices[pi]];
                } else if (fli && screen.mode == vic2::Mode::afli) {
                    // AFLI FLI bug: on forced bad lines (rows 1-7),
                    // VIC-II reads $FF from floating bus → color 15
                    if (cell_x < vic2::fli_bug_columns && dy > 0)
                        color_idx = 15;
                    else
                        color_idx = cell.cell_colors[dy * 2 + cell.pixel_indices[pi]];
                } else {
                    color_idx = cell.cell_colors[cell.pixel_indices[pi]];
                }
                auto color = pal.colors[color_idx];
                auto out_x = px + dx * pixel_stretch;
                for (std::size_t s = 0; s < pixel_stretch; ++s) {
                    output[out_x + s, py + dy] = color;
                }
                ++pi;
            }
        }
    }

    return output;
}

// ---------------------------------------------------------------------------
// Full pipeline from preprocessed image -> displayed output
// ---------------------------------------------------------------------------

void run_pipeline_and_display(const Image& scaled_image,
                              const preprocess::Settings& pp,
                              const Palette& pal,
                              vic2::Mode mode,
                              const vic2::ModeParams& params,
                              const dither::Settings& ds,
                              bool match_range = true) {
    auto img = scaled_image; // copy
    preprocess::apply(img, pp);
    if (match_range) preprocess::match_palette_range(img, pal);

    auto tfn = make_threshold_fn(ds);
    auto screen = quantize::quantize(img, pal, mode, params, tfn, ds.strength);
    if (!screen) return;

    if (ds.method != dither::Method::none && !dither::is_ordered(ds.method)) {
        dither::apply(img, *screen, pal, params, ds);
    }

    auto output = render_screen(*screen, pal, params);
    iterm2_display_image(output, 3);
}

// Charset pipeline: preprocess -> palette match -> charset convert -> render -> display
void run_charset_pipeline_and_display(const Image& scaled_image,
                                       const preprocess::Settings& pp,
                                       const Palette& pal,
                                       charset::CharsetMode charset_mode,
                                       const dither::Settings& ds,
                                       bool match_range = true) {
    auto img = scaled_image;
    preprocess::apply(img, pp);
    if (match_range) preprocess::match_palette_range(img, pal);

    auto result = charset::convert(img, pal, charset_mode, ds);
    if (!result) return;

    auto preview = charset::render(*result, pal);
    iterm2_display_image(preview, 3);
}

// ---------------------------------------------------------------------------
// Gallery definitions
// ---------------------------------------------------------------------------

struct DitherGalleryEntry {
    dither::Method method;
    std::string_view label;
};

constexpr std::array dither_gallery = {
    DitherGalleryEntry{dither::Method::none,            "none"},
    DitherGalleryEntry{dither::Method::bayer4x4,        "bayer4 (4x4 ordered)"},
    DitherGalleryEntry{dither::Method::bayer8x8,        "bayer8 (8x8 ordered)"},
    DitherGalleryEntry{dither::Method::checker,         "checker (2:1 checkerboard)"},
    DitherGalleryEntry{dither::Method::bayer2x2,        "bayer2x2 (2:1 minimal)"},
    DitherGalleryEntry{dither::Method::h2x4,            "h2x4 (2:1 horiz-biased)"},
    DitherGalleryEntry{dither::Method::clustered_dot,   "clustered (2:1 dot)"},
    DitherGalleryEntry{dither::Method::floyd_steinberg, "fs (Floyd-Steinberg)"},
    DitherGalleryEntry{dither::Method::atkinson,        "atkinson (75% error)"},
    DitherGalleryEntry{dither::Method::sierra_lite,     "sierra (Sierra Lite)"},
    DitherGalleryEntry{dither::Method::fs_wide,         "fs-wide (2:1 Floyd-Steinberg)"},
    DitherGalleryEntry{dither::Method::jarvis,          "jarvis (2:1 Jarvis-Judice-Ninke)"},
    DitherGalleryEntry{dither::Method::line2,           "line2 (horizontal 2-level)"},
    DitherGalleryEntry{dither::Method::line_checker,   "line-checker (line-biased checker)"},
    DitherGalleryEntry{dither::Method::line4,           "line4 (horizontal 4-level)"},
    DitherGalleryEntry{dither::Method::line8,           "line8 (horizontal 8-level)"},
    DitherGalleryEntry{dither::Method::line_fs,         "line-fs (vertical error diffusion)"},
};

struct FloatGalleryEntry {
    float value;
    std::string_view label;
};

// Brightness: -0.3 .. +0.3
constexpr std::array brightness_gallery = {
    FloatGalleryEntry{-0.30f, "brightness = -0.30"},
    FloatGalleryEntry{-0.20f, "brightness = -0.20"},
    FloatGalleryEntry{-0.10f, "brightness = -0.10"},
    FloatGalleryEntry{ 0.00f, "brightness =  0.00 (default)"},
    FloatGalleryEntry{ 0.10f, "brightness = +0.10"},
    FloatGalleryEntry{ 0.20f, "brightness = +0.20"},
    FloatGalleryEntry{ 0.30f, "brightness = +0.30"},
};

// Contrast: 0.5 .. 1.5
constexpr std::array contrast_gallery = {
    FloatGalleryEntry{0.50f, "contrast = 0.50"},
    FloatGalleryEntry{0.70f, "contrast = 0.70"},
    FloatGalleryEntry{0.85f, "contrast = 0.85"},
    FloatGalleryEntry{1.00f, "contrast = 1.00 (default)"},
    FloatGalleryEntry{1.15f, "contrast = 1.15"},
    FloatGalleryEntry{1.30f, "contrast = 1.30"},
    FloatGalleryEntry{1.50f, "contrast = 1.50"},
};

// Saturation: 0.0 .. 2.0
constexpr std::array saturation_gallery = {
    FloatGalleryEntry{0.00f, "saturation = 0.00 (greyscale)"},
    FloatGalleryEntry{0.50f, "saturation = 0.50"},
    FloatGalleryEntry{0.75f, "saturation = 0.75"},
    FloatGalleryEntry{1.00f, "saturation = 1.00 (default)"},
    FloatGalleryEntry{1.25f, "saturation = 1.25"},
    FloatGalleryEntry{1.50f, "saturation = 1.50"},
    FloatGalleryEntry{2.00f, "saturation = 2.00"},
};

// Gamma: 0.25 .. 8.0 (exponential spacing around 1.0)
constexpr std::array gamma_gallery = {
    FloatGalleryEntry{0.25f, "gamma = 0.25 (much brighter)"},
    FloatGalleryEntry{0.40f, "gamma = 0.40"},
    FloatGalleryEntry{0.60f, "gamma = 0.60"},
    FloatGalleryEntry{0.80f, "gamma = 0.80"},
    FloatGalleryEntry{1.00f, "gamma = 1.00 (default)"},
    FloatGalleryEntry{1.40f, "gamma = 1.40"},
    FloatGalleryEntry{2.00f, "gamma = 2.00"},
    FloatGalleryEntry{3.00f, "gamma = 3.00"},
    FloatGalleryEntry{5.00f, "gamma = 5.00"},
    FloatGalleryEntry{8.00f, "gamma = 8.00 (much darker)"},
};

// Error clamp: 0.2 .. 2.0
constexpr std::array error_clamp_gallery = {
    FloatGalleryEntry{0.10f, "error-clamp = 0.10 (default)"},
    FloatGalleryEntry{0.20f, "error-clamp = 0.20"},
    FloatGalleryEntry{0.40f, "error-clamp = 0.40"},
    FloatGalleryEntry{0.60f, "error-clamp = 0.60"},
    FloatGalleryEntry{0.80f, "error-clamp = 0.80"},
    FloatGalleryEntry{1.50f, "error-clamp = 1.50"},
    FloatGalleryEntry{2.00f, "error-clamp = 2.00 (loose)"},
};

// Dither strength: 0.0 .. 2.0
constexpr std::array dither_strength_gallery = {
    FloatGalleryEntry{0.00f, "dither-strength = 0.00 (off)"},
    FloatGalleryEntry{0.25f, "dither-strength = 0.25"},
    FloatGalleryEntry{0.50f, "dither-strength = 0.50"},
    FloatGalleryEntry{0.70f, "dither-strength = 0.70 (default)"},
    FloatGalleryEntry{1.00f, "dither-strength = 1.00"},
    FloatGalleryEntry{1.25f, "dither-strength = 1.25"},
    FloatGalleryEntry{1.50f, "dither-strength = 1.50"},
    FloatGalleryEntry{2.00f, "dither-strength = 2.00"},
};

// Adaptive: 0.0 .. 1.0
constexpr std::array adaptive_gallery = {
    FloatGalleryEntry{0.00f, "adaptive = 0.00 (off)"},
    FloatGalleryEntry{0.15f, "adaptive = 0.15"},
    FloatGalleryEntry{0.30f, "adaptive = 0.30"},
    FloatGalleryEntry{0.50f, "adaptive = 0.50"},
    FloatGalleryEntry{0.70f, "adaptive = 0.70"},
    FloatGalleryEntry{0.85f, "adaptive = 0.85"},
    FloatGalleryEntry{1.00f, "adaptive = 1.00 (full)"},
};

// Dither gallery for bitmap modes: only re-runs dithering (fast)
void run_dither_gallery_bitmap(const Image& image,
                               const quantize::ScreenResult& screen,
                               const Palette& pal,
                               const vic2::ModeParams& params,
                               const dither::Settings& base_settings) {
    std::println("\n=== Dither Gallery ({} methods) ===\n",
                 dither_gallery.size());

    for (auto& [method, label] : dither_gallery) {
        auto screen_copy = screen;
        dither::Settings settings = base_settings;
        settings.method = method;

        if (method != dither::Method::none && !dither::is_ordered(method)) {
            dither::apply(image, screen_copy, pal, params, settings);
        }

        auto output = render_screen(screen_copy, pal, params);
        std::println("--- {} ---", label);
        iterm2_display_image(output, 3);
    }

    std::println("=== Gallery complete ===");
}

// Dither gallery for charset modes: re-runs charset convert per method
void run_dither_gallery_charset(const Image& image, const Palette& pal,
                                 charset::CharsetMode charset_mode,
                                 const dither::Settings& base_settings) {
    std::println("\n=== Dither Gallery ({} methods) ===\n",
                 dither_gallery.size());

    for (auto& [method, label] : dither_gallery) {
        dither::Settings settings = base_settings;
        settings.method = method;

        auto result = charset::convert(image, pal, charset_mode, settings);
        if (!result) continue;

        auto preview = charset::render(*result, pal);
        std::println("--- {} ---", label);
        iterm2_display_image(preview, 3);
    }

    std::println("=== Gallery complete ===");
}

// Float-parameter gallery: re-runs full pipeline for each value
template <std::size_t N>
void run_float_gallery(std::string_view title,
                       const std::array<FloatGalleryEntry, N>& entries,
                       const Image& scaled_image, const Config& config,
                       const Palette& pal, const vic2::ModeParams& params,
                       auto setter) {
    std::println("\n=== {} Gallery ({} values) ===\n", title, N);

    for (auto& [value, label] : entries) {
        auto pp = config.preprocess;
        auto ds = config.dither_settings;
        setter(pp, ds, value);

        std::println("--- {} ---", label);
        if (config.charset) {
            run_charset_pipeline_and_display(scaled_image, pp, pal,
                                             config.get_charset_mode(), ds, config.match_range);
        } else {
            run_pipeline_and_display(scaled_image, pp, pal, config.mode,
                                     params, ds, config.match_range);
        }
    }

    std::println("=== Gallery complete ===");
}

bool run_gallery(const std::string& gallery_name,
                 const Image& scaled_image, const Config& config,
                 const Palette& pal, const vic2::ModeParams& params,
                 const Image* preprocessed_image,
                 const quantize::ScreenResult* screen) {

    if (gallery_name == "dither") {
        if (config.charset) {
            if (preprocessed_image) {
                run_dither_gallery_charset(*preprocessed_image, pal,
                                           config.get_charset_mode(),
                                           config.dither_settings);
            }
        } else if (preprocessed_image && screen) {
            run_dither_gallery_bitmap(*preprocessed_image, *screen, pal,
                                      params, config.dither_settings);
        }
        return true;
    }

    if (gallery_name == "brightness") {
        run_float_gallery("Brightness", brightness_gallery, scaled_image,
            config, pal, params,
            [](preprocess::Settings& pp, dither::Settings&, float v) {
                pp.brightness = v;
            });
        return true;
    }

    if (gallery_name == "contrast") {
        run_float_gallery("Contrast", contrast_gallery, scaled_image,
            config, pal, params,
            [](preprocess::Settings& pp, dither::Settings&, float v) {
                pp.contrast = v;
            });
        return true;
    }

    if (gallery_name == "saturation") {
        run_float_gallery("Saturation", saturation_gallery, scaled_image,
            config, pal, params,
            [](preprocess::Settings& pp, dither::Settings&, float v) {
                pp.saturation = v;
            });
        return true;
    }

    if (gallery_name == "gamma") {
        run_float_gallery("Gamma", gamma_gallery, scaled_image,
            config, pal, params,
            [](preprocess::Settings& pp, dither::Settings&, float v) {
                pp.gamma = v;
            });
        return true;
    }

    if (gallery_name == "error-clamp") {
        run_float_gallery("Error Clamp", error_clamp_gallery, scaled_image,
            config, pal, params,
            [](preprocess::Settings&, dither::Settings& ds, float v) {
                ds.error_clamp = v;
            });
        return true;
    }

    if (gallery_name == "dither-strength") {
        run_float_gallery("Dither Strength", dither_strength_gallery,
            scaled_image, config, pal, params,
            [](preprocess::Settings&, dither::Settings& ds, float v) {
                ds.strength = v;
            });
        return true;
    }

    if (gallery_name == "adaptive") {
        run_float_gallery("Adaptive", adaptive_gallery,
            scaled_image, config, pal, params,
            [](preprocess::Settings&, dither::Settings& ds, float v) {
                ds.adaptive = v;
            });
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Interactive mode (POSIX only — uses termios for raw-tty input)
// ---------------------------------------------------------------------------

#ifndef _WIN32

// Global so the signal handler can restore terminal state
termios g_original_termios{};

void restore_terminal([[maybe_unused]] int sig) {
    std::print("\033[?25h"); // show cursor
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_original_termios);
    if (sig) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }
}

struct RawTerminal {
    RawTerminal() {
        tcgetattr(STDIN_FILENO, &g_original_termios);
        termios raw = g_original_termios;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        std::print("\033[?25l"); // hide cursor
        std::signal(SIGINT, restore_terminal);
        std::signal(SIGTERM, restore_terminal);
    }
    ~RawTerminal() {
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
        restore_terminal(0);
    }
};

#endif // _WIN32

// Short dither names for display
constexpr std::array<std::string_view, 30> dither_names = {
    "none", "bayer4", "bayer8", "checker", "bayer2x2", "h2x4",
    "clustered", "line2", "line-check", "line4", "line8",
    "fs", "atkinson", "sierra", "fs-wide", "jarvis", "line-fs",
    "halftone8", "diagonal8", "spiral5", "hex8", "hex5", "blue-noise",
    "ign", "r2", "white-noise", "crosshatch", "radial", "value-noise",
    "ostromoukhov",
};

constexpr std::array<dither::Method, 30> dither_methods = {
    dither::Method::none, dither::Method::bayer4x4, dither::Method::bayer8x8,
    dither::Method::checker, dither::Method::bayer2x2, dither::Method::h2x4,
    dither::Method::clustered_dot, dither::Method::line2,
    dither::Method::line_checker, dither::Method::line4, dither::Method::line8,
    dither::Method::floyd_steinberg, dither::Method::atkinson,
    dither::Method::sierra_lite, dither::Method::fs_wide,
    dither::Method::jarvis, dither::Method::line_fs,
    dither::Method::halftone8x8, dither::Method::diagonal8x8,
    dither::Method::spiral5x5, dither::Method::hex8x8,
    dither::Method::hex5x5, dither::Method::blue_noise,
    dither::Method::ign, dither::Method::r2_sequence,
    dither::Method::white_noise, dither::Method::crosshatch,
    dither::Method::radial, dither::Method::value_noise,
    dither::Method::ostromoukhov,
};

#ifndef _WIN32

std::size_t dither_index(dither::Method m) {
    for (std::size_t i = 0; i < dither_methods.size(); ++i)
        if (dither_methods[i] == m) return i;
    return 0;
}

std::size_t palette_index(std::string_view name) {
    for (std::size_t i = 0; i < palette::all_palettes.size(); ++i)
        if (palette::all_palettes[i].name == name) return i;
    return 0;
}

void run_interactive(const Image& scaled_image, Config& config,
                     vic2::Mode mode, const vic2::ModeParams& params) {

    auto pal_idx = palette_index(config.palette_name);
    auto dit_idx = dither_index(config.dither_settings.method);
    auto& pp = config.preprocess;
    auto& ds = config.dither_settings;
    bool is_charset = config.charset;
    auto charset_mode = config.get_charset_mode();

    RawTerminal term;
    std::print("\033[2J"); // clear once on entry

    auto refresh = [&] {
        auto pal = palette::by_name(
            palette::all_palettes[pal_idx].name);
        ds.method = dither_methods[dit_idx];
        config.palette_name = std::string(palette::all_palettes[pal_idx].name);

        // Run pipeline
        auto img = scaled_image;
        preprocess::apply(img, pp);
        if (config.match_range) preprocess::match_palette_range(img, pal);

        Image output;
        if (is_charset) {
            auto result = charset::convert(img, pal, charset_mode, ds);
            if (!result) return;
            output = charset::render(*result, pal);
        } else {
            auto tfn = make_threshold_fn(ds);
            auto screen = quantize::quantize(img, pal, mode, params,
                                              tfn, ds.strength);
            if (!screen) return;
            if (ds.method != dither::Method::none && !dither::is_ordered(ds.method))
                dither::apply(img, *screen, pal, params, ds);
            output = render_screen(*screen, pal, params);
        }

        // Move cursor home and overwrite in place (no flicker)
        std::print("\033[H");

        // Panel (each line ends with \033[K to clear stale characters)
        std::println("\033[1m png2c64 interactive \033[0m\033[K");
        std::println("\033[K");
        std::println(" \033[33mp/P\033[0m palette    \033[1m{:<12}\033[0m"
                     "  \033[33md/D\033[0m dither   \033[1m{:<12}\033[0m\033[K",
                     config.palette_name, dither_names[dit_idx]);
        std::println(" \033[33mg/G\033[0m gamma      \033[1m{:<12.2f}\033[0m"
                     "  \033[33ms/S\033[0m strength \033[1m{:<12.2f}\033[0m\033[K",
                     pp.gamma, ds.strength);
        std::println(" \033[33mb/B\033[0m bright     \033[1m{:<12.2f}\033[0m"
                     "  \033[33mc/C\033[0m contrast \033[1m{:<12.2f}\033[0m\033[K",
                     pp.brightness, pp.contrast);
        std::println(" \033[33mt/T\033[0m saturation \033[1m{:<12.2f}\033[0m"
                     "  \033[33mh/H\033[0m hue      \033[1m{:<12.1f}\033[0m\033[K",
                     pp.saturation, pp.hue_shift);
        std::println(" \033[33mn/N\033[0m sharpen    \033[1m{:<12.2f}\033[0m"
                     "  \033[33m[/]\033[0m bk/wt pt \033[1m{:.2f}/{:.2f}\033[0m\033[K",
                     pp.sharpen, pp.black_point, pp.white_point);
        std::println(" \033[33me/E\033[0m errclamp   \033[1m{:<12.2f}\033[0m"
                     "  \033[33ma/A\033[0m adaptive \033[1m{:<12.2f}\033[0m\033[K",
                     ds.error_clamp, ds.adaptive);
        std::println(" \033[33m x \033[0m serpentine \033[1m{:<12}\033[0m"
                     "  \033[33m r \033[0m reset  \033[33m w \033[0m save  \033[33m q \033[0m quit\033[K",
                     ds.serpentine ? "on" : "off");
        std::println("\033[K");

        iterm2_display_image(output, 3);
        std::print("\033[J"); // clear any stale content below
        std::fflush(stdout);
    };

    refresh();

    constexpr float step = 0.05f;
    constexpr float gamma_factor = 1.15f;

    while (true) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) break;

        switch (ch) {
        case 'q': return;

        // Palette
        case 'p':
            pal_idx = (pal_idx + 1) % palette::all_palettes.size();
            break;
        case 'P':
            pal_idx = (pal_idx + palette::all_palettes.size() - 1) %
                      palette::all_palettes.size();
            break;

        // Dither method
        case 'd':
            dit_idx = (dit_idx + 1) % dither_methods.size();
            break;
        case 'D':
            dit_idx = (dit_idx + dither_methods.size() - 1) %
                      dither_methods.size();
            break;

        // Gamma (multiplicative)
        case 'g': pp.gamma = std::min(pp.gamma * gamma_factor, 8.0f); break;
        case 'G': pp.gamma = std::max(pp.gamma / gamma_factor, 0.1f); break;

        // Dither strength
        case 's': ds.strength = std::min(ds.strength + step, 3.0f); break;
        case 'S': ds.strength = std::max(ds.strength - step, 0.0f); break;

        // Brightness
        case 'b': pp.brightness = std::min(pp.brightness + step, 1.0f); break;
        case 'B': pp.brightness = std::max(pp.brightness - step, -1.0f); break;

        // Contrast
        case 'c': pp.contrast = std::min(pp.contrast + step, 3.0f); break;
        case 'C': pp.contrast = std::max(pp.contrast - step, 0.0f); break;

        // Saturation
        case 't': pp.saturation = std::min(pp.saturation + step, 3.0f); break;
        case 'T': pp.saturation = std::max(pp.saturation - step, 0.0f); break;

        // Hue shift
        case 'h': pp.hue_shift = std::min(pp.hue_shift + 5.0f, 180.0f); break;
        case 'H': pp.hue_shift = std::max(pp.hue_shift - 5.0f, -180.0f); break;

        // Sharpen (negative = blur)
        case 'n': pp.sharpen = std::min(pp.sharpen + step, 2.0f); break;
        case 'N': pp.sharpen = std::max(pp.sharpen - step, -1.0f); break;

        // Black/white point
        case '[': pp.black_point = std::min(pp.black_point + 0.01f, 0.4f); break;
        case '{': pp.black_point = std::max(pp.black_point - 0.01f, 0.0f); break;
        case ']': pp.white_point = std::min(pp.white_point + 0.01f, 0.4f); break;
        case '}': pp.white_point = std::max(pp.white_point - 0.01f, 0.0f); break;

        // Error clamp
        case 'e': ds.error_clamp = std::min(ds.error_clamp + step, 3.0f); break;
        case 'E': ds.error_clamp = std::max(ds.error_clamp - step, 0.05f); break;

        // Adaptive
        case 'a': ds.adaptive = std::min(ds.adaptive + step, 1.0f); break;
        case 'A': ds.adaptive = std::max(ds.adaptive - step, 0.0f); break;

        // Serpentine toggle
        case 'x': ds.serpentine = !ds.serpentine; break;

        // Reset
        case 'r':
            pp = preprocess::Settings{};
            ds = dither::Settings{};
            pal_idx = 0;
            dit_idx = dither_index(ds.method);
            break;

        // Save
        case 'w':
            if (!config.output_path.empty()) {
                auto pal = palette::by_name(config.palette_name);
                auto img = scaled_image;
                preprocess::apply(img, pp);
                if (config.match_range) preprocess::match_palette_range(img, pal);
                if (is_charset) {
                    auto result = charset::convert(img, pal, charset_mode, ds);
                    if (result) {
                        // Derive C identifier from path
                        auto stem = config.output_path;
                        auto slash = stem.find_last_of('/');
                        if (slash != std::string::npos) stem = stem.substr(slash + 1);
                        auto dot = stem.find_last_of('.');
                        if (dot != std::string::npos) stem = stem.substr(0, dot);
                        for (auto& c : stem)
                            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                        charset::write_header(config.output_path, *result, stem);
                    }
                } else {
                    auto tfn = make_threshold_fn(ds);
                    auto screen = quantize::quantize(img, pal, mode, params,
                                                      tfn, ds.strength);
                    if (screen) {
                        if (ds.method != dither::Method::none && !dither::is_ordered(ds.method))
                            dither::apply(img, *screen, pal, params, ds);
                        auto out = render_screen(*screen, pal, params);
                        png_io::save(config.output_path, out);
                    }
                }
            }
            break;

        default: continue; // unknown key, don't refresh
        }

        refresh();
    }
}

#else // _WIN32

void run_interactive(const Image&, Config&, vic2::Mode,
                     const vic2::ModeParams&) {
    std::println(stderr,
        "--interactive is not supported on Windows "
        "(requires a POSIX tty).");
}

#endif // _WIN32

} // namespace

int main(int argc, char* argv[]) {
    using namespace png2c64;

    auto config = parse_args(argc, argv);
    if (!config) {
        std::println(stderr, "Error: {}", config.error().message);
        print_usage();
        return 1;
    }

    // Load
    std::println("Loading {}...", config->input_path);
    auto image = png_io::load(config->input_path);
    if (!image) {
        std::println(stderr, "Error: {}", image.error().message);
        return 1;
    }
    std::println("  Loaded {}x{}", image->width(), image->height());

    // --- Charset mode: separate pipeline ---
    if (config->charset) {
        auto pal = palette::by_name(config->palette_name);
        std::println("Using palette: {} ({} colors)", pal.name, pal.size());

        // Scale to requested dimensions if --width/--height provided
        if (config->width || config->height) {
            auto tw = config->width.value_or(image->width());
            auto th = config->height.value_or(image->height());
            if (image->width() != tw || image->height() != th) {
                std::println("Scaling to {}x{}...", tw, th);
                image = scale::bicubic(*std::move(image), tw, th);
                if (!image) {
                    std::println(stderr, "Error: {}", image.error().message);
                    return 1;
                }
            }
        }

        // For pure multicolor (not mixed), scale input width /2 to get logical resolution
        if (config->charset_mc && !config->charset_mixed) {
            auto logical_w = image->width() / 2;
            std::println("Scaling to logical {}x{} for multicolor...",
                         logical_w, image->height());
            image = scale::bicubic(*std::move(image), logical_w,
                                   image->height());
            if (!image) {
                std::println(stderr, "Error: {}", image.error().message);
                return 1;
            }
        }

        // Keep pre-preprocessed copy for galleries/interactive
        auto scaled_image = *image;

        // Interactive mode
        if (config->interactive) {
            vic2::ModeParams dummy_params{};
            run_interactive(scaled_image, *config,
                            vic2::Mode::multicolor, dummy_params);
            return 0;
        }

        // Preprocess
        preprocess::apply(*image, config->preprocess);
        if (config->match_range) preprocess::match_palette_range(*image, pal);

        // Gallery mode
        if (!config->gallery.empty()) {
            // For the dither gallery, pass the preprocessed image
            // For float galleries, pass the scaled (pre-preprocess) image
            vic2::ModeParams dummy_params{}; // not used for charset galleries
            if (!run_gallery(config->gallery, scaled_image, *config, pal,
                             dummy_params, &*image, nullptr)) {
                std::println(stderr, "Unknown gallery: {}", config->gallery);
                std::println(stderr, "Available: dither, brightness, contrast, "
                                     "saturation, gamma, error-clamp, "
                                     "dither-strength");
                return 1;
            }
            return 0;
        }

        // Convert
        auto cmode = config->get_charset_mode();
        auto mode_label = config->charset_mixed ? "mixed hires/multicolor"
                        : config->charset_mc ? "multicolor" : "hires";
        std::println("Converting to {} charset...", mode_label);
        auto result = charset::convert(*image, pal, cmode,
                                       config->dither_settings,
                                       config->metric);
        if (!result) {
            std::println(stderr, "Error: {}", result.error().message);
            return 1;
        }

        std::println("  {}x{} grid, {} cells", result->cols,
                     result->rows, result->cols * result->rows);
        std::println("  Unique chars: {} / 256 ({} empty cells)",
                     result->chars_used, result->empty_cells);
        if (result->unique_before_merge > 256) {
            std::println("  Merged {} -> 256",
                         result->unique_before_merge);
        }
        std::println("  Background: {}", result->background);
        if (config->charset_mc || config->charset_mixed) {
            std::println("  Multicolor1: {}, Multicolor2: {}",
                         result->mc1, result->mc2);
        }
        if (config->charset_mixed) {
            std::println("  Hires cells: {}, MC cells: {}",
                         result->hires_cells, result->mc_cells);
        }

        // Terminal preview
        auto preview = charset::render(*result, pal);
        iterm2_display_image(preview, 3);

        // Write output
        if (!config->output_path.empty()) {
            bool is_png = config->output_path.ends_with(".png");

            if (is_png) {
                std::println("Saving {}...", config->output_path);
                auto save_result = png_io::save(config->output_path, preview);
                if (!save_result) {
                    std::println(stderr, "Error: {}", save_result.error().message);
                    return 1;
                }
            } else {
                // Derive a C identifier from the output filename
                auto stem = config->output_path;
                auto slash = stem.find_last_of('/');
                if (slash != std::string::npos) stem = stem.substr(slash + 1);
                auto dot = stem.find_last_of('.');
                if (dot != std::string::npos) stem = stem.substr(0, dot);
                // Replace non-alnum with underscore
                for (auto& ch : stem)
                    if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';

                std::println("Writing header: {}", config->output_path);
                auto wr = charset::write_header(config->output_path, *result,
                                                 stem);
                if (!wr) {
                    std::println(stderr, "Error: {}", wr.error().message);
                    return 1;
                }
            }

            // Export PRG alongside output
            auto prg_result = prg::charset_text(*result);
            if (prg_result) {
                auto prg_path = config->output_path;
                auto pdot = prg_path.rfind('.');
                if (pdot != std::string::npos) prg_path = prg_path.substr(0, pdot);
                prg_path += ".prg";
                auto pwr = prg::write(prg_path, *prg_result);
                if (pwr) {
                    std::println("PRG: {} ({} bytes)", prg_path,
                                 prg_result->bytes.size());
                }
            }
        }

        std::println("Done!");
        return 0;
    }

    // Compute mode params (sprites use grid dimensions)
    auto params = vic2::is_sprite_mode(config->mode)
        ? vic2::get_sprite_params(config->mode, config->sprites_x,
                                  config->sprites_y)
        : vic2::get_mode_params(config->mode);

    // Scale to target dimensions
    auto target_w = config->width.value_or(params.screen_width);
    auto target_h = config->height.value_or(params.screen_height);

    if (image->width() != target_w || image->height() != target_h) {
        std::println("Scaling to {}x{}...", target_w, target_h);
        image = scale::bicubic(*std::move(image), target_w, target_h);
        if (!image) {
            std::println(stderr, "Error: {}", image.error().message);
            return 1;
        }
    }

    // Keep a copy of the scaled image before preprocessing
    auto scaled_image = *image;

    // Interactive mode
    if (config->interactive) {
        run_interactive(scaled_image, *config, config->mode, params);
        return 0;
    }

    // Get palette
    auto pal = palette::by_name(config->palette_name);
    std::println("Using palette: {} ({} colors)", pal.name, pal.size());

    // Preprocess
    std::println("Preprocessing...");
    preprocess::apply(*image, config->preprocess);
    if (config->match_range) preprocess::match_palette_range(*image, pal);

    // Quantize
    auto mode_name = [&] {
        switch (config->mode) {
        case vic2::Mode::hires:              return "hires";
        case vic2::Mode::multicolor:         return "multicolor";
        case vic2::Mode::sprite_hires:       return "sprite-hires";
        case vic2::Mode::sprite_multicolor:  return "sprite-multicolor";
        case vic2::Mode::fli:                return "fli";
        case vic2::Mode::afli:               return "afli";
        case vic2::Mode::petscii:            return "petscii";
        }
        std::unreachable();
    }();

    if (vic2::is_sprite_mode(config->mode)) {
        std::println("Sprite sheet: {}x{} sprites ({}x{} pixels)",
                     config->sprites_x, config->sprites_y,
                     params.screen_width, params.screen_height);
    }

    std::println("Quantizing ({})...", mode_name);
    auto tfn = make_threshold_fn(config->dither_settings);
    auto screen = quantize::quantize(*image, pal, config->mode, params,
                                     tfn, config->dither_settings.strength,
                                     config->metric, config->graphics_only);
    if (!screen) {
        std::println(stderr, "Error: {}", screen.error().message);
        return 1;
    }

    // Gallery mode
    if (!config->gallery.empty()) {
        if (!run_gallery(config->gallery, scaled_image, *config, pal, params,
                         &*image, &*screen)) {
            std::println(stderr, "Unknown gallery: {}", config->gallery);
            std::println(stderr, "Available: dither, brightness, contrast, "
                                 "saturation, gamma, error-clamp, "
                                 "dither-strength");
            return 1;
        }
        return 0;
    }

    // Post-quantize dither only for error-diffusion methods. Ordered
    // methods feed their threshold into `quantize` via the threshold
    // fn, which bakes the dither pattern into `pixel_indices` at
    // per-pixel-nearest selection time. Running `apply_ordered_matrix`
    // on top re-derives a pattern using a *different* rule (perceptual
    // blend of the two nearest cell colours) that forces dither even
    // on pixels that are already clearly closer to one colour —
    // adding speckle/noise on smooth areas. Skip it.
    if (config->dither_settings.method != dither::Method::none &&
        !dither::is_ordered(config->dither_settings.method)) {
        std::println("Dithering...");
        dither::apply(*image, *screen, pal, params, config->dither_settings);
    }

    // Render and save
    std::println("Saving {}...", config->output_path);
    auto output = render_screen(*screen, pal, params);
    auto save_result = png_io::save(config->output_path, output);
    if (!save_result) {
        std::println(stderr, "Error: {}", save_result.error().message);
        return 1;
    }

    // Optional error map: per-pixel OKLab distance, colour-mapped.
    if (!config->error_map_path.empty()) {
        auto emap = build_error_map(*image, output, params, config->mode);
        auto er = png_io::save(config->error_map_path, emap);
        if (!er) {
            std::println(stderr, "Error map: {}", er.error().message);
        } else {
            std::println("Error map: {}", config->error_map_path);
        }
    }

    // Export PRG alongside PNG (for bitmap modes)
    auto prg_result = prg::from_screen(*screen);
    if (prg_result) {
        auto prg_path = config->output_path;
        auto dot = prg_path.rfind('.');
        if (dot != std::string::npos) prg_path = prg_path.substr(0, dot);
        prg_path += ".prg";
        auto wr = prg::write(prg_path, *prg_result);
        if (wr) {
            std::println("PRG: {} ({} bytes)", prg_path, prg_result->bytes.size());
        }
    }

    std::println("Done! Mode: {}, palette: {}, total error: {:.4f}",
                 mode_name, pal.name, screen->total_error);

    iterm2_display_image(output, 3);

    return 0;
}
