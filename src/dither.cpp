#include "dither.hpp"
#include "color_space.hpp"
#include "vic2.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <vector>

namespace png2c64::dither {

namespace {

// ===========================================================================
// Ordered dither matrices
// ===========================================================================

// Standard Bayer 4x4 (normalized to [-0.5, 0.5])
constexpr auto make_bayer4x4() noexcept {
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{ 0,  8,  2, 10}},
        {{12,  4, 14,  6}},
        {{ 3, 11,  1,  9}},
        {{15,  7, 13,  5}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

// Standard Bayer 8x8
constexpr auto make_bayer8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{ 0, 32,  8, 40,  2, 34, 10, 42}},
        {{48, 16, 56, 24, 50, 18, 58, 26}},
        {{12, 44,  4, 36, 14, 46,  6, 38}},
        {{60, 28, 52, 20, 62, 30, 54, 22}},
        {{ 3, 35, 11, 43,  1, 33,  9, 41}},
        {{51, 19, 59, 27, 49, 17, 57, 25}},
        {{15, 47,  7, 39, 13, 45,  5, 37}},
        {{63, 31, 55, 23, 61, 29, 53, 21}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}

// 2:1 Checkerboard -- 2 levels, simplest multicolor dither
// At 2:1 display this produces a brick-like pattern
constexpr auto make_checker() noexcept {
    std::array<std::array<float, 2>, 2> m{};
    m[0][0] = -0.25f; m[0][1] =  0.25f;
    m[1][0] =  0.25f; m[1][1] = -0.25f;
    return m;
}

// 2:1 Bayer 2x2 -- 4 levels, light ordered dither
constexpr auto make_bayer2x2() noexcept {
    // Standard 2x2 Bayer
    constexpr std::array<std::array<int, 2>, 2> raw = {{
        {{0, 2}},
        {{3, 1}},
    }};
    std::array<std::array<float, 2>, 2> m{};
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 4.0f - 0.5f;
    return m;
}

// 2:1 Horizontal-biased 2x4 Bayer -- 8 levels
// 2 columns x 4 rows: at 2:1 pixel ratio this tiles as a perceptually square
// 4x4 screen pixel block. More vertical variation compensates for wide pixels.
constexpr auto make_h2x4() noexcept {
    constexpr std::array<std::array<int, 2>, 4> raw = {{
        {{0, 4}},
        {{6, 2}},
        {{1, 5}},
        {{7, 3}},
    }};
    std::array<std::array<float, 2>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

// 2:1 Clustered dot 4x4 -- 16 levels
// Threshold values arranged so activated pixels form round dots when displayed
// at 2:1 pixel ratio. The cluster grows from center outward.
constexpr auto make_clustered_dot() noexcept {
    constexpr std::array<std::array<int, 4>, 4> raw = {{
        {{12,  5,  6, 13}},
        {{ 4,  0,  1,  7}},
        {{11,  3,  2,  8}},
        {{15, 10,  9, 14}},
    }};
    std::array<std::array<float, 4>, 4> m{};
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 16.0f - 0.5f;
    return m;
}

// Horizontal line 1x2 -- 2 levels, alternating light/dark rows
constexpr auto make_line2() noexcept {
    std::array<std::array<float, 1>, 2> m{};
    m[0][0] = -0.25f;
    m[1][0] =  0.25f;
    return m;
}

// Line-checker hybrid 2x2 -- rows have large threshold difference,
// columns have small offset. Produces horizontal lines with subtle
// pixel variation within each row. Between line2 and checker.
constexpr auto make_line_checker() noexcept {
    std::array<std::array<float, 2>, 2> m{};
    m[0][0] = -0.35f; m[0][1] = -0.15f;
    m[1][0] =  0.15f; m[1][1] =  0.35f;
    return m;
}

// Horizontal line 1x4 -- 4 levels, smooth vertical gradient
constexpr auto make_line4() noexcept {
    std::array<std::array<float, 1>, 4> m{};
    m[0][0] = -0.375f;
    m[1][0] = -0.125f;
    m[2][0] =  0.125f;
    m[3][0] =  0.375f;
    return m;
}

// Horizontal line 1x8 -- 8 levels, finest horizontal line gradient
constexpr auto make_line8() noexcept {
    constexpr std::array<int, 8> raw = {{0, 4, 2, 6, 1, 5, 3, 7}};
    std::array<std::array<float, 1>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        m[y][0] = (static_cast<float>(raw[y]) + 0.5f) / 8.0f - 0.5f;
    return m;
}

constexpr auto bayer4 = make_bayer4x4();
constexpr auto bayer8 = make_bayer8x8();
constexpr auto checker_mat = make_checker();
constexpr auto bayer2x2_mat = make_bayer2x2();
constexpr auto h2x4_mat = make_h2x4();
constexpr auto clustered_mat = make_clustered_dot();
constexpr auto line2_mat = make_line2();
constexpr auto line_checker_mat = make_line_checker();
constexpr auto line4_mat = make_line4();
constexpr auto line8_mat = make_line8();

// 45-degree halftone 8x8 (newspaper/print look, 32 gray levels)
constexpr auto make_halftone8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{13,  7,  8, 14, 17, 21, 22, 18}},
        {{ 6,  1,  3,  9, 28, 31, 29, 23}},
        {{ 5,  2,  4, 10, 27, 32, 30, 24}},
        {{16, 12, 11, 15, 20, 26, 25, 19}},
        {{17, 21, 22, 18, 13,  7,  8, 14}},
        {{28, 31, 29, 23,  6,  1,  3,  9}},
        {{27, 32, 30, 24,  5,  2,  4, 10}},
        {{20, 26, 25, 19, 16, 12, 11, 15}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) - 0.5f) / 32.0f - 0.5f;
    return m;
}
constexpr auto make_diagonal8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{24, 10, 12, 26, 35, 47, 49, 37}},
        {{ 8,  0,  2, 14, 45, 59, 61, 51}},
        {{22,  6,  4, 16, 43, 57, 63, 53}},
        {{30, 20, 18, 28, 33, 41, 55, 39}},
        {{34, 46, 48, 36, 25, 11, 13, 27}},
        {{44, 58, 60, 50,  9,  1,  3, 15}},
        {{42, 56, 62, 52, 23,  7,  5, 17}},
        {{32, 40, 54, 38, 31, 21, 19, 29}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 64.0f - 0.5f;
    return m;
}
constexpr auto make_spiral5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{20, 21, 22, 23, 24}},
        {{19,  6,  7,  8,  9}},
        {{18,  5,  0,  1, 10}},
        {{17,  4,  3,  2, 11}},
        {{16, 15, 14, 13, 12}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 25.0f - 0.5f;
    return m;
}
constexpr auto make_hex8x8() noexcept {
    constexpr std::array<std::array<int, 8>, 8> raw = {{
        {{3, 4, 2, 7, 1, 6, 0, 5}},
        {{6, 0, 5, 3, 4, 2, 7, 1}},
        {{2, 7, 1, 6, 0, 5, 3, 4}},
        {{5, 3, 4, 2, 7, 1, 6, 0}},
        {{1, 6, 0, 5, 3, 4, 2, 7}},
        {{4, 2, 7, 1, 6, 0, 5, 3}},
        {{0, 5, 3, 4, 2, 7, 1, 6}},
        {{7, 1, 6, 0, 5, 3, 4, 2}},
    }};
    std::array<std::array<float, 8>, 8> m{};
    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 8.0f - 0.5f;
    return m;
}
constexpr auto make_hex5x5() noexcept {
    constexpr std::array<std::array<int, 5>, 5> raw = {{
        {{4, 3, 0, 1, 2}},
        {{0, 1, 2, 4, 3}},
        {{2, 4, 3, 0, 1}},
        {{3, 0, 1, 2, 4}},
        {{1, 2, 4, 3, 0}},
    }};
    std::array<std::array<float, 5>, 5> m{};
    for (std::size_t y = 0; y < 5; ++y)
        for (std::size_t x = 0; x < 5; ++x)
            m[y][x] = (static_cast<float>(raw[y][x]) + 0.5f) / 5.0f - 0.5f;
    return m;
}
constexpr auto make_blue_noise64() noexcept {
    std::array<std::array<float, 64>, 64> m{};
    for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            auto fx = static_cast<float>(x);
            auto fy = static_cast<float>(y);
            float v = 52.9829189f * (0.06711056f * fx + 0.00583715f * fy);
            v = v - static_cast<float>(static_cast<int>(v));
            if (v < 0.0f) v += 1.0f;
            v = 52.9829189f * v;
            v = v - static_cast<float>(static_cast<int>(v));
            if (v < 0.0f) v += 1.0f;
            m[y][x] = v - 0.5f;
        }
    }
    return m;
}

