#include "quantize.hpp"
#include "blur_util.hpp"
#include "color_space.hpp"
#include "petscii_rom.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <format>
#include <limits>
#include <thread>
#include <vector>

namespace png2c64::quantize {

namespace {

auto precompute_oklab(const Palette& palette) {
    std::vector<color_space::OKLab> lab(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i) {
        lab[i] = color_space::linear_to_oklab(palette.colors[i]);
    }
    return lab;
}

// ---------------------------------------------------------------------------
// Precomputed per-cell distance table.
// dist[palette_color][pixel] = squared OKLab distance.
// Color-major layout for cache-friendly vectorized inner loops.
// ---------------------------------------------------------------------------

// Max pixels per cell: sprite hires = 24x21 = 504
static constexpr std::size_t max_pixels_per_cell = 504;
static constexpr std::size_t max_palette = 16;

struct CellDistTable {
    std::array<std::array<float, max_pixels_per_cell>, max_palette> dist{};
    std::size_t num_pixels{};
};

CellDistTable precompute_cell_dist(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& threshold_fn = {},
    float threshold_strength = 0.0f) {

    CellDistTable table;
    table.num_pixels = params.cell_width * params.cell_height;
    auto px = cell_x * params.cell_width;
    auto py = cell_y * params.cell_height;
    auto n_colors = palette_lab.size();
    bool has_threshold = static_cast<bool>(threshold_fn);

    std::size_t pi = 0;
    for (std::size_t dy = 0; dy < params.cell_height; ++dy) {
        for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
            auto pixel_lab =
                color_space::linear_to_oklab(image[px + dx, py + dy]);

            // Dither-aware: bias pixel by the ordered threshold so the
            // quantizer picks colors that dither well together.
            if (has_threshold) {
                float t = threshold_fn(px + dx, py + dy) * threshold_strength;
                pixel_lab.L += t * 0.15f;
                pixel_lab.a += t * 0.03f;
                pixel_lab.b += t * 0.03f;
            }

            for (std::size_t c = 0; c < n_colors; ++c) {
                auto& cl = palette_lab[c];
                float dL = pixel_lab.L - cl.L;
                float da = pixel_lab.a - cl.a;
                float db = pixel_lab.b - cl.b;
                table.dist[c][pi] = dL * dL + da * da + db * db;
            }
            ++pi;
        }
    }

