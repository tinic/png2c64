#include "api.hpp"
#include "charset.hpp"
#include "color_space.hpp"
#include "prg.hpp"

#include <stb_image.h>
#include "dither.hpp"
#include "palette.hpp"
#include "png_io.hpp"
#include "preprocess.hpp"
#include "quantize.hpp"
#include "scale.hpp"
#include "types.hpp"
#include "vic2.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <sstream>

namespace png2c64::api {

namespace {

vic2::Mode parse_mode(const std::string& s) {
    if (s == "hires") return vic2::Mode::hires;
    if (s == "multicolor") return vic2::Mode::multicolor;
    if (s == "sprite-hi") return vic2::Mode::sprite_hires;
    if (s == "sprite-mc") return vic2::Mode::sprite_multicolor;
    if (s == "fli") return vic2::Mode::fli;
    if (s == "afli") return vic2::Mode::afli;
    if (s == "petscii") return vic2::Mode::petscii;
    return vic2::Mode::multicolor;
}

dither::Method parse_dither(const std::string& s) {
    if (s == "none") return dither::Method::none;
    if (s == "bayer4") return dither::Method::bayer4x4;
    if (s == "bayer8") return dither::Method::bayer8x8;
    if (s == "checker") return dither::Method::checker;
    if (s == "bayer2x2") return dither::Method::bayer2x2;
    if (s == "h2x4") return dither::Method::h2x4;
    if (s == "clustered") return dither::Method::clustered_dot;
    if (s == "line2") return dither::Method::line2;
    if (s == "line-checker") return dither::Method::line_checker;
    if (s == "line4") return dither::Method::line4;
    if (s == "line8") return dither::Method::line8;
    if (s == "fs") return dither::Method::floyd_steinberg;
    if (s == "atkinson") return dither::Method::atkinson;
    if (s == "sierra") return dither::Method::sierra_lite;
    if (s == "fs-wide") return dither::Method::fs_wide;
    if (s == "jarvis") return dither::Method::jarvis;
    if (s == "line-fs") return dither::Method::line_fs;
    if (s == "halftone8") return dither::Method::halftone8x8;
    if (s == "diagonal8") return dither::Method::diagonal8x8;
    if (s == "spiral5") return dither::Method::spiral5x5;
    if (s == "hex8") return dither::Method::hex8x8;
    if (s == "hex5") return dither::Method::hex5x5;
    if (s == "blue-noise") return dither::Method::blue_noise;
    if (s == "ign") return dither::Method::ign;
    if (s == "r2") return dither::Method::r2_sequence;
    if (s == "white-noise") return dither::Method::white_noise;
    if (s == "crosshatch") return dither::Method::crosshatch;
    if (s == "radial") return dither::Method::radial;
    if (s == "value-noise") return dither::Method::value_noise;
    if (s == "ostromoukhov") return dither::Method::ostromoukhov;
    return dither::Method::checker;
}

bool is_charset_mode(const std::string& mode) {
    return mode == "charset-hi" || mode == "charset-mc" || mode == "charset-mixed";
}

charset::CharsetMode get_charset_mode(const std::string& mode) {
    if (mode == "charset-mixed") return charset::CharsetMode::mixed;
    if (mode == "charset-mc") return charset::CharsetMode::multicolor;
    return charset::CharsetMode::hires;
}

struct PipelineResult {
    Image rendered;
    quantize::ScreenResult screen;
    Image preprocessed;       // post-scale, post-preprocess source
    vic2::ModeParams params;  // for pixel_stretch
    vic2::Mode mode;
};

// Load image from memory, scale, preprocess
Result<Image> load_and_preprocess(const std::uint8_t* input_data,
                                   std::size_t input_size,
                                   const Options& options,
                                   std::size_t target_w,
                                   std::size_t target_h) {
    int w{}, h{}, channels{};
    auto* raw = stbi_load_from_memory(input_data,
        static_cast<int>(input_size), &w, &h, &channels, 3);
    if (!raw)
        return std::unexpected{Error{ErrorCode::invalid_png, "Failed to decode image"}};

    auto width = static_cast<std::size_t>(w);
    auto height = static_cast<std::size_t>(h);
    std::vector<Color3f> pixels(width * height);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        auto base = i * 3;
        pixels[i] = color_space::srgb_u8_to_linear(
            raw[base], raw[base + 1], raw[base + 2]);
    }
    stbi_image_free(raw);