constexpr auto halftone8x8_mat = make_halftone8x8();
constexpr auto diagonal8x8_mat = make_diagonal8x8();
constexpr auto spiral5x5_mat = make_spiral5x5();
constexpr auto hex8x8_mat = make_hex8x8();
constexpr auto hex5x5_mat = make_hex5x5();
constexpr auto blue_noise_mat = make_blue_noise64();

// ===========================================================================
// OKLab arithmetic helpers
// ===========================================================================

using OKLab = color_space::OKLab;

constexpr OKLab oklab_add(OKLab a, OKLab b) noexcept {
    return {a.L + b.L, a.a + b.a, a.b + b.b};
}

constexpr OKLab oklab_sub(OKLab a, OKLab b) noexcept {
    return {a.L - b.L, a.a - b.a, a.b - b.b};
}

constexpr OKLab oklab_scale(OKLab v, float s) noexcept {
    return {v.L * s, v.a * s, v.b * s};
}

constexpr OKLab oklab_clamp(OKLab e, float max_mag) noexcept {
    return {
        std::clamp(e.L, -max_mag, max_mag),
        std::clamp(e.a, -max_mag, max_mag),
        std::clamp(e.b, -max_mag, max_mag),
    };
}

void oklab_distribute(std::vector<OKLab>& buf, std::size_t idx,
                      OKLab error, float weight, float max_mag) {
    buf[idx] = oklab_clamp(oklab_add(buf[idx], oklab_scale(error, weight)),
                           max_mag);
}