    return table;
}

// ---------------------------------------------------------------------------
// Fast error-only evaluation from precomputed table.
// These tight loops auto-vectorize to NEON/SSE by the compiler.
// ---------------------------------------------------------------------------

// Min-of-2 across all pixels (hires)
float cell_error_2(const CellDistTable& table,
                   std::uint8_t c0, std::uint8_t c1) {
    float error = 0.0f;
    const float* __restrict d0 = table.dist[c0].data();
    const float* __restrict d1 = table.dist[c1].data();
    for (std::size_t p = 0; p < table.num_pixels; ++p) {
        error += std::min(d0[p], d1[p]);
    }
    return error;
}

// Min-of-4 across all pixels (multicolor)
float cell_error_4(const CellDistTable& table,
                   std::uint8_t c0, std::uint8_t c1,
                   std::uint8_t c2, std::uint8_t c3) {
    float error = 0.0f;
    const float* __restrict d0 = table.dist[c0].data();
    const float* __restrict d1 = table.dist[c1].data();
    const float* __restrict d2 = table.dist[c2].data();
    const float* __restrict d3 = table.dist[c3].data();
    for (std::size_t p = 0; p < table.num_pixels; ++p) {
        error += std::min(std::min(d0[p], d1[p]), std::min(d2[p], d3[p]));
    }
    return error;
}

// Full evaluation with pixel assignments (used once for the winning combination)
struct CellEval {
    float error{};
    std::vector<std::uint8_t> assignments;
};

CellEval evaluate_cell_full(
    const CellDistTable& table,
    std::span<const std::uint8_t> color_indices) {

    CellEval result;
    result.assignments.reserve(table.num_pixels);
    result.error = 0.0f;

    for (std::size_t p = 0; p < table.num_pixels; ++p) {
        float best_dist = std::numeric_limits<float>::max();
        std::uint8_t best_idx = 0;
        for (std::size_t c = 0; c < color_indices.size(); ++c) {
            float d = table.dist[color_indices[c]][p];
            if (d < best_dist) {
                best_dist = d;
                best_idx = static_cast<std::uint8_t>(c);
            }
        }
        result.assignments.push_back(best_idx);
        result.error += best_dist;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Hires: brute force C(16,2) = 120 pairs using precomputed table
// ---------------------------------------------------------------------------

CellResult quantize_cell_hires(
    const CellDistTable& table, const Palette& palette) {

    auto n = static_cast<std::uint8_t>(palette.size());
    float best_error = std::numeric_limits<float>::max();
    std::uint8_t best_i = 0, best_j = 1;

    for (std::uint8_t i = 0; i < n; ++i) {
        for (std::uint8_t j = i + 1; j < n; ++j) {
            float err = cell_error_2(table, i, j);
            if (err < best_error) {
                best_error = err;
                best_i = i;
                best_j = j;
            }
        }
    }

    std::array<std::uint8_t, 2> indices = {best_i, best_j};
    auto eval = evaluate_cell_full(table, indices);

    CellResult result;
    result.pixel_indices = std::move(eval.assignments);
    result.cell_colors = {best_i, best_j};
    result.error = eval.error;
    return result;
}

// ---------------------------------------------------------------------------
// Multicolor: brute force C(15,3) = 455 triples using precomputed table
// ---------------------------------------------------------------------------

CellResult quantize_cell_multicolor(
    const CellDistTable& table, std::uint8_t bg_color,
    const Palette& palette) {

    auto n = static_cast<std::uint8_t>(palette.size());
    float best_error = std::numeric_limits<float>::max();
    std::uint8_t best_i = 0, best_j = 0, best_k = 0;

    for (std::uint8_t i = 0; i < n; ++i) {
        if (i == bg_color) continue;
        for (std::uint8_t j = i + 1; j < n; ++j) {
            if (j == bg_color) continue;
            for (std::uint8_t k = j + 1; k < n; ++k) {
                if (k == bg_color) continue;

                float err = cell_error_4(table, bg_color, i, j, k);
                if (err < best_error) {
                    best_error = err;
                    best_i = i;
                    best_j = j;
                    best_k = k;
                }
            }
        }
    }

    std::array<std::uint8_t, 4> indices = {bg_color, best_i, best_j, best_k};
    auto eval = evaluate_cell_full(table, indices);

    CellResult result;
    result.pixel_indices = std::move(eval.assignments);
    result.cell_colors = {bg_color, best_i, best_j, best_k};
    result.error = eval.error;
    return result;
}

// ---------------------------------------------------------------------------
// FLI multicolor: per-row screen colors + shared colorram
// cell_colors = [bg, colorram, r0_hi, r0_lo, ..., r7_hi, r7_lo] (18 entries)
// ---------------------------------------------------------------------------

// Error for a single row (4 pixels) with 4 colors
float row_error_4(const CellDistTable& table, std::size_t row_start,
                  std::size_t row_pixels,
                  std::uint8_t c0, std::uint8_t c1,
                  std::uint8_t c2, std::uint8_t c3) {
    float error = 0.0f;
    for (std::size_t p = 0; p < row_pixels; ++p) {
        auto pi = row_start + p;
        error += std::min(std::min(table.dist[c0][pi], table.dist[c1][pi]),
                          std::min(table.dist[c2][pi], table.dist[c3][pi]));
    }
    return error;
}

// Error for a single row (8 pixels) with 2 colors
float row_error_2(const CellDistTable& table, std::size_t row_start,
                  std::size_t row_pixels,
                  std::uint8_t c0, std::uint8_t c1) {
    float error = 0.0f;
    for (std::size_t p = 0; p < row_pixels; ++p) {
        auto pi = row_start + p;
        error += std::min(table.dist[c0][pi], table.dist[c1][pi]);
    }
    return error;
}

CellResult quantize_cell_fli(
    const CellDistTable& table, std::uint8_t bg_color,
    const Palette& palette) {

    auto n = static_cast<std::uint8_t>(palette.size());
    constexpr std::size_t cell_w = 4;
    constexpr std::size_t cell_h = 8;

    float best_error = std::numeric_limits<float>::max();
    std::uint8_t best_colorram = 0;
    std::array<std::uint8_t, 16> best_screen{}; // [r0_hi, r0_lo, ..., r7_hi, r7_lo]

    for (std::uint8_t cr = 0; cr < n; ++cr) {
        if (cr == bg_color) continue;

        float total = 0.0f;
        std::array<std::uint8_t, 16> screen{};

        for (std::size_t row = 0; row < cell_h; ++row) {
            auto row_start = row * cell_w;
            float best_row = std::numeric_limits<float>::max();
            std::uint8_t best_hi = 0, best_lo = 0;

            for (std::uint8_t i = 0; i < n; ++i) {
                if (i == bg_color || i == cr) continue;
                for (std::uint8_t j = i + 1; j < n; ++j) {
                    if (j == bg_color || j == cr) continue;

                    float err = row_error_4(table, row_start, cell_w,
                                             bg_color, i, j, cr);
                    if (err < best_row) {
                        best_row = err;
                        best_hi = i;
                        best_lo = j;
                    }
                }
            }

            screen[row * 2] = best_hi;
            screen[row * 2 + 1] = best_lo;
            total += best_row;
        }

        if (total < best_error) {
            best_error = total;
            best_colorram = cr;
            best_screen = screen;
        }
    }

    // Build CellResult with per-row colors
    // cell_colors = [bg, colorram, r0_hi, r0_lo, ..., r7_hi, r7_lo]
    CellResult result;
    result.cell_colors.resize(18);
    result.cell_colors[0] = bg_color;
    result.cell_colors[1] = best_colorram;
    for (std::size_t i = 0; i < 16; ++i)
        result.cell_colors[2 + i] = best_screen[i];

    // Assign pixels: for each row, find nearest of the 4 row-specific colors
    result.pixel_indices.resize(cell_w * cell_h);
    result.error = 0.0f;
    for (std::size_t row = 0; row < cell_h; ++row) {
        std::array<std::uint8_t, 4> row_colors = {
            bg_color, best_screen[row * 2], best_screen[row * 2 + 1], best_colorram
        };
        for (std::size_t col = 0; col < cell_w; ++col) {
            auto pi = row * cell_w + col;
            float best_d = std::numeric_limits<float>::max();
            std::uint8_t best_idx = 0;
            for (std::uint8_t c = 0; c < 4; ++c) {
                float d = table.dist[row_colors[c]][pi];
                if (d < best_d) { best_d = d; best_idx = c; }
            }
            result.pixel_indices[pi] = best_idx;
            result.error += best_d;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// AFLI hires: per-row 2 colors, no shared background or colorram
// cell_colors = [r0_c0, r0_c1, r1_c0, r1_c1, ..., r7_c0, r7_c1] (16 entries)
// ---------------------------------------------------------------------------

CellResult quantize_cell_afli(
    const CellDistTable& table, const Palette& palette) {

    auto n = static_cast<std::uint8_t>(palette.size());
    constexpr std::size_t cell_w = 8;
    constexpr std::size_t cell_h = 8;

    CellResult result;
    result.cell_colors.resize(16);
    result.pixel_indices.resize(cell_w * cell_h);
    result.error = 0.0f;

    for (std::size_t row = 0; row < cell_h; ++row) {
        auto row_start = row * cell_w;
        float best_row = std::numeric_limits<float>::max();
        std::uint8_t best_c0 = 0, best_c1 = 1;

        for (std::uint8_t i = 0; i < n; ++i) {
            for (std::uint8_t j = i + 1; j < n; ++j) {
                float err = row_error_2(table, row_start, cell_w, i, j);
                if (err < best_row) {
                    best_row = err;
                    best_c0 = i;
                    best_c1 = j;
                }
            }
        }

        result.cell_colors[row * 2] = best_c0;
        result.cell_colors[row * 2 + 1] = best_c1;

        // Assign pixels for this row
        for (std::size_t col = 0; col < cell_w; ++col) {
            auto pi = row_start + col;
            if (table.dist[best_c1][pi] < table.dist[best_c0][pi]) {
                result.pixel_indices[pi] = 1;
                result.error += table.dist[best_c1][pi];
            } else {
                result.pixel_indices[pi] = 0;
                result.error += table.dist[best_c0][pi];
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Parallel cell processing
// ---------------------------------------------------------------------------

unsigned hw_threads() {
    auto n = std::thread::hardware_concurrency();
    return n > 0 ? n : 4;
}

template <typename Func>
void parallel_for_cells(std::size_t cx, std::size_t cy, Func&& func) {
    auto total = cx * cy;
    std::atomic<std::size_t> next_cell{0};

    auto worker = [&] {
        while (true) {
            auto idx = next_cell.fetch_add(1, std::memory_order_relaxed);
            if (idx >= total) break;
            auto x = idx % cx;
            auto y = idx / cx;
            func(idx, x, y);
        }
    };

    auto num_threads = std::min(static_cast<std::size_t>(hw_threads()), total);
    std::vector<std::jthread> threads;
    threads.reserve(num_threads - 1);

    for (std::size_t t = 1; t < num_threads; ++t) {
        threads.emplace_back(worker);
    }
    worker();
}

// ---------------------------------------------------------------------------
// Background color selection
// ---------------------------------------------------------------------------

std::uint8_t find_most_common_color(
    const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab) {

    auto n = palette.size();
    std::vector<std::size_t> counts(n, 0);

    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            auto pixel_lab = color_space::linear_to_oklab(image[x, y]);

            float best_dist = std::numeric_limits<float>::max();
            std::size_t best_idx = 0;

            for (std::size_t c = 0; c < n; ++c) {
                auto& cl = palette_lab[c];
                float dL = pixel_lab.L - cl.L;
                float da = pixel_lab.a - cl.a;
                float db = pixel_lab.b - cl.b;
                float dist = dL * dL + da * da + db * db;

                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = c;
                }
            }

            ++counts[best_idx];
        }
    }

    std::size_t max_count = 0;
    std::uint8_t most_common = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (counts[i] > max_count) {
            max_count = counts[i];
            most_common = static_cast<std::uint8_t>(i);
        }
    }

    return most_common;
}

// ---------------------------------------------------------------------------
// Hires quantization
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Metric-aware helpers (shared by hires / multicolor / sprite paths).
// ---------------------------------------------------------------------------

using Tap = blur_util::Tap;
using blur_util::build_kernel_taps;
using blur_util::compute_blurred;
using blur_util::PixelDistLut;
using blur_util::build_pixel_dist_lut;
using blur_util::ClosedCtx;
using blur_util::make_closed_ctx;
using blur_util::score_cell_blur_2color;
using blur_util::score_cell_blur_fused;

struct CellSsimStats {
    color_space::OKLab mu_s;
    color_space::OKLab var_s;
};

inline CellSsimStats compute_ssim_stats(
    const std::vector<color_space::OKLab>& cell_lab) {
    CellSsimStats out{};
    auto n = cell_lab.size();
    float inv_n = 1.0f / static_cast<float>(n);
    for (auto& v : cell_lab) {
        out.mu_s.L += v.L; out.mu_s.a += v.a; out.mu_s.b += v.b;
    }
    out.mu_s.L *= inv_n; out.mu_s.a *= inv_n; out.mu_s.b *= inv_n;
    for (auto& v : cell_lab) {
        float dL = v.L - out.mu_s.L, da = v.a - out.mu_s.a, db = v.b - out.mu_s.b;
        out.var_s.L += dL * dL;
        out.var_s.a += da * da;
        out.var_s.b += db * db;
    }
    out.var_s.L *= inv_n; out.var_s.a *= inv_n; out.var_s.b *= inv_n;
    return out;
}

// Build per-pixel-nearest render of a cell given a k-color palette.
inline void render_cell_nearest(
    const std::vector<color_space::OKLab>& cell_lab,
    const std::vector<color_space::OKLab>& pal_lab,
    std::span<const std::uint8_t> colors,
    std::vector<color_space::OKLab>& out) {
    out.resize(cell_lab.size());
    for (std::size_t p = 0; p < cell_lab.size(); ++p) {
        auto& px = cell_lab[p];
        float bd = std::numeric_limits<float>::max();
        std::uint8_t bi = 0;
        for (std::uint8_t c = 0; c < colors.size(); ++c) {
            auto& cl = pal_lab[colors[c]];
            float d = (px.L-cl.L)*(px.L-cl.L)
                    + (px.a-cl.a)*(px.a-cl.a)
                    + (px.b-cl.b)*(px.b-cl.b);
            if (d < bd) { bd = d; bi = c; }
        }
        out[p] = pal_lab[colors[bi]];
    }
}


// Score a candidate (cell, colors[]) under the ssim+mse hybrid metric.
inline float score_cell_ssim(
    const std::vector<color_space::OKLab>& cell_lab,
    const CellSsimStats& stats,
    const std::vector<color_space::OKLab>& pal_lab,
    std::span<const std::uint8_t> colors,
    std::vector<color_space::OKLab>& scratch) {
    render_cell_nearest(cell_lab, pal_lab, colors, scratch);
    constexpr float C1 = 0.01f * 0.01f;
    constexpr float C2 = 0.03f * 0.03f;
    constexpr float kMseLambda = 1.0f;
    auto n = cell_lab.size();
    float inv_n = 1.0f / static_cast<float>(n);
    color_space::OKLab mu_r{0, 0, 0};
    float mse = 0;
    for (std::size_t p = 0; p < n; ++p) {
        mu_r.L += scratch[p].L;
        mu_r.a += scratch[p].a;
        mu_r.b += scratch[p].b;
        float dL = cell_lab[p].L - scratch[p].L;
        float da = cell_lab[p].a - scratch[p].a;
        float db = cell_lab[p].b - scratch[p].b;
        mse += dL * dL + da * da + db * db;
    }
    mu_r.L *= inv_n; mu_r.a *= inv_n; mu_r.b *= inv_n;
    color_space::OKLab var_r{0, 0, 0}, cov{0, 0, 0};
    for (std::size_t p = 0; p < n; ++p) {
        float drL = scratch[p].L - mu_r.L;
        float dra = scratch[p].a - mu_r.a;
        float drb = scratch[p].b - mu_r.b;
        var_r.L += drL * drL;
        var_r.a += dra * dra;
        var_r.b += drb * drb;
        cov.L += drL * (cell_lab[p].L - stats.mu_s.L);
        cov.a += dra * (cell_lab[p].a - stats.mu_s.a);
        cov.b += drb * (cell_lab[p].b - stats.mu_s.b);
    }
    var_r.L *= inv_n; var_r.a *= inv_n; var_r.b *= inv_n;
    cov.L *= inv_n; cov.a *= inv_n; cov.b *= inv_n;
    auto ssim_ch = [&](float ms, float vs, float cv, float mr, float vr) {
        float num = (2.0f * ms * mr + C1) * (2.0f * cv + C2);
        float den = (ms * ms + mr * mr + C1) * (vs + vr + C2);
        return num / den;
    };
    float ssim = ssim_ch(stats.mu_s.L, stats.var_s.L, cov.L, mu_r.L, var_r.L)
               + ssim_ch(stats.mu_s.a, stats.var_s.a, cov.a, mu_r.a, var_r.a)
               + ssim_ch(stats.mu_s.b, stats.var_s.b, cov.b, mu_r.b, var_r.b);
    return -ssim + kMseLambda * mse;
}

// Read a cell's source pixels into an OKLab vector. If `threshold_fn` is
// non-empty, applies the ordered-dither threshold bias to each pixel's
// OKLab (same formula precompute_cell_dist uses for the MSE path) so
// blur/ssim scoring and per-pixel-nearest rendering both see the
// dithered source.
inline std::vector<color_space::OKLab> read_cell_lab(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    const vic2::ModeParams& params,
    const ThresholdFn& threshold_fn = {},
    float threshold_strength = 0.0f) {
    std::vector<color_space::OKLab> cell_lab;
    cell_lab.reserve(params.cell_width * params.cell_height);
    auto px = cell_x * params.cell_width;
    auto py = cell_y * params.cell_height;
    bool has_threshold = static_cast<bool>(threshold_fn);
    for (std::size_t dy = 0; dy < params.cell_height; ++dy)
        for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
            auto lab = color_space::linear_to_oklab(image[px + dx, py + dy]);
            if (has_threshold) {
                float t = threshold_fn(px + dx, py + dy) * threshold_strength;
                lab.L += t * 0.15f;
                lab.a += t * 0.03f;
                lab.b += t * 0.03f;
            }
            cell_lab.push_back(lab);
        }
    return cell_lab;
}

ScreenResult quantize_hires(vic2::Mode mode, const Image& image,
                             const Palette& palette,
                             const std::vector<color_space::OKLab>& palette_lab,
                             const vic2::ModeParams& params,
                             const ThresholdFn& tfn, float tstr,
                             Metric metric) {
    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto n = static_cast<std::uint8_t>(palette.size());

    ScreenResult result;
    result.mode = mode;
    result.background_color = 0;
    result.total_error = 0.0f;
    result.cells.resize(cx * cy);

    std::atomic<float> total_error{0.0f};

    if (metric == Metric::mse) {
        parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
            auto table = precompute_cell_dist(image, x, y, palette_lab, params,
                                               tfn, tstr);
            auto cell = quantize_cell_hires(table, palette);

            float current = total_error.load(std::memory_order_relaxed);
            while (!total_error.compare_exchange_weak(
                current, current + cell.error, std::memory_order_relaxed)) {}

            result.cells[idx] = std::move(cell);
        });
    } else {
        auto kernel_taps = build_kernel_taps(params.cell_width,
                                              params.cell_height);
        parallel_for_cells(cx, cy,
            [&](std::size_t idx, std::size_t x, std::size_t y) {
                auto cell_lab = read_cell_lab(image, x, y, params,
                                               tfn, tstr);
                std::vector<color_space::OKLab> blurred;
                CellSsimStats stats{};
                PixelDistLut pd_lut;
                ClosedCtx blur_ctx{};
                if (metric == Metric::blur) {
                    blurred = compute_blurred(cell_lab, kernel_taps);
                    pd_lut = build_pixel_dist_lut(cell_lab, palette_lab);
                    blur_ctx = make_closed_ctx(blurred);
                } else {
                    stats = compute_ssim_stats(cell_lab);
                }
                std::vector<color_space::OKLab> ssim_scratch;
                ssim_scratch.reserve(cell_lab.size());
                float best_err = std::numeric_limits<float>::max();
                std::uint8_t best_i = 0, best_j = 1;
                for (std::uint8_t i = 0; i < n; ++i) {
                    for (std::uint8_t j = i + 1; j < n; ++j) {
                        float err;
                        if (metric == Metric::blur) {
                            err = score_cell_blur_2color(
                                pd_lut, blurred, kernel_taps, palette_lab,
                                blur_ctx, i, j);
                        } else {
                            std::array<std::uint8_t, 2> colors{i, j};
                            err = score_cell_ssim(cell_lab, stats, palette_lab,
                                                   colors, ssim_scratch);
                        }
                        if (err < best_err) {
                            best_err = err;
                            best_i = i; best_j = j;
                        }
                    }
                }
                // Build final pixel assignments via per-pixel-nearest of (best_i, best_j).
                CellResult cell;
                cell.cell_colors = {best_i, best_j};
                cell.pixel_indices.resize(cell_lab.size());
                auto& c0 = palette_lab[best_i];
                auto& c1 = palette_lab[best_j];
                for (std::size_t p = 0; p < cell_lab.size(); ++p) {
                    auto& px = cell_lab[p];
                    float d0 = (px.L-c0.L)*(px.L-c0.L) + (px.a-c0.a)*(px.a-c0.a) + (px.b-c0.b)*(px.b-c0.b);
                    float d1 = (px.L-c1.L)*(px.L-c1.L) + (px.a-c1.a)*(px.a-c1.a) + (px.b-c1.b)*(px.b-c1.b);
                    cell.pixel_indices[p] = (d1 < d0) ? 1 : 0;
                }
                cell.error = best_err;

                float current = total_error.load(std::memory_order_relaxed);
                while (!total_error.compare_exchange_weak(
                    current, current + cell.error, std::memory_order_relaxed)) {}

                result.cells[idx] = std::move(cell);
            });
    }

    result.total_error = total_error.load();
    return result;
}

// ---------------------------------------------------------------------------
// Multicolor quantization with precomputed tables
// ---------------------------------------------------------------------------

// Precompute distance tables for ALL cells once, reuse for all bg candidates.
ScreenResult quantize_multicolor_bruteforce(
    vic2::Mode mode, const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& tfn, float tstr,
    Metric metric) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;
    auto n = static_cast<std::uint8_t>(palette.size());

    float best_total = std::numeric_limits<float>::max();
    ScreenResult best;

    if (metric == Metric::mse) {
        // Step 1: Precompute all cell distance tables in parallel
        std::vector<CellDistTable> tables(total_cells);
        parallel_for_cells(cx, cy,
            [&](std::size_t idx, std::size_t x, std::size_t y) {
                tables[idx] = precompute_cell_dist(image, x, y, palette_lab,
                                                    params, tfn, tstr);
            });

        // Step 2: Try all 16 background colors, cells in parallel for each
        for (std::uint8_t bg = 0; bg < n; ++bg) {
            ScreenResult candidate;
            candidate.mode = mode;
            candidate.background_color = bg;
            candidate.cells.resize(total_cells);

            std::atomic<float> total_error{0.0f};

            parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t, std::size_t) {
                auto cell = quantize_cell_multicolor(tables[idx], bg, palette);

                float current = total_error.load(std::memory_order_relaxed);
                while (!total_error.compare_exchange_weak(
                    current, current + cell.error, std::memory_order_relaxed)) {}

                candidate.cells[idx] = std::move(cell);
            });

            candidate.total_error = total_error.load();

            if (candidate.total_error < best_total) {
                best_total = candidate.total_error;
                best = std::move(candidate);
            }
        }
        return best;
    }