    Image image(width, height, std::move(pixels));

    if (image.width() != target_w || image.height() != target_h) {
        auto scaled = scale::bicubic(image, target_w, target_h);
        if (!scaled) return std::unexpected{scaled.error()};
        image = *std::move(scaled);
    }

    preprocess::Settings pp;
    pp.gamma = options.gamma;
    pp.brightness = options.brightness;
    pp.contrast = options.contrast;
    pp.saturation = options.saturation;
    pp.hue_shift = options.hue_shift;
    pp.sharpen = options.sharpen;
    pp.black_point = options.black_point;
    pp.white_point = options.white_point;
    preprocess::apply(image, pp);

    auto pal = palette::by_name(options.palette);
    if (pal.colors.empty()) pal = palette::by_name("colodore");
    if (options.match_range)
        preprocess::match_palette_range(image, pal);

    return image;
}

quantize::Metric parse_metric(const Options& options) {
    if (options.metric == "blur") return quantize::Metric::blur;
    if (options.metric == "ssim") return quantize::Metric::ssim;
    return quantize::Metric::mse;
}

dither::Settings make_dither_settings(const Options& options) {
    dither::Settings ds;
    ds.method = parse_dither(options.dither);
    ds.strength = options.dither_strength;
    ds.error_clamp = options.error_clamp;
    ds.serpentine = options.serpentine;
    ds.adaptive = options.adaptive;
    return ds;
}

// Charset pipeline: returns rendered preview image
Result<Image> run_charset_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& options) {
    auto cmode = get_charset_mode(options.mode);
    bool mc_halve = (options.mode == "charset-mc"); // only pure MC halves width

    // Determine target dimensions
    auto target_w = options.width > 0
        ? static_cast<std::size_t>(options.width) : std::size_t{320};
    auto target_h = options.height > 0
        ? static_cast<std::size_t>(options.height) : std::size_t{200};

    auto image = load_and_preprocess(input_data, input_size, options,
                                      target_w, target_h);
    if (!image) return std::unexpected{image.error()};

    // For pure multicolor charset, scale width /2 for logical resolution
    if (mc_halve) {
        auto logical_w = image->width() / 2;
        auto scaled = scale::bicubic(*image, logical_w, image->height());
        if (!scaled) return std::unexpected{scaled.error()};
        image = std::move(scaled);
    }

    auto pal = palette::by_name(options.palette);
    if (pal.colors.empty()) pal = palette::by_name("colodore");

    auto ds = make_dither_settings(options);
    auto metric = parse_metric(options);
    auto result = charset::convert(*image, pal, cmode, ds, metric);
    if (!result) return std::unexpected{result.error()};

    return charset::render(*result, pal);
}