// ===========================================================================
// Nearest-color search in OKLab (precomputed palette)
// ===========================================================================

struct NearestResult {
    std::uint8_t index;
    OKLab color_lab;
};

NearestResult find_nearest_oklab(
    OKLab pixel_lab,
    std::span<const std::uint8_t> cell_colors,
    std::span<const OKLab> palette_lab) {

    float best_dist = std::numeric_limits<float>::max();
    std::uint8_t best_idx = 0;
    OKLab best_lab{};

    for (std::size_t c = 0; c < cell_colors.size(); ++c) {
        auto& cl = palette_lab[cell_colors[c]];
        float dL = pixel_lab.L - cl.L;
        float da = pixel_lab.a - cl.a;
        float db = pixel_lab.b - cl.b;
        float dist = dL * dL + da * da + db * db;

        if (dist < best_dist) {
            best_dist = dist;
            best_idx = static_cast<std::uint8_t>(c);
            best_lab = cl;
        }
    }

    return {best_idx, best_lab};
}

std::vector<OKLab> precompute_palette_lab(const Palette& palette) {
    std::vector<OKLab> lab(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i) {
        lab[i] = color_space::linear_to_oklab(palette.colors[i]);
    }
    return lab;
}

// ===========================================================================
// Cell lookup
// ===========================================================================

struct PixelCell {
    quantize::CellResult* cell;
    std::size_t local_idx;
};

PixelCell get_pixel_cell(quantize::ScreenResult& result,
                         std::size_t x, std::size_t y,
                         const vic2::ModeParams& params, std::size_t cx) {
    auto cell_x = x / params.cell_width;
    auto cell_y = y / params.cell_height;
    auto cell_idx = cell_y * cx + cell_x;
    auto local_x = x % params.cell_width;
    auto local_y = y % params.cell_height;
    auto pi = local_y * params.cell_width + local_x;
    return {&result.cells[cell_idx], pi};
}