    // Metric-aware path: precompute per-cell OKLab and metric stats,
    // then brute-force 16 bgs × C(15,3) triples per cell using the
    // metric scorer.
    auto kernel_taps = build_kernel_taps(params.cell_width, params.cell_height);
    std::vector<std::vector<color_space::OKLab>> cells_lab(total_cells);
    std::vector<std::vector<color_space::OKLab>> blurred_src;
    std::vector<PixelDistLut> pd_luts;
    std::vector<CellSsimStats> ssim_stats;
    if (metric == Metric::blur) {
        blurred_src.resize(total_cells);
        pd_luts.resize(total_cells);
    } else {
        ssim_stats.resize(total_cells);
    }
    parallel_for_cells(cx, cy,
        [&](std::size_t idx, std::size_t x, std::size_t y) {
            cells_lab[idx] = read_cell_lab(image, x, y, params, tfn, tstr);
            if (metric == Metric::blur) {
                blurred_src[idx] = compute_blurred(cells_lab[idx],
                                                     kernel_taps);
                pd_luts[idx] = build_pixel_dist_lut(cells_lab[idx],
                                                     palette_lab);
            } else {
                ssim_stats[idx] = compute_ssim_stats(cells_lab[idx]);
            }
        });

    // Per-cell err for each unordered 4-set of palette colours.
    //
    // Key observation: for a fixed 4-set, both the blur and ssim scorers
    // produce the same error regardless of which element we label "bg"
    // (the rendering is per-pixel-nearest of the 4 colours; bg is just
    // one of them). The original algorithm tried 16 bgs × C(15,3) = 7280
    // (bg, triple) combos per cell, but those collapse to C(16,4) = 1820
    // unique 4-sets (each evaluated 4 times). Compute each 4-set's err
    // ONCE per cell, then choose the bg that minimises the screen total
    // by reducing.
    constexpr auto encode_4set = [](std::uint8_t a, std::uint8_t b,
                                     std::uint8_t c, std::uint8_t d) {
        return static_cast<std::uint16_t>(
            (1u << a) | (1u << b) | (1u << c) | (1u << d));
    };
    struct FourSet {
        std::uint16_t mask;            // bitmap of the 4 palette indices
        std::uint8_t a, b, c, d;       // a < b < c < d
    };
    std::vector<FourSet> four_sets;
    four_sets.reserve(1820);
    for (std::uint8_t a = 0; a < n; ++a)
        for (std::uint8_t b = a + 1; b < n; ++b)
            for (std::uint8_t c = b + 1; c < n; ++c)
                for (std::uint8_t d = c + 1; d < n; ++d)
                    four_sets.push_back({encode_4set(a, b, c, d), a, b, c, d});
    auto num_sets = four_sets.size();