// Bitmap/sprite pipeline
Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& options) {
    auto mode = parse_mode(options.mode);
    auto params = vic2::get_mode_params(mode);

    auto target_w = options.width > 0
        ? static_cast<std::size_t>(options.width) : params.screen_width;
    auto target_h = options.height > 0
        ? static_cast<std::size_t>(options.height) : params.screen_height;
    params.screen_width = target_w;
    params.screen_height = target_h;

    auto image = load_and_preprocess(input_data, input_size, options,
                                      target_w, target_h);
    if (!image) return std::unexpected{image.error()};

    auto pal = palette::by_name(options.palette);
    if (pal.colors.empty()) pal = palette::by_name("colodore");

    auto ds = make_dither_settings(options);

    quantize::ThresholdFn tfn;
    if (dither::is_ordered(ds.method) && ds.method != dither::Method::none) {
        auto m = ds.method;
        auto s = ds.strength;
        tfn = [m, s](std::size_t x, std::size_t y) {
            return dither::ordered_threshold(m, x, y) * s;
        };
    }

    auto metric =
        options.metric == "blur" ? quantize::Metric::blur :
        options.metric == "ssim" ? quantize::Metric::ssim :
                                   quantize::Metric::mse;

    auto screen = quantize::quantize(*image, pal, mode, params, tfn, ds.strength, metric,
                                     options.graphics_only);
    if (!screen) return std::unexpected{screen.error()};

    // Post-quantize dithering only makes sense with the MSE metric — blur
    // and ssim already encode against the continuous source and would be
    // disturbed by an extra error-diffusion pass on top.
    if (ds.method != dither::Method::none && !dither::is_ordered(ds.method))
        dither::apply(*image, *screen, pal, params, ds);

    // Render
    auto cx = vic2::cells_x(params);
    std::size_t pixel_stretch = vic2::is_double_wide(mode) ? 2 : 1;
    Image output(params.screen_width * pixel_stretch, params.screen_height);

    for (std::size_t ci = 0; ci < screen->cells.size(); ++ci) {
        auto& cell = screen->cells[ci];
        auto cell_x = ci % cx;
        auto cell_y = ci / cx;
        auto px = cell_x * params.cell_width * pixel_stretch;
        auto py = cell_y * params.cell_height;

        bool fli = vic2::is_fli_mode(mode);
        std::size_t pi = 0;
        for (std::size_t dy = 0; dy < params.cell_height; ++dy) {
            for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
                std::uint8_t color_idx;
                if (fli && mode == vic2::Mode::fli) {
                    std::array<std::uint8_t, 4> rc = {
                        cell.cell_colors[0], cell.cell_colors[2 + dy * 2],
                        cell.cell_colors[3 + dy * 2], cell.cell_colors[1]};
                    color_idx = rc[cell.pixel_indices[pi]];
                } else if (fli && mode == vic2::Mode::afli) {
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
                for (std::size_t s = 0; s < pixel_stretch; ++s)
                    output[out_x + s, py + dy] = color;
                ++pi;
            }
        }
    }

    return PipelineResult{std::move(output), *std::move(screen),
                          *std::move(image), params, mode};
}

// Get rendered Image for any mode (charset or bitmap/sprite)
Result<Image> get_rendered(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options) {
    if (is_charset_mode(options.mode))
        return run_charset_pipeline(input_data, input_size, options);

    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return std::unexpected{result.error()};
    return std::move(result->rendered);
}

std::vector<std::uint8_t> image_to_rgba(const Image& img) {
    auto w = img.width();
    auto h = img.height();
    std::vector<std::uint8_t> rgba(w * h * 4);
    for (std::size_t i = 0; i < w * h; ++i) {
        auto y = i / w;
        auto x = i % w;
        auto srgb = color_space::linear_to_srgb(img[x, y]).clamped();
        auto base = i * 4;
        rgba[base + 0] = static_cast<std::uint8_t>(std::lround(srgb.r * 255.0f));
        rgba[base + 1] = static_cast<std::uint8_t>(std::lround(srgb.g * 255.0f));
        rgba[base + 2] = static_cast<std::uint8_t>(std::lround(srgb.b * 255.0f));
        rgba[base + 3] = 255;
    }
    return rgba;
}

} // namespace

ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options) {
    auto img = get_rendered(input_data, input_size, options);
    if (!img) return {{}, 0, 0, img.error().message};

    auto png = png_io::encode(*img);
    if (!png) return {{}, 0, 0, png.error().message};

    return {*std::move(png),
            static_cast<int>(img->width()),
            static_cast<int>(img->height()), ""};
}

ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options) {
    auto img = get_rendered(input_data, input_size, options);
    if (!img) return {{}, 0, 0, img.error().message};

    return {image_to_rgba(*img),
            static_cast<int>(img->width()),
            static_cast<int>(img->height()), ""};
}

