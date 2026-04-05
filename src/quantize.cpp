#include "quantize.hpp"
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

ScreenResult quantize_hires(vic2::Mode mode, const Image& image,
                             const Palette& palette,
                             const std::vector<color_space::OKLab>& palette_lab,
                             const vic2::ModeParams& params,
                             const ThresholdFn& tfn, float tstr) {
    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);

    ScreenResult result;
    result.mode = mode;
    result.background_color = 0;
    result.total_error = 0.0f;
    result.cells.resize(cx * cy);

    std::atomic<float> total_error{0.0f};

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        auto table = precompute_cell_dist(image, x, y, palette_lab, params,
                                           tfn, tstr);
        auto cell = quantize_cell_hires(table, palette);

        float current = total_error.load(std::memory_order_relaxed);
        while (!total_error.compare_exchange_weak(
            current, current + cell.error, std::memory_order_relaxed)) {}

        result.cells[idx] = std::move(cell);
    });

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
    const ThresholdFn& tfn, float tstr) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;
    auto n = static_cast<std::uint8_t>(palette.size());

    // Step 1: Precompute all cell distance tables in parallel
    std::vector<CellDistTable> tables(total_cells);

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        tables[idx] = precompute_cell_dist(image, x, y, palette_lab, params,
                                            tfn, tstr);
    });

    // Step 2: Try all 16 background colors, cells in parallel for each
    float best_total = std::numeric_limits<float>::max();
    ScreenResult best;

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

// Multicolor with frequency-based background (for sprites)
ScreenResult quantize_multicolor_frequency_bg(
    vic2::Mode mode, const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params,
    const ThresholdFn& tfn, float tstr) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;

    auto bg = find_most_common_color(image, palette, palette_lab);

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

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t, std::size_t) {
        auto cell = quantize_cell_multicolor(tables[idx], bg, palette);

        float current = total_error.load(std::memory_order_relaxed);
        while (!total_error.compare_exchange_weak(
            current, current + cell.error, std::memory_order_relaxed)) {}

        result.cells[idx] = std::move(cell);
    });

    result.total_error = total_error.load();
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
// PETSCII: brute force bg, 256 ROM chars × 15 fg colors per cell
// ---------------------------------------------------------------------------

CellResult quantize_cell_petscii(
    const CellDistTable& table, std::uint8_t bg_color,
    const Palette& palette) {

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
    const ThresholdFn& tfn, float tstr) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);
    auto total_cells = cx * cy;
    auto n = static_cast<std::uint8_t>(palette.size());

    // Precompute all cell distance tables in parallel
    std::vector<CellDistTable> tables(total_cells);
    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        tables[idx] = precompute_cell_dist(image, x, y, palette_lab, params,
                                            tfn, tstr);
    });

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
            auto cell = quantize_cell_petscii(tables[idx], bg, palette);

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
                              float threshold_strength) {
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
                              threshold, threshold_strength);

    case vic2::Mode::multicolor:
        return quantize_multicolor_bruteforce(mode, image, palette,
                                              palette_lab, params,
                                              threshold, threshold_strength);

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
                                params, threshold, threshold_strength);
    }

    std::unreachable();
}

} // namespace png2c64::quantize