    // err_table[idx][s] = per-cell error for 4-set s.
    std::vector<std::vector<float>> err_table(total_cells,
                                               std::vector<float>(num_sets));
    parallel_for_cells(cx, cy,
        [&](std::size_t idx, std::size_t, std::size_t) {
            std::vector<color_space::OKLab> ssim_scratch;
            std::vector<color_space::OKLab> fused_scratch;
            ssim_scratch.reserve(cells_lab[idx].size());
            fused_scratch.reserve(cells_lab[idx].size());
            for (std::size_t s = 0; s < num_sets; ++s) {
                auto& fs = four_sets[s];
                std::array<std::uint8_t, 4> colors{fs.a, fs.b, fs.c, fs.d};
                if (metric == Metric::blur) {
                    err_table[idx][s] = score_cell_blur_fused(
                        pd_luts[idx], blurred_src[idx],
                        kernel_taps, palette_lab, colors, fused_scratch);
                } else {
                    err_table[idx][s] = score_cell_ssim(
                        cells_lab[idx], ssim_stats[idx],
                        palette_lab, colors, ssim_scratch);
                }
            }
        });

    // Reduce: for each candidate bg, sum per-cell min err over 4-sets
    // that contain bg.
    for (std::uint8_t bg = 0; bg < n; ++bg) {
        auto bg_bit = static_cast<std::uint16_t>(1u << bg);
        // Gather indices of 4-sets containing bg (455 of them).
        std::vector<std::uint16_t> bg_sets;
        bg_sets.reserve(455);
        for (std::uint16_t s = 0; s < num_sets; ++s)
            if (four_sets[s].mask & bg_bit) bg_sets.push_back(s);

        ScreenResult candidate;
        candidate.mode = mode;
        candidate.background_color = bg;
        candidate.cells.resize(total_cells);

        std::atomic<float> total_error{0.0f};
        parallel_for_cells(cx, cy,
            [&](std::size_t idx, std::size_t, std::size_t) {
                float best_err = std::numeric_limits<float>::max();
                std::uint16_t best_s = 0;
                for (auto s : bg_sets) {
                    float e = err_table[idx][s];
                    if (e < best_err) { best_err = e; best_s = s; }
                }
                auto& fs = four_sets[best_s];
                // Order chosen so bg is at slot 0; remaining 3 keep
                // ascending order for deterministic labelling.
                std::array<std::uint8_t, 4> chosen{bg, 0, 0, 0};
                std::size_t slot = 1;
                for (auto c : {fs.a, fs.b, fs.c, fs.d})
                    if (c != bg) chosen[slot++] = c;

                CellResult cell;
                cell.cell_colors = {chosen[0], chosen[1], chosen[2], chosen[3]};
                cell.pixel_indices.resize(cells_lab[idx].size());
                for (std::size_t p = 0; p < cells_lab[idx].size(); ++p) {
                    auto& px = cells_lab[idx][p];
                    float bd = std::numeric_limits<float>::max();
                    std::uint8_t bc = 0;
                    for (std::uint8_t c = 0; c < 4; ++c) {
                        auto& cl = palette_lab[chosen[c]];
                        float d = (px.L-cl.L)*(px.L-cl.L)
                                + (px.a-cl.a)*(px.a-cl.a)
                                + (px.b-cl.b)*(px.b-cl.b);
                        if (d < bd) { bd = d; bc = c; }
                    }
                    cell.pixel_indices[p] = bc;
                }
                cell.error = best_err;

                float current = total_error.load(std::memory_order_relaxed);
                while (!total_error.compare_exchange_weak(
                    current, current + cell.error, std::memory_order_relaxed)) {}

                candidate.cells[idx] = std::move(cell);
            });

        candidate.total_error = total_error.load();
        if (candidate.total_error < best_total) {
            best_total = candidate.total_error;
            best = std::move(candidate);
        }
    }