ConvertResult convert_error_map_rgba(const std::uint8_t* input_data,
                                     std::size_t input_size,
                                     const Options& options) {
    if (is_charset_mode(options.mode)) {
        return {{}, 0, 0, "Error map not supported for charset modes"};
    }
    auto pr = run_pipeline(input_data, input_size, options);
    if (!pr) return {{}, 0, 0, pr.error().message};

    auto& src = pr->preprocessed;
    auto& rendered = pr->rendered;
    auto w = pr->params.screen_width;
    auto h = pr->params.screen_height;
    std::size_t pixel_stretch = vic2::is_double_wide(pr->mode) ? 2 : 1;

    std::vector<float> err(w * h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto s = color_space::linear_to_oklab(src[x, y]);
            auto r = color_space::linear_to_oklab(
                rendered[x * pixel_stretch, y]);
            float dL = s.L - r.L, da = s.a - r.a, db = s.b - r.b;
            err[y * w + x] = std::sqrt(dL * dL + da * da + db * db);
        }
    }

    // Top-5% threshold via nth_element.
    std::vector<float> sorted = err;
    auto cut = sorted.size() - sorted.size() / 20;
    if (cut >= sorted.size()) cut = sorted.size() - 1;
    std::nth_element(sorted.begin(),
                     sorted.begin() + static_cast<std::ptrdiff_t>(cut),
                     sorted.end());
    float threshold = sorted[cut];

    Image emap(w * pixel_stretch, h);
    constexpr float dim = 0.18f;
    constexpr Color3f hot{1.0f, 0.05f, 0.0f};
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto base = rendered[x * pixel_stretch, y];
            Color3f c{base.r * dim, base.g * dim, base.b * dim};
            if (err[y * w + x] >= threshold) c = hot;
            for (std::size_t s = 0; s < pixel_stretch; ++s)
                emap[x * pixel_stretch + s, y] = c;
        }
    }

    return {image_to_rgba(emap),
            static_cast<int>(emap.width()),
            static_cast<int>(emap.height()), ""};
}