// ===========================================================================
// FLI per-row color extraction
// ===========================================================================

// Get the available colors for a specific row within a FLI/AFLI cell.
// Returns a small vector of palette indices.
std::vector<std::uint8_t> get_row_colors(
    const quantize::CellResult& cell, vic2::Mode mode, std::size_t local_y) {
    if (mode == vic2::Mode::fli) {
        // cell_colors = [bg, colorram, r0_hi, r0_lo, ..., r7_hi, r7_lo]
        return {cell.cell_colors[0],
                cell.cell_colors[2 + local_y * 2],
                cell.cell_colors[3 + local_y * 2],
                cell.cell_colors[1]};
    }
    if (mode == vic2::Mode::afli) {
        // cell_colors = [r0_c0, r0_c1, ..., r7_c0, r7_c1]
        return {cell.cell_colors[local_y * 2],
                cell.cell_colors[local_y * 2 + 1]};
    }
    // Standard modes: uniform colors
    return {cell.cell_colors.begin(), cell.cell_colors.end()};
}

// ===========================================================================
// Generic ordered dithering with arbitrary matrix dimensions
// ===========================================================================

template <std::size_t W, std::size_t H>
void apply_ordered_matrix(const Image& image, quantize::ScreenResult& result,
                          const std::array<std::array<float, W>, H>& matrix,
                          std::span<const OKLab> palette_lab,
                          const vic2::ModeParams& params, float strength) {
    auto cx = params.screen_width / params.cell_width;

    for (std::size_t cell_idx = 0; cell_idx < result.cells.size();
         ++cell_idx) {
        auto& cell = result.cells[cell_idx];
        auto cell_x = cell_idx % cx;
        auto cell_y = cell_idx / cx;
        auto px = cell_x * params.cell_width;
        auto py = cell_y * params.cell_height;

        bool fli = vic2::is_fli_mode(result.mode);
        std::size_t pi = 0;
        for (std::size_t dy = 0; dy < params.cell_height; ++dy) {
            auto row_colors = fli
                ? get_row_colors(cell, result.mode, dy)
                : std::vector<std::uint8_t>(cell.cell_colors.begin(),
                                             cell.cell_colors.end());
            for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
                auto pixel_lab =
                    color_space::linear_to_oklab(image[px + dx, py + dy]);

                float threshold = matrix[(py + dy) % H][(px + dx) % W];

                pixel_lab.L += threshold * strength * 0.15f;
                pixel_lab.a += threshold * strength * 0.03f;
                pixel_lab.b += threshold * strength * 0.03f;

                auto [idx, chosen] = find_nearest_oklab(
                    pixel_lab, row_colors, palette_lab);
                cell.pixel_indices[pi] = idx;
                ++pi;
            }
        }
    }
}

// ===========================================================================
// Error diffusion (all in OKLab space)
// ===========================================================================

struct DiffusionEntry {
    int dx;
    int dy;
    float weight;
};

// Compute per-pixel local contrast in OKLab L channel.
// Returns values in [0, 1] where 0 = flat, 1 = high contrast.
std::vector<float> compute_contrast_map(
    const std::vector<OKLab>& image_lab,
    std::size_t w, std::size_t h) {

    constexpr float threshold = 0.15f; // L difference for "high contrast"
    std::vector<float> contrast(w * h);

    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            float L = image_lab[y * w + x].L;
            float grad = 0.0f;
            float count = 0.0f;

            if (x > 0)     { grad += std::abs(L - image_lab[y*w + x-1].L); ++count; }
            if (x+1 < w)   { grad += std::abs(L - image_lab[y*w + x+1].L); ++count; }
            if (y > 0)     { grad += std::abs(L - image_lab[(y-1)*w + x].L); ++count; }
            if (y+1 < h)   { grad += std::abs(L - image_lab[(y+1)*w + x].L); ++count; }

            grad /= count;
            contrast[y * w + x] = std::min(grad / threshold, 1.0f);
        }
    }

    return contrast;
}