    return best;
}

// Multicolor with frequency-based background (for sprites)
// Shared MC0/MC1 across all sprites, only per-sprite color varies.
ScreenResult quantize_multicolor_frequency_bg(
    vic2::Mode mode, const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& tfn, float tstr) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;
    auto n = static_cast<std::uint8_t>(palette.size());

    auto bg = find_most_common_color(image, palette, palette_lab);

    std::vector<CellDistTable> tables(total_cells);
    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        tables[idx] = precompute_cell_dist(image, x, y, palette_lab, params,
                                            tfn, tstr);
    });

    // Brute force shared MC0/MC1: try all C(15,2) pairs.
    // For each pair, find the best per-sprite color for each cell.
    float best_total = std::numeric_limits<float>::max();
    std::vector<CellResult> best_cells;

    for (std::uint8_t mc0 = 0; mc0 < n; ++mc0) {
        if (mc0 == bg) continue;
        for (std::uint8_t mc1 = mc0 + 1; mc1 < n; ++mc1) {
            if (mc1 == bg) continue;

            float total = 0.0f;
            std::vector<CellResult> cells(total_cells);

            for (std::size_t idx = 0; idx < total_cells; ++idx) {
                auto& table = tables[idx];
                float cell_best = std::numeric_limits<float>::max();
                std::uint8_t cell_best_k = 0;

                // Try each per-sprite color
                for (std::uint8_t k = 0; k < n; ++k) {
                    if (k == bg || k == mc0 || k == mc1) continue;
                    float err = cell_error_4(table, bg, mc0, mc1, k);
                    if (err < cell_best) {
                        cell_best = err;
                        cell_best_k = k;
                    }
                }

                std::array<std::uint8_t, 4> indices = {bg, mc0, mc1, cell_best_k};
                auto eval = evaluate_cell_full(table, indices);

                cells[idx].pixel_indices = std::move(eval.assignments);
                cells[idx].cell_colors = {bg, mc0, mc1, cell_best_k};
                cells[idx].error = eval.error;
                total += eval.error;
            }

            if (total < best_total) {
                best_total = total;
                best_cells = std::move(cells);
            }
        }
    }

    ScreenResult result;
    result.mode = mode;
    result.background_color = bg;
    result.cells = std::move(best_cells);
    result.total_error = best_total;
    return result;
}

// ---------------------------------------------------------------------------
// FLI multicolor quantization (brute force bg, per-row screen colors)
// ---------------------------------------------------------------------------

ScreenResult quantize_fli_with_bg(
    vic2::Mode mode, std::uint8_t bg,
    const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& tfn, float tstr) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;

    std::vector<CellDistTable> tables(total_cells);
    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        tables[idx] = precompute_cell_dist(image, x, y, palette_lab, params,
                                            tfn, tstr);
    });

    ScreenResult result;
    result.mode = mode;
    result.background_color = bg;
    result.cells.resize(total_cells);

    std::atomic<float> total_error{0.0f};

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t) {
        CellResult cell;
        if (x < vic2::fli_bug_columns) {
            // FLI bug: shows $D021 (background) color
            auto pixels = params.cell_width * params.cell_height;
            cell.cell_colors.resize(18, bg);
            cell.pixel_indices.resize(pixels, 0);
            cell.error = 0.0f;
            for (std::size_t p = 0; p < pixels; ++p)
                cell.error += tables[idx].dist[bg][p];
        } else {
            cell = quantize_cell_fli(tables[idx], bg, palette);
        }

        float current = total_error.load(std::memory_order_relaxed);
        while (!total_error.compare_exchange_weak(
            current, current + cell.error, std::memory_order_relaxed)) {}

        result.cells[idx] = std::move(cell);
    });

    result.total_error = total_error.load();
    return result;
}

// ---------------------------------------------------------------------------
// AFLI hires quantization (per-row 2 colors, no shared background)
// ---------------------------------------------------------------------------

// AFLI with a specific background color for bug columns + $D021
ScreenResult quantize_afli_with_bg(
    vic2::Mode mode, std::uint8_t bg,
    const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& tfn, float tstr) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;

    std::vector<CellDistTable> tables(total_cells);
    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        tables[idx] = precompute_cell_dist(image, x, y, palette_lab, params,
                                            tfn, tstr);
    });

    ScreenResult result;
    result.mode = mode;
    result.background_color = bg;
    result.cells.resize(total_cells);

    std::atomic<float> total_error{0.0f};

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t) {
        CellResult cell;
        if (x < vic2::fli_bug_columns) {
            // FLI bug: shows $D021 (background) color
            constexpr std::size_t cell_w = 8, cell_h = 8;
            cell.cell_colors.resize(16, bg);
            cell.pixel_indices.resize(cell_w * cell_h, 0);
            cell.error = 0.0f;
            for (std::size_t p = 0; p < cell_w * cell_h; ++p)
                cell.error += tables[idx].dist[bg][p];
        } else {
            cell = quantize_cell_afli(tables[idx], palette);
        }

        float current = total_error.load(std::memory_order_relaxed);
        while (!total_error.compare_exchange_weak(
            current, current + cell.error, std::memory_order_relaxed)) {}

        result.cells[idx] = std::move(cell);
    });

    result.total_error = total_error.load();
    return result;
}