ConvertResult convert_prg(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    if (is_charset_mode(options.mode)) {
        auto cmode = get_charset_mode(options.mode);
        bool mc_halve = (options.mode == "charset-mc");

        auto target_w = options.width > 0
            ? static_cast<std::size_t>(options.width) : std::size_t{320};
        auto target_h = options.height > 0
            ? static_cast<std::size_t>(options.height) : std::size_t{200};

        auto image = load_and_preprocess(input_data, input_size, options,
                                          target_w, target_h);
        if (!image) return {{}, 0, 0, image.error().message};

        if (mc_halve) {
            auto logical_w = image->width() / 2;
            auto scaled = scale::bicubic(*image, logical_w, image->height());
            if (!scaled) return {{}, 0, 0, scaled.error().message};
            image = std::move(scaled);
        }

        auto pal = palette::by_name(options.palette);
        if (pal.colors.empty()) pal = palette::by_name("colodore");

        auto ds = make_dither_settings(options);
        auto metric = parse_metric(options);
    auto result = charset::convert(*image, pal, cmode, ds, metric);
        if (!result) return {{}, 0, 0, result.error().message};

        auto prg_data = prg::charset_text(*result);
        if (!prg_data) return {{}, 0, 0, prg_data.error().message};

        auto preview = charset::render(*result, pal);
        return {std::move(prg_data->bytes),
                static_cast<int>(preview.width()),
                static_cast<int>(preview.height()), ""};
    }

    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto prg_data = prg::from_screen(result->screen);
    if (!prg_data) return {{}, 0, 0, prg_data.error().message};

    return {std::move(prg_data->bytes),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_koa(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    if (options.mode != "multicolor")
        return {{}, 0, 0, "Koala export only supports multicolor mode"};

    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto koa = prg::koala_raw(result->screen);
    if (!koa) return {{}, 0, 0, koa.error().message};

    return {std::move(koa->bytes),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_hir(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    if (options.mode != "hires")
        return {{}, 0, 0, "Art Studio export only supports hires mode"};

    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto hir = prg::hires_raw(result->screen);
    if (!hir) return {{}, 0, 0, hir.error().message};

    return {std::move(hir->bytes),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

// Generate sprite header from ScreenResult
std::string generate_sprite_header(const quantize::ScreenResult& screen,
                                    const vic2::ModeParams& params,
                                    std::string_view name) {
    std::ostringstream out;
    bool mc = (screen.mode == vic2::Mode::sprite_multicolor);
    auto cx = params.screen_width / params.cell_width;
    auto cy = params.screen_height / params.cell_height;
    auto count = screen.cells.size();

    out << "#pragma once\n\n";
    out << "// Generated by png2c64\n";
    out << std::format("// Mode: {} sprites\n", mc ? "multicolor" : "hires");
    out << std::format("// Grid: {}x{} ({} sprites)\n\n", cx, cy, count);

    out << std::format("#define {}_COLS {}\n", name, cx);
    out << std::format("#define {}_ROWS {}\n", name, cy);
    out << std::format("#define {}_COUNT {}\n", name, count);
    out << std::format("#define {}_BACKGROUND {}\n", name, screen.background_color);
    if (mc) {
        // For multicolor sprites, cell_colors[0]=bg, [1]=mc1(shared), [2]=mc2(shared), [3]=per-sprite
        // mc1 and mc2 are shared across all sprites — take from first cell
        if (!screen.cells.empty() && screen.cells[0].cell_colors.size() >= 3) {
            out << std::format("#define {}_MULTICOLOR0 {}\n", name, screen.cells[0].cell_colors[1]);
            out << std::format("#define {}_MULTICOLOR1 {}\n", name, screen.cells[0].cell_colors[2]);
        }
    }

    // Sprite data: 64 bytes per sprite (63 data + 1 padding)
    out << std::format("\nstatic const unsigned char {}_data[{}] = {{\n",
                       name, count * 64);
    for (std::size_t si = 0; si < count; ++si) {
        auto& cell = screen.cells[si];
        out << std::format("    // Sprite {}\n    ", si);

        // Encode 21 rows * 3 bytes = 63 bytes
        for (std::size_t row = 0; row < params.cell_height; ++row) {
            if (mc) {
                // 12 pixels per row, 2 bits each = 3 bytes
                for (std::size_t byteIdx = 0; byteIdx < 3; ++byteIdx) {
                    std::uint8_t byte = 0;
                    for (std::size_t bit = 0; bit < 4; ++bit) {
                        auto col = byteIdx * 4 + bit;
                        auto pi = row * params.cell_width + col;
                        if (pi < cell.pixel_indices.size())
                            byte |= static_cast<std::uint8_t>(
                                cell.pixel_indices[pi] << (6 - bit * 2));
                    }
                    out << std::format("0x{:02x},", byte);
                }
            } else {
                // 24 pixels per row, 1 bit each = 3 bytes
                for (std::size_t byteIdx = 0; byteIdx < 3; ++byteIdx) {
                    std::uint8_t byte = 0;
                    for (std::size_t bit = 0; bit < 8; ++bit) {
                        auto col = byteIdx * 8 + bit;
                        auto pi = row * params.cell_width + col;
                        if (pi < cell.pixel_indices.size() &&
                            cell.pixel_indices[pi] == 1)
                            byte |= static_cast<std::uint8_t>(0x80 >> bit);
                    }
                    out << std::format("0x{:02x},", byte);
                }
            }
        }
        // 1 byte padding
        out << "0x00";
        if (si < count - 1) out << ",";
        out << "\n";
    }
    out << "};\n";

    // Per-sprite colors
    out << std::format("\nstatic const unsigned char {}_colors[{}] = {{\n    ",
                       name, count);
    for (std::size_t si = 0; si < count; ++si) {
        auto& cc = screen.cells[si].cell_colors;
        // Per-sprite color: last entry in cell_colors
        auto color = cc.empty() ? 0 : cc.back();
        out << std::format("0x{:02x}", color);
        if (si < count - 1) out << ",";
    }
    out << "\n};\n";

    return out.str();
}

ConvertResult convert_header(const std::uint8_t* input_data,
                             std::size_t input_size,
                             const Options& options,
                             const std::string& name) {
    bool is_charset = is_charset_mode(options.mode);
    bool is_sprite = (options.mode == "sprite-hi" || options.mode == "sprite-mc");
    bool is_petscii = (options.mode == "petscii");

    if (!is_charset && !is_sprite && !is_petscii)
        return {{}, 0, 0, "Header export only for charset, sprite, and PETSCII modes"};

    if (is_charset) {
        auto cmode = get_charset_mode(options.mode);
        bool mc_halve = (options.mode == "charset-mc");

        auto target_w = options.width > 0
            ? static_cast<std::size_t>(options.width) : std::size_t{320};
        auto target_h = options.height > 0
            ? static_cast<std::size_t>(options.height) : std::size_t{200};

        auto image = load_and_preprocess(input_data, input_size, options,
                                          target_w, target_h);
        if (!image) return {{}, 0, 0, image.error().message};

        if (mc_halve) {
            auto logical_w = image->width() / 2;
            auto scaled = scale::bicubic(*image, logical_w, image->height());
            if (!scaled) return {{}, 0, 0, scaled.error().message};
            image = std::move(scaled);
        }

        auto pal = palette::by_name(options.palette);
        if (pal.colors.empty()) pal = palette::by_name("colodore");

        auto ds = make_dither_settings(options);
        auto metric = parse_metric(options);
    auto result = charset::convert(*image, pal, cmode, ds, metric);
        if (!result) return {{}, 0, 0, result.error().message};

        auto header_text = charset::generate_header(*result, name);
        std::vector<std::uint8_t> bytes(header_text.begin(), header_text.end());

        auto preview = charset::render(*result, pal);
        return {std::move(bytes),
                static_cast<int>(preview.width()),
                static_cast<int>(preview.height()), ""};
    }

    if (is_petscii) {
        auto pipe = run_pipeline(input_data, input_size, options);
        if (!pipe) return {{}, 0, 0, pipe.error().message};

        std::ostringstream out;
        out << "#pragma once\n\n";
        out << "// Generated by png2c64 — PETSCII mode\n\n";
        out << std::format("#define {}_COLS 40\n", name);
        out << std::format("#define {}_ROWS 25\n", name);
        out << std::format("#define {}_BACKGROUND {}\n\n", name,
                           pipe->screen.background_color);

        out << std::format("static const unsigned char {}_screen[1000] = {{\n    ",
                           name);
        for (std::size_t i = 0; i < pipe->screen.cells.size(); ++i) {
            out << std::format("0x{:02x}", pipe->screen.cells[i].char_index);
            if (i < 999) out << ",";
            if (i % 40 == 39 && i < 999) out << "\n    ";
        }
        out << "\n};\n\n";

        out << std::format("static const unsigned char {}_color[1000] = {{\n    ",
                           name);
        for (std::size_t i = 0; i < pipe->screen.cells.size(); ++i) {
            auto& cc = pipe->screen.cells[i].cell_colors;
            out << std::format("0x{:02x}", cc.size() > 1 ? cc[1] : 0);
            if (i < 999) out << ",";
            if (i % 40 == 39 && i < 999) out << "\n    ";
        }
        out << "\n};\n";

        auto text = out.str();
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        return {std::move(bytes),
                static_cast<int>(pipe->rendered.width()),
                static_cast<int>(pipe->rendered.height()), ""};
    }

    // Sprite mode
    auto pipe = run_pipeline(input_data, input_size, options);
    if (!pipe) return {{}, 0, 0, pipe.error().message};

    auto mode = parse_mode(options.mode);
    auto params = vic2::get_mode_params(mode);
    params.screen_width = options.width > 0
        ? static_cast<std::size_t>(options.width) : params.screen_width;
    params.screen_height = options.height > 0
        ? static_cast<std::size_t>(options.height) : params.screen_height;

    auto header_text = generate_sprite_header(pipe->screen, params, name);
    std::vector<std::uint8_t> bytes(header_text.begin(), header_text.end());

    return {std::move(bytes),
            static_cast<int>(pipe->rendered.width()),
            static_cast<int>(pipe->rendered.height()), ""};
}

} // namespace png2c64::api
