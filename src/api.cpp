#include "api.hpp"
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

namespace png2c64::api {

namespace {

vic2::Mode parse_mode(const std::string& s) {
    if (s == "hires") return vic2::Mode::hires;
    if (s == "multicolor") return vic2::Mode::multicolor;
    if (s == "sprite-hi") return vic2::Mode::sprite_hires;
    if (s == "sprite-mc") return vic2::Mode::sprite_multicolor;
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
    return dither::Method::checker;
}

struct PipelineResult {
    Image rendered;
    quantize::ScreenResult screen;
};

// Core pipeline: load -> scale -> preprocess -> quantize -> dither -> render
Result<PipelineResult> run_pipeline(const std::uint8_t* input_data,
                                    std::size_t input_size,
                                    const Options& options) {
    // Load from memory via stb_image
    int w{}, h{}, channels{};
    auto* raw = stbi_load_from_memory(input_data,
        static_cast<int>(input_size), &w, &h, &channels, 3);
    if (!raw) {
        return std::unexpected{Error{ErrorCode::invalid_png, "Failed to decode image"}};
    }

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

    // Mode params — for sprites, width/height define the sheet dimensions
    auto mode = parse_mode(options.mode);
    auto params = vic2::get_mode_params(mode);

    auto target_w = options.width > 0
        ? static_cast<std::size_t>(options.width) : params.screen_width;
    auto target_h = options.height > 0
        ? static_cast<std::size_t>(options.height) : params.screen_height;

    // Update params to match actual target (critical for sprites/charsets)
    params.screen_width = target_w;
    params.screen_height = target_h;

    // Scale
    if (image.width() != target_w || image.height() != target_h) {
        auto scaled = scale::bicubic(image, target_w, target_h);
        if (!scaled) return std::unexpected{scaled.error()};
        image = *std::move(scaled);
    }

    // Preprocess
    preprocess::Settings pp;
    pp.gamma = options.gamma;
    pp.brightness = options.brightness;
    pp.contrast = options.contrast;
    pp.saturation = options.saturation;
    preprocess::apply(image, pp);

    auto pal = palette::by_name(options.palette);
    if (pal.colors.empty())
        pal = palette::by_name("pepto");

    preprocess::match_palette_range(image, pal);

    // Dither settings
    dither::Settings ds;
    ds.method = parse_dither(options.dither);
    ds.strength = options.dither_strength;
    ds.error_clamp = options.error_clamp;
    ds.serpentine = options.serpentine;
    ds.adaptive = options.adaptive;

    // Dither-aware quantization
    quantize::ThresholdFn tfn;
    if (dither::is_ordered(ds.method) && ds.method != dither::Method::none) {
        auto m = ds.method;
        auto s = ds.strength;
        tfn = [m, s](std::size_t x, std::size_t y) {
            return dither::ordered_threshold(m, x, y) * s;
        };
    }

    auto screen = quantize::quantize(image, pal, mode, params, tfn, ds.strength);
    if (!screen) return std::unexpected{screen.error()};

    if (ds.method != dither::Method::none)
        dither::apply(image, *screen, pal, params, ds);

    // Render
    auto out_w = params.screen_width;
    auto out_h = params.screen_height;
    auto cx = vic2::cells_x(params);
    std::size_t pixel_stretch = vic2::is_double_wide(mode) ? 2 : 1;
    Image output(out_w * pixel_stretch, out_h);

    for (std::size_t ci = 0; ci < screen->cells.size(); ++ci) {
        auto& cell = screen->cells[ci];
        auto cell_x = ci % cx;
        auto cell_y = ci / cx;
        auto px = cell_x * params.cell_width * pixel_stretch;
        auto py = cell_y * params.cell_height;

        std::size_t pi = 0;
        for (std::size_t dy = 0; dy < params.cell_height; ++dy) {
            for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
                auto color_idx = cell.cell_colors[cell.pixel_indices[pi]];
                auto color = pal.colors[color_idx];
                auto out_x = px + dx * pixel_stretch;
                for (std::size_t s = 0; s < pixel_stretch; ++s)
                    output[out_x + s, py + dy] = color;
                ++pi;
            }
        }
    }

    return PipelineResult{std::move(output), *std::move(screen)};
}

} // namespace

ConvertResult convert(const std::uint8_t* input_data, std::size_t input_size,
                      const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto png = png_io::encode(result->rendered);
    if (!png) return {{}, 0, 0, png.error().message};

    return {*std::move(png),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

ConvertResult convert_rgba(const std::uint8_t* input_data,
                           std::size_t input_size,
                           const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto& img = result->rendered;
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

    return {std::move(rgba), static_cast<int>(w), static_cast<int>(h), ""};
}

ConvertResult convert_prg(const std::uint8_t* input_data,
                          std::size_t input_size,
                          const Options& options) {
    auto result = run_pipeline(input_data, input_size, options);
    if (!result) return {{}, 0, 0, result.error().message};

    auto prg_data = prg::from_screen(result->screen);
    if (!prg_data) return {{}, 0, 0, prg_data.error().message};

    return {std::move(prg_data->bytes),
            static_cast<int>(result->rendered.width()),
            static_cast<int>(result->rendered.height()), ""};
}

} // namespace png2c64::api
