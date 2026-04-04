#include "quantize.hpp"
#include "color_space.hpp"

#include <array>
#include <atomic>
#include <cstddef>
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

struct CellEval {
    float error{};
    std::vector<std::uint8_t> assignments;
};

CellEval evaluate_cell(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    std::span<const std::uint8_t> color_indices,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params) {

    auto px = cell_x * params.cell_width;
    auto py = cell_y * params.cell_height;

    CellEval result;
    result.assignments.reserve(params.cell_width * params.cell_height);
    result.error = 0.0f;

    for (std::size_t dy = 0; dy < params.cell_height; ++dy) {
        for (std::size_t dx = 0; dx < params.cell_width; ++dx) {
            auto pixel_lab =
                color_space::linear_to_oklab(image[px + dx, py + dy]);

            float best_dist = std::numeric_limits<float>::max();
            std::uint8_t best_idx = 0;

            for (std::size_t c = 0; c < color_indices.size(); ++c) {
                auto& cl = palette_lab[color_indices[c]];
                float dL = pixel_lab.L - cl.L;
                float da = pixel_lab.a - cl.a;
                float db = pixel_lab.b - cl.b;
                float dist = dL * dL + da * da + db * db;

                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = static_cast<std::uint8_t>(c);
                }
            }

            result.assignments.push_back(best_idx);
            result.error += best_dist;
        }
    }

    return result;
}

CellResult quantize_cell_hires(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params) {

    auto n = static_cast<std::uint8_t>(palette.size());
    float best_error = std::numeric_limits<float>::max();
    CellResult best;

    for (std::uint8_t i = 0; i < n; ++i) {
        for (std::uint8_t j = i + 1; j < n; ++j) {
            std::array<std::uint8_t, 2> indices = {i, j};
            auto eval = evaluate_cell(image, cell_x, cell_y, indices,
                                       palette_lab, params);
            if (eval.error < best_error) {
                best_error = eval.error;
                best.pixel_indices = std::move(eval.assignments);
                best.cell_colors = {i, j};
                best.error = eval.error;
            }
        }
    }

    return best;
}

CellResult quantize_cell_multicolor(
    const Image& image, std::size_t cell_x, std::size_t cell_y,
    std::uint8_t bg_color, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params) {

    auto n = static_cast<std::uint8_t>(palette.size());
    float best_error = std::numeric_limits<float>::max();
    CellResult best;

    for (std::uint8_t i = 0; i < n; ++i) {
        if (i == bg_color) continue;
        for (std::uint8_t j = i + 1; j < n; ++j) {
            if (j == bg_color) continue;
            for (std::uint8_t k = j + 1; k < n; ++k) {
                if (k == bg_color) continue;

                std::array<std::uint8_t, 4> indices = {bg_color, i, j, k};
                auto eval = evaluate_cell(image, cell_x, cell_y, indices,
                                           palette_lab, params);
                if (eval.error < best_error) {
                    best_error = eval.error;
                    best.pixel_indices = std::move(eval.assignments);
                    best.cell_colors = {bg_color, i, j, k};
                    best.error = eval.error;
                }
            }
        }
    }

    return best;
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

// Find the most common nearest-palette-color across all pixels.
// Good for sprites where background is typically a large uniform area.
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
// Hires quantization (no shared background)
// ---------------------------------------------------------------------------

ScreenResult quantize_hires(vic2::Mode mode, const Image& image,
                             const Palette& palette,
                             const std::vector<color_space::OKLab>& palette_lab,
                             const vic2::ModeParams& params) {
    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);

    ScreenResult result;
    result.mode = mode;
    result.background_color = 0;
    result.total_error = 0.0f;
    result.cells.resize(cx * cy);

    std::atomic<float> total_error{0.0f};

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        auto cell = quantize_cell_hires(image, x, y, palette,
                                         palette_lab, params);
        float current = total_error.load(std::memory_order_relaxed);
        while (!total_error.compare_exchange_weak(
            current, current + cell.error, std::memory_order_relaxed)) {}

        result.cells[idx] = std::move(cell);
    });

    result.total_error = total_error.load();
    return result;
}

// ---------------------------------------------------------------------------
// Multicolor quantization with a fixed background
// ---------------------------------------------------------------------------

ScreenResult quantize_multicolor_with_bg(
    vic2::Mode mode, std::uint8_t bg, const Image& image,
    const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params) {

    auto cx = vic2::cells_x(params);
    auto cy = vic2::cells_y(params);

    ScreenResult candidate;
    candidate.mode = mode;
    candidate.background_color = bg;
    candidate.cells.resize(cx * cy);

    std::atomic<float> total_error{0.0f};

    parallel_for_cells(cx, cy, [&](std::size_t idx, std::size_t x, std::size_t y) {
        auto cell = quantize_cell_multicolor(
            image, x, y, bg, palette, palette_lab, params);

        float current = total_error.load(std::memory_order_relaxed);
        while (!total_error.compare_exchange_weak(
            current, current + cell.error, std::memory_order_relaxed)) {}

        candidate.cells[idx] = std::move(cell);
    });

    candidate.total_error = total_error.load();
    return candidate;
}

// ---------------------------------------------------------------------------
// Multicolor: brute force over all 16 backgrounds
// ---------------------------------------------------------------------------

ScreenResult quantize_multicolor_bruteforce(
    vic2::Mode mode, const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params) {

    auto n = static_cast<std::uint8_t>(palette.size());

    std::vector<ScreenResult> candidates(n);

    // Run background candidates sequentially; cells in parallel inside
    for (std::uint8_t bg = 0; bg < n; ++bg) {
        candidates[bg] = quantize_multicolor_with_bg(
            mode, bg, image, palette, palette_lab, params);
    }

    float best_total = std::numeric_limits<float>::max();
    std::size_t best_idx = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (candidates[i].total_error < best_total) {
            best_total = candidates[i].total_error;
            best_idx = i;
        }
    }

    return std::move(candidates[best_idx]);
}

// ---------------------------------------------------------------------------
// Multicolor: frequency-based background (for sprites)
// ---------------------------------------------------------------------------

ScreenResult quantize_multicolor_frequency_bg(
    vic2::Mode mode, const Image& image, const Palette& palette,
    const std::vector<color_space::OKLab>& palette_lab,
    const vic2::ModeParams& params) {

    auto bg = find_most_common_color(image, palette, palette_lab);
    return quantize_multicolor_with_bg(mode, bg, image, palette,
                                        palette_lab, params);
}

} // namespace

Result<ScreenResult> quantize(const Image& image, const Palette& palette,
                              vic2::Mode mode,
                              const vic2::ModeParams& params) {
    if (image.width() != params.screen_width ||
        image.height() != params.screen_height) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Image dimensions must match mode",
        }};
    }

    auto palette_lab = precompute_oklab(palette);

    switch (mode) {
    case vic2::Mode::hires:
    case vic2::Mode::sprite_hires:
        return quantize_hires(mode, image, palette, palette_lab, params);

    case vic2::Mode::multicolor:
        return quantize_multicolor_bruteforce(mode, image, palette,
                                              palette_lab, params);

    case vic2::Mode::sprite_multicolor:
        return quantize_multicolor_frequency_bg(mode, image, palette,
                                                palette_lab, params);
    }

    std::unreachable();
}

} // namespace png2c64::quantize