// ---------------------------------------------------------------------------
// PETSCII metric helpers — shared precomputed quantities for the blur
// and ssim metrics. The 8×8 cell is convolved with a 3×3 binomial
// (≈ Gaussian σ=0.85) low-pass with replicate padding; closed-form
// expansions then make per-(fg, bg, glyph) scoring O(1).
// ---------------------------------------------------------------------------

constexpr std::array<std::array<float, 3>, 3> kBlurKernel = {{
    {1.0f / 16, 2.0f / 16, 1.0f / 16},
    {2.0f / 16, 4.0f / 16, 2.0f / 16},
    {1.0f / 16, 2.0f / 16, 1.0f / 16},
}};

// 64 output pixels, each gathers from 9 source positions (with replicate
// padding folding edge taps onto boundary pixels — duplicates allowed).
using KernelTaps = std::array<std::array<Tap, 9>, 64>;

KernelTaps build_kernel_taps_8x8() {
    KernelTaps taps{};
    for (std::size_t py = 0; py < 8; ++py) {
        for (std::size_t px = 0; px < 8; ++px) {
            std::size_t p_out = py * 8 + px;
            std::size_t k = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(py) + dy, 0, 7);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(static_cast<int>(px) + dx, 0, 7);
                    taps[p_out][k++] = {
                        static_cast<std::uint16_t>(
                            static_cast<std::size_t>(ny) * 8 +
                            static_cast<std::size_t>(nx)),
                        kBlurKernel[static_cast<std::size_t>(dy + 1)]
                                   [static_cast<std::size_t>(dx + 1)]};
                }
            }
        }
    }
    return taps;
}

// Per glyph: 64-bit fg mask + kernel-weighted quantities for the
// blurred-MSE closed form (reused across bg/fg trials and across cells).
struct GlyphPrecompute {
    std::uint64_t fg_mask;          // bit p set ⇔ pixel p is fg
    std::array<float, 64> fw;       // fg fractional kernel weight per output pixel
    float K2;                       // Σ fw²
    float K3;                       // Σ fw·(1−fw)
    float K4;                       // Σ (1−fw)²
};

std::array<GlyphPrecompute, 256>
build_glyph_precompute(const KernelTaps& taps) {
    std::array<GlyphPrecompute, 256> g{};
    for (std::size_t ch = 0; ch < 256; ++ch) {
        std::uint64_t mask = 0;
        const auto& bits = petscii::char_bits[ch];
        for (std::uint8_t i = 0; i < bits.count; ++i)
            mask |= (std::uint64_t{1} << bits.positions[i]);
        g[ch].fg_mask = mask;
        float K2 = 0, K3 = 0, K4 = 0;
        for (std::size_t p = 0; p < 64; ++p) {
            float w = 0;
            for (auto& tap : taps[p]) {
                if ((mask >> tap.q) & 1ULL) w += tap.w;
            }
            g[ch].fw[p] = w;
            float ma = 1.0f - w;
            K2 += w * w;
            K3 += w * ma;
            K4 += ma * ma;
        }
        g[ch].K2 = K2;
        g[ch].K3 = K3;
        g[ch].K4 = K4;
    }
    return g;
}

// Per-cell precomputed values for the blur metric.
struct BlurCell {
    std::array<color_space::OKLab, 64> blurred;
    float K0;                               // Σ ||blurred[p]||²
    std::array<color_space::OKLab, 256> T1; // Σ blurred[p] · fw[p]   per glyph
    std::array<color_space::OKLab, 256> T2; // Σ blurred[p] · (1−fw)  per glyph
};

void precompute_blur_cell(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    const KernelTaps& taps,
    const std::array<GlyphPrecompute, 256>& gp,
    BlurCell& out) {
    auto px = cell_x * 8;
    auto py = cell_y * 8;
    std::array<color_space::OKLab, 64> src_lab;
    for (std::size_t dy = 0; dy < 8; ++dy)
        for (std::size_t dx = 0; dx < 8; ++dx)
            src_lab[dy * 8 + dx] =
                color_space::linear_to_oklab(image[px + dx, py + dy]);
    out.K0 = 0;
    for (std::size_t p = 0; p < 64; ++p) {
        color_space::OKLab b{0, 0, 0};
        for (auto& tap : taps[p]) {
            auto& v = src_lab[tap.q];
            b.L += tap.w * v.L;
            b.a += tap.w * v.a;
            b.b += tap.w * v.b;
        }
        out.blurred[p] = b;
        out.K0 += b.L * b.L + b.a * b.a + b.b * b.b;
    }
    for (std::size_t ch = 0; ch < 256; ++ch) {
        color_space::OKLab T1{0, 0, 0};
        color_space::OKLab T2{0, 0, 0};
        for (std::size_t p = 0; p < 64; ++p) {
            float w = gp[ch].fw[p];
            float ma = 1.0f - w;
            T1.L += out.blurred[p].L * w;
            T1.a += out.blurred[p].a * w;
            T1.b += out.blurred[p].b * w;
            T2.L += out.blurred[p].L * ma;
            T2.a += out.blurred[p].a * ma;
            T2.b += out.blurred[p].b * ma;
        }
        out.T1[ch] = T1;
        out.T2[ch] = T2;
    }
}

// Per-cell precomputed values for the SSIM metric. n=64 is Wang's
// canonical 8×8 window size, so per-cell variance estimates are
// statistically sound here.
//
// `A_norm = Σ_p ||src[p]||²` and `total_sum = Σ_p src[p]` are stored to
// support a hybrid SSIM + MSE error: SSIM drives structure (contours,
// glyph choice), MSE pins colour fidelity. The per-pair MSE reuses the
// existing per-glyph S_fg via B_fg = S_fg + pop·μ_s — no extra per-glyph
// precompute needed.
struct SsimCell {
    color_space::OKLab mu_s;
    color_space::OKLab var_s;
    color_space::OKLab total_sum;       // Σ src[p]
    float A_norm;                        // Σ ||src[p]||²
    std::array<color_space::OKLab, 256> S_fg;  // per glyph
};

void precompute_ssim_cell(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    const std::array<GlyphPrecompute, 256>& gp,
    SsimCell& out) {
    auto px = cell_x * 8;
    auto py = cell_y * 8;
    std::array<color_space::OKLab, 64> src_lab;
    color_space::OKLab mu{0, 0, 0};
    for (std::size_t dy = 0; dy < 8; ++dy)
        for (std::size_t dx = 0; dx < 8; ++dx) {
            auto v = color_space::linear_to_oklab(image[px + dx, py + dy]);
            src_lab[dy * 8 + dx] = v;
            mu.L += v.L; mu.a += v.a; mu.b += v.b;
        }
    constexpr float inv_n = 1.0f / 64.0f;
    out.total_sum = mu;  // unscaled sum for hybrid MSE term
    mu.L *= inv_n; mu.a *= inv_n; mu.b *= inv_n;
    color_space::OKLab var{0, 0, 0};
    float A_norm = 0;
    for (std::size_t p = 0; p < 64; ++p) {
        float dL = src_lab[p].L - mu.L;
        float da = src_lab[p].a - mu.a;
        float db = src_lab[p].b - mu.b;
        var.L += dL * dL;
        var.a += da * da;
        var.b += db * db;
        A_norm += src_lab[p].L * src_lab[p].L
                + src_lab[p].a * src_lab[p].a
                + src_lab[p].b * src_lab[p].b;
    }
    var.L *= inv_n; var.a *= inv_n; var.b *= inv_n;
    out.mu_s = mu;
    out.var_s = var;
    out.A_norm = A_norm;
    for (std::size_t ch = 0; ch < 256; ++ch) {
        color_space::OKLab S{0, 0, 0};
        std::uint64_t mask = gp[ch].fg_mask;
        while (mask) {
            auto p = static_cast<unsigned>(std::countr_zero(mask));
            mask &= mask - 1;
            S.L += src_lab[p].L - mu.L;
            S.a += src_lab[p].a - mu.a;
            S.b += src_lab[p].b - mu.b;
        }
        out.S_fg[ch] = S;
    }
}