void apply_error_diffusion(
    const Image& image, quantize::ScreenResult& result,
    const Palette& palette, const vic2::ModeParams& params,
    float strength, float error_clamp_val,
    bool serpentine, float adaptive,
    std::span<const DiffusionEntry> kernel) {

    auto w = params.screen_width;
    auto h = params.screen_height;
    auto cx = params.screen_width / params.cell_width;

    std::vector<OKLab> image_lab(w * h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            image_lab[y * w + x] = color_space::linear_to_oklab(image[x, y]);
        }
    }

    auto palette_lab = precompute_palette_lab(palette);

    // Precompute contrast map for adaptive diffusion
    std::vector<float> contrast_map;
    if (adaptive > 0.0f) {
        contrast_map = compute_contrast_map(image_lab, w, h);
    }

    std::vector<OKLab> error_buf(w * h);

    for (std::size_t y = 0; y < h; ++y) {
        bool reverse = serpentine && (y % 2 == 1);

        for (std::size_t step = 0; step < w; ++step) {
            std::size_t x = reverse ? (w - 1 - step) : step;
            auto buf_idx = y * w + x;

            auto [cell, pi] = get_pixel_cell(result, x, y, params, cx);

            // Adaptive: scale incoming error by inverse local contrast.
            // High detail → less error accepted → sharper.
            // Flat areas → full error → smoother gradients.
            auto clamped_error = oklab_clamp(error_buf[buf_idx], error_clamp_val);
            if (adaptive > 0.0f) {
                float detail = contrast_map[buf_idx];
                float scale = 1.0f - adaptive * detail;
                clamped_error = oklab_scale(clamped_error, scale);
            }

            auto adjusted = oklab_add(image_lab[buf_idx], clamped_error);

            // FLI: use row-specific colors
            auto local_y = (y % params.cell_height);
            auto colors = vic2::is_fli_mode(result.mode)
                ? get_row_colors(*cell, result.mode, local_y)
                : std::vector<std::uint8_t>(cell->cell_colors.begin(),
                                             cell->cell_colors.end());

            auto [idx, chosen_lab] = find_nearest_oklab(
                adjusted, colors, palette_lab);
            cell->pixel_indices[pi] = idx;

            auto quant_error =
                oklab_scale(oklab_sub(adjusted, chosen_lab), strength);

            for (auto& [kdx, kdy, weight] : kernel) {
                int actual_dx = reverse ? -kdx : kdx;
                auto nx = static_cast<int>(x) + actual_dx;
                auto ny = static_cast<int>(y) + kdy;

                if (nx >= 0 && static_cast<std::size_t>(nx) < w &&
                    ny >= 0 && static_cast<std::size_t>(ny) < h) {
                    auto nidx = static_cast<std::size_t>(ny) * w +
                                static_cast<std::size_t>(nx);
                    oklab_distribute(error_buf, nidx, quant_error, weight,
                                    error_clamp_val);
                }
            }
        }
    }
}

// ===========================================================================
// Error diffusion kernels
// ===========================================================================

// Floyd-Steinberg (standard, square pixel)
//        * 7/16
//  3/16 5/16 1/16
constexpr std::array floyd_steinberg_kernel = {
    DiffusionEntry{ 1, 0, 7.0f / 16.0f},
    DiffusionEntry{-1, 1, 3.0f / 16.0f},
    DiffusionEntry{ 0, 1, 5.0f / 16.0f},
    DiffusionEntry{ 1, 1, 1.0f / 16.0f},
};

// Atkinson: distributes only 75% of error (6/8), cleaner for limited palettes
//      * 1/8 1/8
//  1/8 1/8 1/8
//      1/8
constexpr std::array atkinson_kernel = {
    DiffusionEntry{ 1, 0, 1.0f / 8.0f},
    DiffusionEntry{ 2, 0, 1.0f / 8.0f},
    DiffusionEntry{-1, 1, 1.0f / 8.0f},
    DiffusionEntry{ 0, 1, 1.0f / 8.0f},
    DiffusionEntry{ 1, 1, 1.0f / 8.0f},
    DiffusionEntry{ 0, 2, 1.0f / 8.0f},
};

// Sierra Lite
//    * 2/4
//  1/4 1/4
constexpr std::array sierra_lite_kernel = {
    DiffusionEntry{ 1, 0, 2.0f / 4.0f},
    DiffusionEntry{-1, 1, 1.0f / 4.0f},
    DiffusionEntry{ 0, 1, 1.0f / 4.0f},
};

// Floyd-Steinberg adjusted for 2:1 pixel ratio.
// Horizontal neighbor is 2 screen pixels away, vertical is 1.
// Shift weight from horizontal to vertical to equalize screen-space diffusion.
//          *  5/16
//  3/16  6/16  2/16
constexpr std::array fs_wide_kernel = {
    DiffusionEntry{ 1, 0, 5.0f / 16.0f},
    DiffusionEntry{-1, 1, 3.0f / 16.0f},
    DiffusionEntry{ 0, 1, 6.0f / 16.0f},
    DiffusionEntry{ 1, 1, 2.0f / 16.0f},
};

// Jarvis-Judice-Ninke: wider 5x3 kernel, distributes error over more pixels.
// The larger reach naturally handles 2:1 ratio better -- more vertical taps
// means the vertical diffusion has finer granularity.
//              *   7/48  5/48
//  3/48  5/48  7/48  5/48  3/48
//  1/48  3/48  5/48  3/48  1/48
constexpr std::array jarvis_kernel = {
    // Row 0 (current row, ahead of cursor)
    DiffusionEntry{ 1, 0, 7.0f / 48.0f},
    DiffusionEntry{ 2, 0, 5.0f / 48.0f},
    // Row 1
    DiffusionEntry{-2, 1, 3.0f / 48.0f},
    DiffusionEntry{-1, 1, 5.0f / 48.0f},
    DiffusionEntry{ 0, 1, 7.0f / 48.0f},
    DiffusionEntry{ 1, 1, 5.0f / 48.0f},
    DiffusionEntry{ 2, 1, 3.0f / 48.0f},
    // Row 2
    DiffusionEntry{-2, 2, 1.0f / 48.0f},
    DiffusionEntry{-1, 2, 3.0f / 48.0f},
    DiffusionEntry{ 0, 2, 5.0f / 48.0f},
    DiffusionEntry{ 1, 2, 3.0f / 48.0f},
    DiffusionEntry{ 2, 2, 1.0f / 48.0f},
};

// Line-biased Floyd-Steinberg: no horizontal neighbor, all error goes downward.
// Creates horizontal line coherence — adjacent pixels in the same row are
// dithered independently, only receiving error from the row above.
//          *
//  2/8   3/8   2/8
//        1/8
constexpr std::array line_fs_kernel = {
    DiffusionEntry{-1, 1, 2.0f / 8.0f},
    DiffusionEntry{ 0, 1, 3.0f / 8.0f},
    DiffusionEntry{ 1, 1, 2.0f / 8.0f},
    DiffusionEntry{ 0, 2, 1.0f / 8.0f},
};

} // namespace