// Build a cell's pixel_indices from a chosen character bitmap.
std::vector<std::uint8_t> bitmap_for_char(std::uint8_t ch) {
    std::vector<std::uint8_t> idx(64);
    for (std::size_t row = 0; row < 8; ++row) {
        auto byte = petscii::character_rom[ch * 8 + row];
        for (std::size_t col = 0; col < 8; ++col)
            idx[row * 8 + col] = (byte & (0x80 >> col)) ? 1 : 0;
    }
    return idx;
}

CellResult petscii_pick_blur(
    const BlurCell& bc,
    const std::array<GlyphPrecompute, 256>& gp,
    std::uint8_t bg_color,
    const std::vector<color_space::OKLab>& pal_lab,
    const std::vector<float>& pal_norm,
    const std::vector<std::vector<float>>& pal_dot,
    bool graphics_only) {
    auto n = static_cast<std::uint8_t>(pal_lab.size());
    auto& bg_lab = pal_lab[bg_color];
    float best_err = std::numeric_limits<float>::max();
    std::uint8_t best_ch = 0, best_fg = bg_color;
    for (std::size_t ch = 0; ch < 256; ++ch) {
        if (graphics_only &&
            !petscii::is_graphic_char(static_cast<std::uint8_t>(ch)))
            continue;
        auto& T1 = bc.T1[ch];
        auto& T2 = bc.T2[ch];
        float K2 = gp[ch].K2;
        float K3 = gp[ch].K3;
        float K4 = gp[ch].K4;
        // bg-only contribution: const w.r.t. fg, hoist out.
        float bg_term = -2.0f * (T2.L * bg_lab.L + T2.a * bg_lab.a +
                                  T2.b * bg_lab.b)
                      + K4 * pal_norm[bg_color];
        for (std::uint8_t fg = 0; fg < n; ++fg) {
            if (fg == bg_color) continue;
            auto& fg_lab = pal_lab[fg];
            float err = bc.K0
                      - 2.0f * (T1.L * fg_lab.L + T1.a * fg_lab.a +
                                T1.b * fg_lab.b)
                      + bg_term
                      + 2.0f * K3 * pal_dot[fg][bg_color]
                      + K2 * pal_norm[fg];
            if (err < best_err) {
                best_err = err;
                best_ch = static_cast<std::uint8_t>(ch);
                best_fg = fg;
            }
        }
    }
    CellResult r;
    r.pixel_indices = bitmap_for_char(best_ch);
    r.cell_colors = {bg_color, best_fg};
    r.char_index = best_ch;
    r.error = best_err;
    return r;
}

CellResult petscii_pick_ssim(
    const SsimCell& sc,
    std::uint8_t bg_color,
    const std::vector<color_space::OKLab>& pal_lab,
    const std::vector<float>& pal_norm,
    bool graphics_only) {
    auto n = static_cast<std::uint8_t>(pal_lab.size());
    auto& bg_lab = pal_lab[bg_color];
    constexpr float inv_n = 1.0f / 64.0f;
    constexpr float C1 = 0.01f * 0.01f;
    constexpr float C2 = 0.03f * 0.03f;
    // Hybrid weight: SSIM (full L+a+b) drives glyph/structure choice;
    // this MSE term pulls colour selection back toward the source so
    // chroma drift (a known SSIM weakness) is bounded. Tuned by eye on
    // alien.png — small enough that SSIM still wins on contour
    // fidelity, big enough to stop the encoder picking near-mean
    // colours that ignore the source's actual hue.
    constexpr float kMseLambda = 1.0f;
    float best_err = std::numeric_limits<float>::max();
    std::uint8_t best_ch = 0, best_fg = bg_color;
    for (std::size_t ch = 0; ch < 256; ++ch) {
        if (graphics_only &&
            !petscii::is_graphic_char(static_cast<std::uint8_t>(ch)))
            continue;
        auto pop = static_cast<std::size_t>(petscii::char_bits[ch].count);
        float wf = static_cast<float>(pop) * inv_n;
        float wb = 1.0f - wf;
        float wfwb = wf * wb;
        auto& S = sc.S_fg[ch];
        float cov_sL = S.L * inv_n;
        float cov_sa = S.a * inv_n;
        float cov_sb = S.b * inv_n;
        // For the MSE term we need the un-centered partial sums:
        //   B_fg = Σ_{p∈fg} src[p]   = S_fg + pop·μ_s
        //   B_bg = Σ_{p∉fg} src[p]   = total_sum − B_fg
        color_space::OKLab B_fg{
            S.L + static_cast<float>(pop) * sc.mu_s.L,
            S.a + static_cast<float>(pop) * sc.mu_s.a,
            S.b + static_cast<float>(pop) * sc.mu_s.b};
        color_space::OKLab B_bg{
            sc.total_sum.L - B_fg.L,
            sc.total_sum.a - B_fg.a,
            sc.total_sum.b - B_fg.b};
        float n_fg = static_cast<float>(pop);
        float n_bg = static_cast<float>(64 - pop);
        // bg-only MSE contribution: hoist out of the fg loop.
        float bg_norm = pal_norm[bg_color];
        float mse_bg = -2.0f * (B_bg.L * bg_lab.L + B_bg.a * bg_lab.a +
                                 B_bg.b * bg_lab.b)
                     + n_bg * bg_norm;
        for (std::uint8_t fg = 0; fg < n; ++fg) {
            if (fg == bg_color) continue;
            auto& fg_lab = pal_lab[fg];
            auto channel = [&](float ms, float vs, float cs,
                               float fc, float bc) {
                float mu_r = wf * fc + wb * bc;
                float dfb = fc - bc;
                float var_r = wfwb * dfb * dfb;
                float cov = cs * dfb;
                float num = (2.0f * ms * mu_r + C1) * (2.0f * cov + C2);
                float den = (ms * ms + mu_r * mu_r + C1)
                          * (vs + var_r + C2);
                return num / den;
            };
            float ssim = channel(sc.mu_s.L, sc.var_s.L, cov_sL, fg_lab.L, bg_lab.L)
                       + channel(sc.mu_s.a, sc.var_s.a, cov_sa, fg_lab.a, bg_lab.a)
                       + channel(sc.mu_s.b, sc.var_s.b, cov_sb, fg_lab.b, bg_lab.b);
            // mse = A_norm − 2·fg·B_fg − 2·bg·B_bg + pop·||fg||²
            //                                     + (n−pop)·||bg||²
            float mse = sc.A_norm
                      - 2.0f * (B_fg.L * fg_lab.L + B_fg.a * fg_lab.a +
                                 B_fg.b * fg_lab.b)
                      + n_fg * pal_norm[fg]
                      + mse_bg;
            float err = -ssim + kMseLambda * mse;
            if (err < best_err) {
                best_err = err;
                best_ch = static_cast<std::uint8_t>(ch);
                best_fg = fg;
            }
        }
    }
    CellResult r;
    r.pixel_indices = bitmap_for_char(best_ch);
    r.cell_colors = {bg_color, best_fg};
    r.char_index = best_ch;
    r.error = best_err;
    return r;
}