void apply(const Image& image, quantize::ScreenResult& result,
           const Palette& palette, const vic2::ModeParams& params,
           const Settings& settings) {

    // PETSCII: pixel patterns are fixed by character ROM, skip dithering
    if (result.mode == vic2::Mode::petscii) return;

    auto palette_lab = precompute_palette_lab(palette);

    switch (settings.method) {
    case Method::none:
        return;

    // Square-pixel ordered
    case Method::bayer4x4:
        apply_ordered_matrix(image, result, bayer4, palette_lab,
                             params, settings.strength);
        return;
    case Method::bayer8x8:
        apply_ordered_matrix(image, result, bayer8, palette_lab,
                             params, settings.strength);
        return;

    // 2:1 ordered
    case Method::checker:
        apply_ordered_matrix(image, result, checker_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::bayer2x2:
        apply_ordered_matrix(image, result, bayer2x2_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::h2x4:
        apply_ordered_matrix(image, result, h2x4_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::clustered_dot:
        apply_ordered_matrix(image, result, clustered_mat, palette_lab,
                             params, settings.strength);
        return;

    // Horizontal line ordered
    case Method::line2:
        apply_ordered_matrix(image, result, line2_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::line_checker:
        apply_ordered_matrix(image, result, line_checker_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::line4:
        apply_ordered_matrix(image, result, line4_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::line8:
        apply_ordered_matrix(image, result, line8_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::halftone8x8:
        apply_ordered_matrix(image, result, halftone8x8_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::diagonal8x8:
        apply_ordered_matrix(image, result, diagonal8x8_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::spiral5x5:
        apply_ordered_matrix(image, result, spiral5x5_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::hex8x8:
        apply_ordered_matrix(image, result, hex8x8_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::hex5x5:
        apply_ordered_matrix(image, result, hex5x5_mat, palette_lab,
                             params, settings.strength);
        return;
    case Method::blue_noise:
        apply_ordered_matrix(image, result, blue_noise_mat, palette_lab,
                             params, settings.strength);
        return;

    // Square-pixel error diffusion
    case Method::floyd_steinberg:
        apply_error_diffusion(image, result, palette, params,
                              settings.strength, settings.error_clamp,
                              settings.serpentine, settings.adaptive,
                              floyd_steinberg_kernel);
        return;
    case Method::atkinson:
        apply_error_diffusion(image, result, palette, params,
                              settings.strength, settings.error_clamp,
                              settings.serpentine, settings.adaptive,
                              atkinson_kernel);
        return;
    case Method::sierra_lite:
        apply_error_diffusion(image, result, palette, params,
                              settings.strength, settings.error_clamp,
                              settings.serpentine, settings.adaptive,
                              sierra_lite_kernel);
        return;

    // 2:1 error diffusion
    case Method::fs_wide:
        apply_error_diffusion(image, result, palette, params,
                              settings.strength, settings.error_clamp,
                              settings.serpentine, settings.adaptive,
                              fs_wide_kernel);
        return;
    case Method::jarvis:
        apply_error_diffusion(image, result, palette, params,
                              settings.strength, settings.error_clamp,
                              settings.serpentine, settings.adaptive,
                              jarvis_kernel);
        return;

    // Horizontal line error diffusion
    case Method::line_fs:
        apply_error_diffusion(image, result, palette, params,
                              settings.strength, settings.error_clamp,
                              settings.serpentine, settings.adaptive,
                              line_fs_kernel);
        return;
    }
}

float ordered_threshold(Method method, std::size_t x, std::size_t y) {
    switch (method) {
    case Method::bayer4x4:      return bayer4[y % 4][x % 4];
    case Method::bayer8x8:      return bayer8[y % 8][x % 8];
    case Method::checker:       return checker_mat[y % 2][x % 2];
    case Method::bayer2x2:      return bayer2x2_mat[y % 2][x % 2];
    case Method::h2x4:          return h2x4_mat[y % 4][x % 2];
    case Method::clustered_dot: return clustered_mat[y % 4][x % 4];
    case Method::line2:         return line2_mat[y % 2][0];
    case Method::line_checker:  return line_checker_mat[y % 2][x % 2];
    case Method::line4:         return line4_mat[y % 4][0];
    case Method::line8:         return line8_mat[y % 8][0];
    case Method::halftone8x8:  return halftone8x8_mat[y % 8][x % 8];
    case Method::diagonal8x8:  return diagonal8x8_mat[y % 8][x % 8];
    case Method::spiral5x5:    return spiral5x5_mat[y % 5][x % 5];
    case Method::hex8x8:       return hex8x8_mat[y % 8][x % 8];
    case Method::hex5x5:       return hex5x5_mat[y % 5][x % 5];
    case Method::blue_noise:   return blue_noise_mat[y % 64][x % 64];
    default:                    return 0.0f;
    }
}

} // namespace png2c64::dither