// ---------------------------------------------------------------------------
// PETSCII: brute force bg, 256 ROM chars × 15 fg colors per cell
// ---------------------------------------------------------------------------

CellResult quantize_cell_petscii(
    const CellDistTable& table, std::uint8_t bg_color,
    const Palette& palette, bool graphics_only) {

    auto n = static_cast<std::uint8_t>(palette.size());

    // Precompute bg_sum (sum of bg distance for all 64 pixels)
    float bg_sum = 0.0f;
    for (std::size_t p = 0; p < 64; ++p)
        bg_sum += table.dist[bg_color][p];

    // Precompute delta[fg][p] = dist[fg][p] - dist[bg][p]
    std::array<std::array<float, 64>, max_palette> delta{};
    for (std::uint8_t fg = 0; fg < n; ++fg) {
        if (fg == bg_color) continue;
        for (std::size_t p = 0; p < 64; ++p)
            delta[fg][p] = table.dist[fg][p] - table.dist[bg_color][p];
    }

    float best_error = std::numeric_limits<float>::max();
    std::uint8_t best_char = 0;
    std::uint8_t best_fg = 0;

    for (std::uint8_t fg = 0; fg < n; ++fg) {
        if (fg == bg_color) continue;

        for (std::size_t ch = 0; ch < 256; ++ch) {
            if (graphics_only &&
                !petscii::is_graphic_char(static_cast<std::uint8_t>(ch)))
                continue;
            const auto& bits = petscii::char_bits[ch];

            float err = bg_sum;
            for (std::uint8_t i = 0; i < bits.count; ++i)
                err += delta[fg][bits.positions[i]];

            if (err < best_error) {
                best_error = err;
                best_char = static_cast<std::uint8_t>(ch);
                best_fg = fg;
            }
        }
    }

    // Build pixel_indices from the winning character bitmap
    CellResult result;
    result.pixel_indices.resize(64);
    for (std::size_t row = 0; row < 8; ++row) {
        auto byte = petscii::character_rom[best_char * 8 + row];
        for (std::size_t col = 0; col < 8; ++col)
            result.pixel_indices[row * 8 + col] =
                (byte & (0x80 >> col)) ? 1 : 0;
    }

    result.cell_colors = {bg_color, best_fg};
    result.char_index = best_char;
    result.error = best_error;
    return result;
}

ScreenResult quantize_petscii(
    vic2::Mode mode, const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& tfn, float tstr,
    Metric metric, bool graphics_only) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;
    auto n = static_cast<std::uint8_t>(palette.size());

    // Per-cell precompute selection: MSE uses the existing distance
    // table; blur and ssim build their own per-cell + per-glyph
    // structures so the inner (bg, fg) scan is a constant-time formula.
    std::vector<CellDistTable> mse_tables;
    std::vector<BlurCell> blur_cells;
    std::vector<SsimCell> ssim_cells;
    std::array<GlyphPrecompute, 256> glyph_pre{};
    KernelTaps kernel_taps{};
    std::vector<float> pal_norm;
    std::vector<std::vector<float>> pal_dot;

    if (metric == Metric::mse) {
        mse_tables.resize(total_cells);
        parallel_for_cells(cx, cy,
            [&](std::size_t idx, std::size_t x, std::size_t y) {
                mse_tables[idx] = precompute_cell_dist(
                    image, x, y, palette_lab, params, tfn, tstr);
            });
    } else {
        kernel_taps = build_kernel_taps_8x8();
        glyph_pre = build_glyph_precompute(kernel_taps);
        pal_norm.resize(n);
        pal_dot.assign(n, std::vector<float>(n, 0));
        for (std::uint8_t i = 0; i < n; ++i) {
            auto& a = palette_lab[i];
            pal_norm[i] = a.L * a.L + a.a * a.a + a.b * a.b;
            for (std::uint8_t j = 0; j < n; ++j) {
                auto& b = palette_lab[j];
                pal_dot[i][j] = a.L * b.L + a.a * b.a + a.b * b.b;
            }
        }
        if (metric == Metric::blur) {
            blur_cells.resize(total_cells);
            parallel_for_cells(cx, cy,
                [&](std::size_t idx, std::size_t x, std::size_t y) {
                    precompute_blur_cell(image, x, y, kernel_taps,
                                         glyph_pre, blur_cells[idx]);
                });
        } else {  // Metric::ssim
            ssim_cells.resize(total_cells);
            parallel_for_cells(cx, cy,
                [&](std::size_t idx, std::size_t x, std::size_t y) {
                    precompute_ssim_cell(image, x, y, glyph_pre,
                                         ssim_cells[idx]);
                });
        }
    }

    // Brute force all 16 background colors
    float best_total = std::numeric_limits<float>::max();
    ScreenResult best;

    for (std::uint8_t bg = 0; bg < n; ++bg) {
        ScreenResult candidate;
        candidate.mode = mode;
        candidate.background_color = bg;
        candidate.cells.resize(total_cells);

        std::atomic<float> total_error{0.0f};

        parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t, std::size_t) {
            CellResult cell;
            switch (metric) {
            case Metric::blur:
                cell = petscii_pick_blur(blur_cells[idx], glyph_pre, bg,
                                         palette_lab, pal_norm, pal_dot,
                                         graphics_only);
                break;
            case Metric::ssim:
                cell = petscii_pick_ssim(ssim_cells[idx], bg, palette_lab,
                                         pal_norm, graphics_only);
                break;
            case Metric::mse:
                cell = quantize_cell_petscii(mse_tables[idx], bg, palette,
                                             graphics_only);
                break;
            }

            float current = total_error.load(std::memory_order_relaxed);
            while (!total_error.compare_exchange_weak(
                current, current + cell.error, std::memory_order_relaxed)) {}

            candidate.cells[idx] = std::move(cell);
        });

        candidate.total_error = total_error.load();

        if (candidate.total_error < best_total) {
            best_total = candidate.total_error;
            best = std::move(candidate);
        }
    }

    return best;
}

} // namespace

Result<ScreenResult> quantize(const Image& image, const Palette& palette,
                              vic2::Mode mode,
                              const vic2::ModeParams& params,
                              ThresholdFn threshold,
                              float threshold_strength,
                              Metric metric,
                              bool graphics_only) {
    if (image.width() != params.screen_width ||
        image.height() != params.screen_height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Image is {}x{} but mode requires {}x{}",
                        image.width(), image.height(),
                        params.screen_width, params.screen_height),
        }};
    }

    auto palette_lab = precompute_oklab(palette);

    switch (mode) {
    case vic2::Mode::hires:
    case vic2::Mode::sprite_hires:
        return quantize_hires(mode, image, palette, palette_lab, params,
                              threshold, threshold_strength, metric);

    case vic2::Mode::multicolor:
        return quantize_multicolor_bruteforce(mode, image, palette,
                                              palette_lab, params,
                                              threshold, threshold_strength,
                                              metric);

    case vic2::Mode::sprite_multicolor:
        return quantize_multicolor_frequency_bg(mode, image, palette,
                                                palette_lab, params,
                                                threshold, threshold_strength);

    case vic2::Mode::fli:
        return quantize_fli_with_bg(mode, 0, image, palette, palette_lab,
                                    params, threshold, threshold_strength);

    case vic2::Mode::afli:
        return quantize_afli_with_bg(mode, 0, image, palette, palette_lab,
                                     params, threshold, threshold_strength);

    case vic2::Mode::petscii:
        return quantize_petscii(mode, image, palette, palette_lab,
                                params, threshold, threshold_strength,
                                metric, graphics_only);
    }

    std::unreachable();
}

} // namespace png2c64::quantize
