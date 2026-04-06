#include "charset.hpp"
#include "color_space.hpp"
#include "quantize.hpp"
#include "vic2.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include "log.hpp"

#include <cstddef>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <vector>

namespace png2c64::charset {

namespace {

using OKLab = color_space::OKLab;
using Pattern = std::array<std::uint8_t, 8>;

std::vector<OKLab> precompute_lab(const Palette& palette) {
    std::vector<OKLab> lab(palette.size());
    for (std::size_t i = 0; i < palette.size(); ++i)
        lab[i] = color_space::linear_to_oklab(palette.colors[i]);
    return lab;
}

unsigned hw_threads() {
    auto n = std::thread::hardware_concurrency();
    return n > 0 ? n : 4;
}

constexpr Pattern empty_pattern{};

// ---------------------------------------------------------------------------
// Pattern distance for merge decisions.
//
// For shared-color pixels (both patterns use known global colors), we compute
// actual squared OKLab distance. For pixels involving the per-cell slot
// (bit pattern 11 in MC, bit 1 in hires), we use a fixed max penalty since
// the actual per-cell color varies per cell and we can't know the true cost.
// ---------------------------------------------------------------------------

// Hires: bit 0 = bg (shared), bit 1 = fg (per-cell, varies).
// Every differing pixel involves the per-cell slot, so we use OKLab distance
// between bg and bg (= 0 for same-slot) or a fixed penalty for bg↔fg flips.
// Effectively: Hamming distance * penalty, where penalty = OKLab(bg, avg_fg).
// Since all flips are bg↔fg, Hamming is the best we can do here.
float pattern_distance_hires(const Pattern& a, const Pattern& b) {
    float dist = 0.0f;
    for (std::size_t i = 0; i < 8; ++i)
        dist += static_cast<float>(std::popcount(
            static_cast<unsigned int>(
                static_cast<std::uint8_t>(a[i] ^ b[i]))));
    return dist;
}

// Multicolor: 00=bg, 01=mc1, 10=mc2 are known shared colors.
// 11=per-cell is unknown (varies by cell).
// For differences between two shared colors: actual OKLab distance.
// For differences involving per-cell (11): max penalty to be conservative.
float pattern_distance_multicolor(const Pattern& a, const Pattern& b,
                                   const std::array<OKLab, 3>& shared_lab,
                                   float per_cell_penalty) {
    float dist = 0.0f;
    for (std::size_t row = 0; row < 8; ++row) {
        if (a[row] == b[row]) continue;
        for (std::size_t col = 0; col < 4; ++col) {
            auto shift = static_cast<unsigned>(6 - col * 2);
            auto bits_a = static_cast<std::size_t>((a[row] >> shift) & 0x03);
            auto bits_b = static_cast<std::size_t>((b[row] >> shift) & 0x03);
            if (bits_a == bits_b) continue;

            // If either pixel uses per-cell color (11), use max penalty
            if (bits_a == 3 || bits_b == 3) {
                dist += per_cell_penalty;
            } else {
                // Both use shared colors — compute actual OKLab distance
                auto& ca = shared_lab[bits_a];
                auto& cb = shared_lab[bits_b];
                float dL = ca.L - cb.L, da = ca.a - cb.a, db = ca.b - cb.b;
                dist += dL * dL + da * da + db * db;
            }
        }
    }
    return dist;
}

// ---------------------------------------------------------------------------
// Phase 1: Color selection — determine shared colors and per-cell colors.
// Returns a quantize::ScreenResult with cell_colors and initial pixel_indices.
// ---------------------------------------------------------------------------

// Hires charset: 1 shared bg + 1 fg per cell
quantize::ScreenResult select_colors_hires(
    const Image& image, const Palette& palette,
    std::size_t cols, std::size_t rows) {

    auto n = static_cast<std::uint8_t>(palette.size());
    auto pal_lab = precompute_lab(palette);
    auto num_cells = cols * rows;

    // Precompute cell pixels in OKLab
    std::vector<std::vector<OKLab>> cells_lab(num_cells);
    for (std::size_t cy = 0; cy < rows; ++cy)
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto ci = cy * cols + cx;
            cells_lab[ci].reserve(64);
            for (std::size_t dy = 0; dy < 8; ++dy)
                for (std::size_t dx = 0; dx < 8; ++dx)
                    cells_lab[ci].push_back(
                        color_space::linear_to_oklab(
                            image[cx * 8 + dx, cy * 8 + dy]));
        }

    // Try all 16 backgrounds
    float best_total = std::numeric_limits<float>::max();
    std::uint8_t best_bg = 0;
    std::vector<std::uint8_t> best_fgs(num_cells);

    for (std::uint8_t bg = 0; bg < n; ++bg) {
        float total = 0.0f;
        std::vector<std::uint8_t> fgs(num_cells);

        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            float best_cell = std::numeric_limits<float>::max();
            std::uint8_t best_fg = bg;
            for (std::uint8_t fg = 0; fg < n; ++fg) {
                if (fg == bg) continue;
                float err = 0.0f;
                for (auto& px : cells_lab[ci]) {
                    auto& bl = pal_lab[bg];
                    auto& fl = pal_lab[fg];
                    float db = (px.L-bl.L)*(px.L-bl.L) + (px.a-bl.a)*(px.a-bl.a) + (px.b-bl.b)*(px.b-bl.b);
                    float df = (px.L-fl.L)*(px.L-fl.L) + (px.a-fl.a)*(px.a-fl.a) + (px.b-fl.b)*(px.b-fl.b);
                    err += std::min(db, df);
                }
                if (err < best_cell) { best_cell = err; best_fg = fg; }
            }
            fgs[ci] = best_fg;
            total += best_cell;
        }
        if (total < best_total) {
            best_total = total;
            best_bg = bg;
            best_fgs = std::move(fgs);
        }
    }

    // Build ScreenResult with initial nearest-color assignments
    quantize::ScreenResult screen;
    screen.mode = vic2::Mode::hires; // cell dims match
    screen.background_color = best_bg;
    screen.cells.resize(num_cells);

    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cell = screen.cells[ci];
        cell.cell_colors = {best_bg, best_fgs[ci]};
        cell.pixel_indices.resize(64);

        auto& bg_l = pal_lab[best_bg];
        auto& fg_l = pal_lab[best_fgs[ci]];
        for (std::size_t pi = 0; pi < 64; ++pi) {
            auto& px = cells_lab[ci][pi];
            float db = (px.L-bg_l.L)*(px.L-bg_l.L) + (px.a-bg_l.a)*(px.a-bg_l.a) + (px.b-bg_l.b)*(px.b-bg_l.b);
            float df = (px.L-fg_l.L)*(px.L-fg_l.L) + (px.a-fg_l.a)*(px.a-fg_l.a) + (px.b-fg_l.b)*(px.b-fg_l.b);
            cell.pixel_indices[pi] = (df < db) ? 1 : 0;
        }
    }

    return screen;
}

// Multicolor charset: 3 shared (bg, mc1, mc2) + 1 per cell
struct McColorSelection {
    quantize::ScreenResult screen;
    std::uint8_t bg{}, mc1{}, mc2{};
};

McColorSelection select_colors_multicolor(
    const Image& image, const Palette& palette,
    std::size_t cols, std::size_t rows) {

    auto n = static_cast<std::uint8_t>(palette.size());
    auto pal_lab = precompute_lab(palette);
    auto num_cells = cols * rows;

    std::vector<std::vector<OKLab>> cells_lab(num_cells);
    for (std::size_t cy = 0; cy < rows; ++cy)
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto ci = cy * cols + cx;
            cells_lab[ci].reserve(32);
            for (std::size_t dy = 0; dy < 8; ++dy)
                for (std::size_t dx = 0; dx < 4; ++dx)
                    cells_lab[ci].push_back(
                        color_space::linear_to_oklab(
                            image[cx * 4 + dx, cy * 8 + dy]));
        }

    // Try all C(16,3) triples
    struct TripleResult {
        std::uint8_t c0, c1, c2;
        float total_error;
        std::vector<std::uint8_t> per_cell;
    };

    std::vector<TripleResult> candidates;
    candidates.reserve(560);
    for (std::uint8_t i = 0; i < n; ++i)
        for (std::uint8_t j = i + 1; j < n; ++j)
            for (std::uint8_t k = j + 1; k < n; ++k)
                candidates.push_back({i, j, k, 0.0f, {}});

    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        while (true) {
            auto ti = next.fetch_add(1, std::memory_order_relaxed);
            if (ti >= candidates.size()) break;
            auto& cand = candidates[ti];
            cand.per_cell.resize(num_cells);
            cand.total_error = 0.0f;
            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                float best_cell = std::numeric_limits<float>::max();
                std::uint8_t best_pc = 0;
                // VIC-II MC text mode: color RAM is 3 bits (0-7) + bit 3 = MC enable
                for (std::uint8_t pc = 0; pc < 8; ++pc) {
                    if (pc == cand.c0 || pc == cand.c1 || pc == cand.c2) continue;
                    std::array<std::uint8_t, 4> colors = {cand.c0, cand.c1, cand.c2, pc};
                    float err = 0.0f;
                    for (auto& px : cells_lab[ci]) {
                        float best = std::numeric_limits<float>::max();
                        for (auto c : colors) {
                            auto& cl = pal_lab[c];
                            float d = (px.L-cl.L)*(px.L-cl.L) + (px.a-cl.a)*(px.a-cl.a) + (px.b-cl.b)*(px.b-cl.b);
                            if (d < best) best = d;
                        }
                        err += best;
                    }
                    if (err < best_cell) { best_cell = err; best_pc = pc; }
                }
                cand.per_cell[ci] = best_pc;
                cand.total_error += best_cell;
            }
        }
    };

    {
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), candidates.size());
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(worker);
        worker();
    }

    auto best_it = std::ranges::min_element(candidates, {}, &TripleResult::total_error);

    // Assign bg = most common shared color
    std::array<std::uint8_t, 3> shared = {best_it->c0, best_it->c1, best_it->c2};
    std::array<std::size_t, 3> counts{};
    for (auto& cell : cells_lab)
        for (auto& px : cell) {
            int nearest = 0;
            float bd = std::numeric_limits<float>::max();
            for (int s = 0; s < 3; ++s) {
                auto& c = pal_lab[shared[static_cast<std::size_t>(s)]];
                float d = (px.L-c.L)*(px.L-c.L) + (px.a-c.a)*(px.a-c.a) + (px.b-c.b)*(px.b-c.b);
                if (d < bd) { bd = d; nearest = s; }
            }
            ++counts[static_cast<std::size_t>(nearest)];
        }

    std::array<std::size_t, 3> order = {0, 1, 2};
    std::ranges::sort(order, [&](auto a, auto b) { return counts[a] > counts[b]; });

    auto bg_color  = shared[order[0]];
    auto mc1_color = shared[order[1]];
    auto mc2_color = shared[order[2]];

    // Build ScreenResult: cell_colors ordered as [bg, mc1, mc2, per_cell]
    // so pixel_index maps directly to bit pattern (0=00, 1=01, 2=10, 3=11)
    quantize::ScreenResult screen;
    screen.mode = vic2::Mode::multicolor;
    screen.background_color = bg_color;
    screen.cells.resize(num_cells);

    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cell = screen.cells[ci];
        auto pc = best_it->per_cell[ci];
        cell.cell_colors = {bg_color, mc1_color, mc2_color, pc};
        cell.pixel_indices.resize(32);

        for (std::size_t pi = 0; pi < 32; ++pi) {
            auto& px = cells_lab[ci][pi];
            float best_d = std::numeric_limits<float>::max();
            std::uint8_t best_idx = 0;
            for (std::uint8_t c = 0; c < 4; ++c) {
                auto& cl = pal_lab[cell.cell_colors[c]];
                float d = (px.L-cl.L)*(px.L-cl.L) + (px.a-cl.a)*(px.a-cl.a) + (px.b-cl.b)*(px.b-cl.b);
                if (d < best_d) { best_d = d; best_idx = c; }
            }
            cell.pixel_indices[pi] = best_idx;
        }
    }

    return {std::move(screen), bg_color, mc1_color, mc2_color};
}

// ---------------------------------------------------------------------------
// Cell quantization result (shared by all modes)
// ---------------------------------------------------------------------------

struct CellQuant {
    Pattern pattern{};
    std::uint8_t color_ram{};
    bool is_hires{};
};

// ---------------------------------------------------------------------------
// Mixed mode: per-cell hires/multicolor decision
// Input is full 320-wide resolution. MC cells average pixel pairs.
// ---------------------------------------------------------------------------

struct MixedColorSelection {
    quantize::ScreenResult screen;
    std::uint8_t bg{}, mc1{}, mc2{};
    std::vector<bool> cell_is_hires;
};

MixedColorSelection select_colors_mixed(
    const Image& image, const Palette& palette,
    std::size_t cols, std::size_t rows) {

    auto n = static_cast<std::uint8_t>(palette.size());
    auto pal_lab = precompute_lab(palette);
    auto num_cells = cols * rows;

    // Precompute cell pixels in OKLab — both hires (8x8) and MC (4x8, averaged pairs)
    std::vector<std::vector<OKLab>> cells_hi(num_cells);
    std::vector<std::vector<OKLab>> cells_mc(num_cells);
    for (std::size_t cy = 0; cy < rows; ++cy)
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto ci = cy * cols + cx;
            cells_hi[ci].reserve(64);
            cells_mc[ci].reserve(32);
            for (std::size_t dy = 0; dy < 8; ++dy) {
                for (std::size_t dx = 0; dx < 8; ++dx)
                    cells_hi[ci].push_back(
                        color_space::linear_to_oklab(
                            image[cx * 8 + dx, cy * 8 + dy]));
                // MC pixels: average pairs of horizontal pixels in linear space
                for (std::size_t dx = 0; dx < 4; ++dx) {
                    auto p0 = image[cx * 8 + dx * 2, cy * 8 + dy];
                    auto p1 = image[cx * 8 + dx * 2 + 1, cy * 8 + dy];
                    Color3f avg{(p0.r + p1.r) * 0.5f,
                                (p0.g + p1.g) * 0.5f,
                                (p0.b + p1.b) * 0.5f};
                    cells_mc[ci].push_back(color_space::linear_to_oklab(avg));
                }
            }
        }

    // Try all C(16,3) triples for shared colors
    struct TripleResult {
        std::uint8_t c0, c1, c2;
        float total_error;
        std::vector<std::uint8_t> per_cell; // fg (hires) or per-cell color (mc)
        std::vector<bool> is_hires;
    };

    std::vector<TripleResult> candidates;
    candidates.reserve(560);
    for (std::uint8_t i = 0; i < n; ++i)
        for (std::uint8_t j = i + 1; j < n; ++j)
            for (std::uint8_t k = j + 1; k < n; ++k)
                candidates.push_back({i, j, k, 0.0f, {}, {}});

    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        while (true) {
            auto ti = next.fetch_add(1, std::memory_order_relaxed);
            if (ti >= candidates.size()) break;
            auto& cand = candidates[ti];
            cand.per_cell.resize(num_cells);
            cand.is_hires.resize(num_cells);
            cand.total_error = 0.0f;

            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                // Try MC: best per-cell color from 0-7
                float best_mc_err = std::numeric_limits<float>::max();
                std::uint8_t best_mc_pc = 0;
                for (std::uint8_t pc = 0; pc < 8; ++pc) {
                    if (pc == cand.c0 || pc == cand.c1 || pc == cand.c2) continue;
                    std::array<std::uint8_t, 4> colors = {cand.c0, cand.c1, cand.c2, pc};
                    float err = 0.0f;
                    for (auto& px : cells_mc[ci]) {
                        float best = std::numeric_limits<float>::max();
                        for (auto c : colors) {
                            auto& cl = pal_lab[c];
                            float d = (px.L-cl.L)*(px.L-cl.L) + (px.a-cl.a)*(px.a-cl.a) + (px.b-cl.b)*(px.b-cl.b);
                            if (d < best) best = d;
                        }
                        err += best;
                    }
                    if (err < best_mc_err) { best_mc_err = err; best_mc_pc = pc; }
                }

                // Try hires: best bg from shared triple + best fg from all 16
                // Try each shared color as potential background
                float best_hi_err = std::numeric_limits<float>::max();
                std::uint8_t best_hi_fg = 0;
                for (auto bg_cand : {cand.c0, cand.c1, cand.c2}) {
                    for (std::uint8_t fg = 0; fg < n; ++fg) {
                        if (fg == bg_cand) continue;
                        float err = 0.0f;
                        for (auto& px : cells_hi[ci]) {
                            auto& bl = pal_lab[bg_cand];
                            auto& fl = pal_lab[fg];
                            float db = (px.L-bl.L)*(px.L-bl.L) + (px.a-bl.a)*(px.a-bl.a) + (px.b-bl.b)*(px.b-bl.b);
                            float df = (px.L-fl.L)*(px.L-fl.L) + (px.a-fl.a)*(px.a-fl.a) + (px.b-fl.b)*(px.b-fl.b);
                            err += std::min(db, df);
                        }
                        if (err < best_hi_err) { best_hi_err = err; best_hi_fg = fg; }
                    }
                }

                // Normalize per-pixel: hires has 64 pixels, MC has 32
                // Compare average error per pixel for a fair mode decision
                auto hi_avg = best_hi_err / 64.0f;
                auto mc_avg = best_mc_err / 32.0f;
                if (hi_avg <= mc_avg) {
                    cand.is_hires[ci] = true;
                    cand.per_cell[ci] = best_hi_fg;
                    cand.total_error += best_hi_err;
                } else {
                    cand.is_hires[ci] = false;
                    cand.per_cell[ci] = best_mc_pc;
                    cand.total_error += best_mc_err;
                }
            }
        }
    };

    {
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), candidates.size());
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(worker);
        worker();
    }

    auto best_it = std::ranges::min_element(candidates, {}, &TripleResult::total_error);

    // Assign bg = most common shared color (same as MC path)
    std::array<std::uint8_t, 3> shared = {best_it->c0, best_it->c1, best_it->c2};
    std::array<std::size_t, 3> counts{};
    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        if (best_it->is_hires[ci]) continue; // only count MC cells for bg selection
        for (auto& px : cells_mc[ci]) {
            int nearest = 0;
            float bd = std::numeric_limits<float>::max();
            for (int s = 0; s < 3; ++s) {
                auto& c = pal_lab[shared[static_cast<std::size_t>(s)]];
                float d = (px.L-c.L)*(px.L-c.L) + (px.a-c.a)*(px.a-c.a) + (px.b-c.b)*(px.b-c.b);
                if (d < bd) { bd = d; nearest = s; }
            }
            ++counts[static_cast<std::size_t>(nearest)];
        }
    }

    std::array<std::size_t, 3> order = {0, 1, 2};
    std::ranges::sort(order, [&](auto a, auto b) { return counts[a] > counts[b]; });

    auto bg_color  = shared[order[0]];
    auto mc1_color = shared[order[1]];
    auto mc2_color = shared[order[2]];

    // Build ScreenResult with per-cell modes
    quantize::ScreenResult screen;
    screen.mode = vic2::Mode::multicolor; // VIC-II is in MC text mode
    screen.background_color = bg_color;
    screen.cells.resize(num_cells);

    std::vector<bool> cell_is_hires(num_cells);

    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cell = screen.cells[ci];
        auto is_hi = best_it->is_hires[ci];
        cell_is_hires[ci] = is_hi;

        if (is_hi) {
            auto fg = best_it->per_cell[ci];
            cell.cell_colors = {bg_color, fg};
            cell.pixel_indices.resize(64);
            auto& bg_l = pal_lab[bg_color];
            auto& fg_l = pal_lab[fg];
            for (std::size_t pi = 0; pi < 64; ++pi) {
                auto& px = cells_hi[ci][pi];
                float db = (px.L-bg_l.L)*(px.L-bg_l.L) + (px.a-bg_l.a)*(px.a-bg_l.a) + (px.b-bg_l.b)*(px.b-bg_l.b);
                float df = (px.L-fg_l.L)*(px.L-fg_l.L) + (px.a-fg_l.a)*(px.a-fg_l.a) + (px.b-fg_l.b)*(px.b-fg_l.b);
                cell.pixel_indices[pi] = (df < db) ? 1 : 0;
            }
        } else {
            auto pc = best_it->per_cell[ci];
            cell.cell_colors = {bg_color, mc1_color, mc2_color, pc};
            cell.pixel_indices.resize(32);
            for (std::size_t pi = 0; pi < 32; ++pi) {
                auto& px = cells_mc[ci][pi];
                float best_d = std::numeric_limits<float>::max();
                std::uint8_t best_idx = 0;
                for (std::uint8_t c = 0; c < 4; ++c) {
                    auto& cl = pal_lab[cell.cell_colors[c]];
                    float d = (px.L-cl.L)*(px.L-cl.L) + (px.a-cl.a)*(px.a-cl.a) + (px.b-cl.b)*(px.b-cl.b);
                    if (d < best_d) { best_d = d; best_idx = c; }
                }
                cell.pixel_indices[pi] = best_idx;
            }
        }
    }

    return {std::move(screen), bg_color, mc1_color, mc2_color, std::move(cell_is_hires)};
}

// Encode mixed-mode patterns from a ScreenResult with per-cell modes
std::vector<CellQuant> encode_patterns_mixed(
    const quantize::ScreenResult& screen, std::size_t num_cells,
    const std::vector<bool>& cell_is_hires) {

    std::vector<CellQuant> cells(num_cells);
    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cell = screen.cells[ci];
        cells[ci].is_hires = cell_is_hires[ci];

        if (cell_is_hires[ci]) {
            cells[ci].color_ram = cell.cell_colors[1]; // fg
            for (std::size_t row = 0; row < 8; ++row) {
                std::uint8_t byte = 0;
                for (std::size_t col = 0; col < 8; ++col) {
                    auto pi = row * 8 + col;
                    if (cell.pixel_indices[pi] == 1)
                        byte |= static_cast<std::uint8_t>(0x80 >> col);
                }
                cells[ci].pattern[row] = byte;
            }
        } else {
            cells[ci].color_ram = cell.cell_colors[3]; // per-cell MC color
            for (std::size_t row = 0; row < 8; ++row) {
                std::uint8_t byte = 0;
                for (std::size_t col = 0; col < 4; ++col) {
                    auto pi = row * 4 + col;
                    auto bits = cell.pixel_indices[pi];
                    byte |= static_cast<std::uint8_t>(bits << (6 - col * 2));
                }
                cells[ci].pattern[row] = byte;
            }
        }
    }
    return cells;
}

// ---------------------------------------------------------------------------
// Phase 3: Encode dithered pixel assignments into 8-byte patterns
// ---------------------------------------------------------------------------

std::vector<CellQuant> encode_patterns_hires(
    const quantize::ScreenResult& screen, std::size_t num_cells) {

    std::vector<CellQuant> cells(num_cells);
    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cell = screen.cells[ci];
        cells[ci].color_ram = cell.cell_colors[1];
        cells[ci].is_hires = true;

        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t byte = 0;
            for (std::size_t col = 0; col < 8; ++col) {
                auto pi = row * 8 + col;
                if (cell.pixel_indices[pi] == 1)
                    byte |= static_cast<std::uint8_t>(0x80 >> col);
            }
            cells[ci].pattern[row] = byte;
        }
    }
    return cells;
}

std::vector<CellQuant> encode_patterns_multicolor(
    const quantize::ScreenResult& screen, std::size_t num_cells) {

    std::vector<CellQuant> cells(num_cells);
    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cell = screen.cells[ci];
        cells[ci].color_ram = cell.cell_colors[3];
        cells[ci].is_hires = false;

        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t byte = 0;
            for (std::size_t col = 0; col < 4; ++col) {
                auto pi = row * 4 + col;
                auto bits = cell.pixel_indices[pi]; // 0-3 maps to 00,01,10,11
                byte |= static_cast<std::uint8_t>(bits << (6 - col * 2));
            }
            cells[ci].pattern[row] = byte;
        }
    }
    return cells;
}

// ---------------------------------------------------------------------------
// Dedup + merge
// ---------------------------------------------------------------------------

struct CharEntry {
    Pattern pattern;
    bool is_hires{};
    std::vector<std::size_t> cell_indices;
    bool alive = true;
};

struct DeduplicatedCharset {
    std::vector<CharEntry> entries;
    std::vector<std::size_t> cell_to_entry;
};

DeduplicatedCharset deduplicate(const std::vector<CellQuant>& cells) {
    DeduplicatedCharset result;
    result.cell_to_entry.resize(cells.size());

    using PatternKey = std::pair<Pattern, bool>;
    std::map<PatternKey, std::size_t> pattern_map;
    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        PatternKey key{cells[ci].pattern, cells[ci].is_hires};
        auto [it, inserted] = pattern_map.try_emplace(key, result.entries.size());
        if (inserted) {
            auto& e = result.entries.emplace_back();
            e.pattern = cells[ci].pattern;
            e.is_hires = cells[ci].is_hires;
            e.cell_indices.push_back(ci);
        } else
            result.entries[it->second].cell_indices.push_back(ci);
        result.cell_to_entry[ci] = it->second;
    }

    return result;
}

// dist_func: callable(std::size_t entry_a, std::size_t entry_b) -> float
void merge_to_256(DeduplicatedCharset& dedup, auto dist_func) {
    std::vector<std::size_t> alive;
    for (std::size_t i = 0; i < dedup.entries.size(); ++i)
        if (dedup.entries[i].alive) alive.push_back(i);

    if (alive.size() <= 256) return;

    auto merges_needed = alive.size() - 256;

    struct Pair {
        std::size_t a, b;
        float distance;
    };

    auto num_pairs = alive.size() * (alive.size() - 1) / 2;
    std::vector<Pair> pairs(num_pairs);

    std::atomic<std::size_t> next_row{0};
    auto pair_worker = [&] {
        while (true) {
            auto row = next_row.fetch_add(1, std::memory_order_relaxed);
            if (row >= alive.size()) break;
            auto base = row * alive.size() - row * (row + 1) / 2;
            auto ai = alive[row];
            for (std::size_t col = row + 1; col < alive.size(); ++col) {
                auto bi = alive[col];
                pairs[base + (col - row - 1)] = {ai, bi,
                    dist_func(ai, bi)};
            }
        }
    };

    {
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), alive.size());
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(pair_worker);
        pair_worker();
    }

    log_println("  Sorting {} pairs...", num_pairs);
    std::ranges::sort(pairs, {}, &Pair::distance);

    std::vector<bool> is_alive(dedup.entries.size(), false);
    for (auto i : alive) is_alive[i] = true;

    std::size_t merges_done = 0;
    for (auto& p : pairs) {
        if (merges_done >= merges_needed) break;
        if (!is_alive[p.a] || !is_alive[p.b]) continue;

        auto keep = p.a;
        auto discard = p.b;
        if (dedup.entries[keep].cell_indices.size() <
            dedup.entries[discard].cell_indices.size())
            std::swap(keep, discard);

        for (auto ci : dedup.entries[discard].cell_indices) {
            dedup.cell_to_entry[ci] = keep;
            dedup.entries[keep].cell_indices.push_back(ci);
        }
        dedup.entries[discard].cell_indices.clear();
        dedup.entries[discard].alive = false;
        is_alive[discard] = false;
        ++merges_done;

        if (merges_done % 100 == 0 || merges_done == merges_needed) {
            log_print("\r  Merged {}/{}", merges_done, merges_needed);
            log_flush();
        }
    }
    log_println("");
}

// ---------------------------------------------------------------------------
// Iterative refinement (k-means on charset patterns)
// ---------------------------------------------------------------------------

// Compute error of a cell using a given pattern and per-cell color.
// cell_lab: precomputed OKLab pixels for this cell (cell_w * 8 elements).
float cell_pattern_error(
    std::span<const OKLab> cell_lab, const Pattern& pat,
    const std::vector<OKLab>& pal_lab,
    bool multicolor,
    std::uint8_t bg, std::uint8_t mc1, std::uint8_t mc2,
    std::uint8_t per_cell_color) {

    float err = 0.0f;

    if (multicolor) {
        std::array<OKLab, 4> colors = {
            pal_lab[bg], pal_lab[mc1], pal_lab[mc2], pal_lab[per_cell_color]};
        for (std::size_t row = 0; row < 8; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                auto bits = static_cast<std::size_t>(
                    (pat[row] >> (6 - col * 2)) & 0x03);
                auto& c = colors[bits];
                auto& px = cell_lab[row * 4 + col];
                float dL = px.L - c.L, da = px.a - c.a, db = px.b - c.b;
                err += dL * dL + da * da + db * db;
            }
        }
    } else {
        auto& bg_l = pal_lab[bg];
        auto& fg_l = pal_lab[per_cell_color];
        for (std::size_t row = 0; row < 8; ++row) {
            for (std::size_t col = 0; col < 8; ++col) {
                bool is_fg = (pat[row] >> (7 - col)) & 1;
                auto& c = is_fg ? fg_l : bg_l;
                auto& px = cell_lab[row * 8 + col];
                float dL = px.L - c.L, da = px.a - c.a, db = px.b - c.b;
                err += dL * dL + da * da + db * db;
            }
        }
    }

    return err;
}

void refine_charset(
    const Image& image, const Palette& palette,
    bool multicolor,
    std::uint8_t bg, std::uint8_t mc1, std::uint8_t mc2,
    std::size_t cols, std::size_t rows,
    std::array<Pattern, 256>& patterns,
    std::vector<std::uint8_t>& assignments,  // cell -> pattern idx
    std::vector<std::uint8_t>& color_ram,    // cell -> per-cell color
    int max_iters,
    bool recompute_centroids) {

    auto pal_lab = precompute_lab(palette);
    auto n_pal = static_cast<std::uint8_t>(palette.size());
    auto cell_w = multicolor ? std::size_t{4} : std::size_t{8};
    auto num_cells = cols * rows;
    auto pixels_per_cell = cell_w * 8;

    // Precompute all cell pixels in OKLab
    std::vector<std::vector<OKLab>> cells_lab(num_cells);
    for (std::size_t cy = 0; cy < rows; ++cy)
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto ci = cy * cols + cx;
            cells_lab[ci].reserve(pixels_per_cell);
            for (std::size_t dy = 0; dy < 8; ++dy)
                for (std::size_t dx = 0; dx < cell_w; ++dx)
                    cells_lab[ci].push_back(color_space::linear_to_oklab(
                        image[cx * cell_w + dx, cy * 8 + dy]));
        }

    for (int iter = 0; iter < max_iters; ++iter) {
        std::size_t changes = 0;

        // Step 1: Reassign each cell to best pattern (with current per-cell color)
        std::atomic<std::size_t> next_cell{0};
        std::atomic<std::size_t> atomic_changes{0};

        auto assign_worker = [&] {
            while (true) {
                auto ci = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;

                auto current_pc = color_ram[ci];
                float best_err = cell_pattern_error(
                    cells_lab[ci], patterns[assignments[ci]], pal_lab,
                    multicolor, bg, mc1, mc2, current_pc);
                auto best_pat = assignments[ci];

                for (std::size_t p = 0; p < 256; ++p) {
                    if (p == assignments[ci]) continue;
                    float err = cell_pattern_error(
                        cells_lab[ci], patterns[p], pal_lab,
                        multicolor, bg, mc1, mc2, current_pc);
                    if (err < best_err) {
                        best_err = err;
                        best_pat = static_cast<std::uint8_t>(p);
                    }
                }

                if (best_pat != assignments[ci]) {
                    assignments[ci] = best_pat;
                    atomic_changes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        next_cell = 0;
        {
            auto nt = std::min(static_cast<std::size_t>(hw_threads()), num_cells);
            std::vector<std::jthread> threads;
            threads.reserve(nt - 1);
            for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(assign_worker);
            assign_worker();
        }
        changes = atomic_changes.load();

        // Step 2: Re-optimize per-cell color for each cell's current pattern
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            float best_err = std::numeric_limits<float>::max();
            std::uint8_t best_pc = color_ram[ci];

            // VIC-II MC text mode: color RAM is 3 bits (0-7)
            auto pc_limit = multicolor ? std::uint8_t{8} : n_pal;
            for (std::uint8_t pc = 0; pc < pc_limit; ++pc) {
                if (multicolor && (pc == bg || pc == mc1 || pc == mc2)) continue;
                if (!multicolor && pc == bg) continue;

                float err = cell_pattern_error(
                    cells_lab[ci], patterns[assignments[ci]], pal_lab,
                    multicolor, bg, mc1, mc2, pc);
                if (err < best_err) { best_err = err; best_pc = pc; }
            }

            if (best_pc != color_ram[ci]) {
                color_ram[ci] = best_pc;
                ++changes;
            }
        }

        // Step 3: Recompute centroids — optimize each pixel of each pattern.
        // Only when recompute_centroids is set (skipped when dithering is active
        // to preserve dither patterns).
        if (recompute_centroids) {
            std::array<std::vector<std::size_t>, 256> pat_cells;
            for (std::size_t ci = 0; ci < num_cells; ++ci)
                pat_cells[assignments[ci]].push_back(ci);

            for (std::size_t p = 0; p < 256; ++p) {
                if (pat_cells[p].empty()) continue;

                Pattern new_pat{};
                if (multicolor) {
                    for (std::size_t row = 0; row < 8; ++row) {
                        std::uint8_t byte = 0;
                        for (std::size_t col = 0; col < 4; ++col) {
                            auto pi = row * 4 + col;
                            float best_bit_err = std::numeric_limits<float>::max();
                            std::uint8_t best_bits = 0;

                            for (std::uint8_t bits = 0; bits < 4; ++bits) {
                                float total = 0.0f;
                                for (auto ci : pat_cells[p]) {
                                    std::array<std::uint8_t, 4> pal_indices = {
                                        bg, mc1, mc2, color_ram[ci]};
                                    auto& c = pal_lab[pal_indices[bits]];
                                    auto& px = cells_lab[ci][pi];
                                    float dL = px.L-c.L, da = px.a-c.a, db = px.b-c.b;
                                    total += dL*dL + da*da + db*db;
                                }
                                if (total < best_bit_err) {
                                    best_bit_err = total;
                                    best_bits = bits;
                                }
                            }
                            byte |= static_cast<std::uint8_t>(
                                best_bits << (6 - col * 2));
                        }
                        new_pat[row] = byte;
                    }
                } else {
                    for (std::size_t row = 0; row < 8; ++row) {
                        std::uint8_t byte = 0;
                        for (std::size_t col = 0; col < 8; ++col) {
                            auto pi = row * 8 + col;
                            float err_bg = 0.0f, err_fg = 0.0f;
                            for (auto ci : pat_cells[p]) {
                                auto& px = cells_lab[ci][pi];
                                auto& bl = pal_lab[bg];
                                auto& fl = pal_lab[color_ram[ci]];
                                float dL, da, db;
                                dL = px.L-bl.L; da = px.a-bl.a; db = px.b-bl.b;
                                err_bg += dL*dL + da*da + db*db;
                                dL = px.L-fl.L; da = px.a-fl.a; db = px.b-fl.b;
                                err_fg += dL*dL + da*da + db*db;
                            }
                            if (err_fg < err_bg)
                                byte |= static_cast<std::uint8_t>(0x80 >> col);
                        }
                        new_pat[row] = byte;
                    }
                }

                if (new_pat != patterns[p]) {
                    patterns[p] = new_pat;
                    ++changes;
                }
            }
        }

        log_print("\r  Refine iter {}: {} changes", iter + 1, changes);
        log_flush();

        if (changes == 0) break;
    }
    log_println("");
}

// ---------------------------------------------------------------------------
// Mixed-mode refinement: cells only assigned to patterns of matching mode
// ---------------------------------------------------------------------------

void refine_charset_mixed(
    const Image& image, const Palette& palette,
    std::uint8_t bg, std::uint8_t mc1, std::uint8_t mc2,
    std::size_t cols, std::size_t rows,
    std::array<Pattern, 256>& patterns,
    std::array<bool, 256>& pattern_is_hires,
    std::vector<std::uint8_t>& assignments,
    std::vector<std::uint8_t>& color_ram,
    const std::vector<bool>& cell_is_hires,
    int max_iters, bool recompute_centroids) {

    auto pal_lab = precompute_lab(palette);
    auto n_pal = static_cast<std::uint8_t>(palette.size());
    auto num_cells = cols * rows;

    // Precompute cell pixels: hires=64 OKLab values, MC=32 (averaged pairs)
    std::vector<std::vector<OKLab>> cells_lab(num_cells);
    for (std::size_t cy = 0; cy < rows; ++cy)
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto ci = cy * cols + cx;
            if (cell_is_hires[ci]) {
                cells_lab[ci].reserve(64);
                for (std::size_t dy = 0; dy < 8; ++dy)
                    for (std::size_t dx = 0; dx < 8; ++dx)
                        cells_lab[ci].push_back(color_space::linear_to_oklab(
                            image[cx * 8 + dx, cy * 8 + dy]));
            } else {
                cells_lab[ci].reserve(32);
                for (std::size_t dy = 0; dy < 8; ++dy)
                    for (std::size_t dx = 0; dx < 4; ++dx) {
                        auto p0 = image[cx * 8 + dx * 2, cy * 8 + dy];
                        auto p1 = image[cx * 8 + dx * 2 + 1, cy * 8 + dy];
                        Color3f avg{(p0.r + p1.r) * 0.5f,
                                    (p0.g + p1.g) * 0.5f,
                                    (p0.b + p1.b) * 0.5f};
                        cells_lab[ci].push_back(color_space::linear_to_oklab(avg));
                    }
            }
        }

    for (int iter = 0; iter < max_iters; ++iter) {
        std::size_t changes = 0;

        // Step 1: Reassign each cell to best pattern of matching mode
        std::atomic<std::size_t> next_cell{0};
        std::atomic<std::size_t> atomic_changes{0};

        auto assign_worker = [&] {
            while (true) {
                auto ci = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;

                bool is_hi = cell_is_hires[ci];
                auto current_pc = color_ram[ci];
                float best_err = cell_pattern_error(
                    cells_lab[ci], patterns[assignments[ci]], pal_lab,
                    !is_hi, bg, mc1, mc2, current_pc);
                auto best_pat = assignments[ci];

                for (std::size_t p = 0; p < 256; ++p) {
                    if (p == assignments[ci]) continue;
                    if (pattern_is_hires[p] != is_hi) continue; // mode must match
                    float err = cell_pattern_error(
                        cells_lab[ci], patterns[p], pal_lab,
                        !is_hi, bg, mc1, mc2, current_pc);
                    if (err < best_err) {
                        best_err = err;
                        best_pat = static_cast<std::uint8_t>(p);
                    }
                }

                if (best_pat != assignments[ci]) {
                    assignments[ci] = best_pat;
                    atomic_changes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        next_cell = 0;
        {
            auto nt = std::min(static_cast<std::size_t>(hw_threads()), num_cells);
            std::vector<std::jthread> threads;
            threads.reserve(nt - 1);
            for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(assign_worker);
            assign_worker();
        }
        changes = atomic_changes.load();

        // Step 2: Re-optimize per-cell color
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            float best_err = std::numeric_limits<float>::max();
            std::uint8_t best_pc = color_ram[ci];
            bool is_hi = cell_is_hires[ci];

            auto pc_limit = is_hi ? n_pal : std::uint8_t{8};
            for (std::uint8_t pc = 0; pc < pc_limit; ++pc) {
                if (is_hi && pc == bg) continue;
                if (!is_hi && (pc == bg || pc == mc1 || pc == mc2)) continue;

                float err = cell_pattern_error(
                    cells_lab[ci], patterns[assignments[ci]], pal_lab,
                    !is_hi, bg, mc1, mc2, pc);
                if (err < best_err) { best_err = err; best_pc = pc; }
            }

            if (best_pc != color_ram[ci]) {
                color_ram[ci] = best_pc;
                ++changes;
            }
        }

        // Step 3: Recompute centroids (when enabled)
        if (recompute_centroids) {
            std::array<std::vector<std::size_t>, 256> pat_cells;
            for (std::size_t ci = 0; ci < num_cells; ++ci)
                pat_cells[assignments[ci]].push_back(ci);

            for (std::size_t p = 0; p < 256; ++p) {
                if (pat_cells[p].empty()) continue;

                Pattern new_pat{};
                if (!pattern_is_hires[p]) {
                    // MC centroid
                    for (std::size_t row = 0; row < 8; ++row) {
                        std::uint8_t byte = 0;
                        for (std::size_t col = 0; col < 4; ++col) {
                            auto pi = row * 4 + col;
                            float best_bit_err = std::numeric_limits<float>::max();
                            std::uint8_t best_bits = 0;
                            for (std::uint8_t bits = 0; bits < 4; ++bits) {
                                float total = 0.0f;
                                for (auto ci : pat_cells[p]) {
                                    std::array<std::uint8_t, 4> pal_indices = {
                                        bg, mc1, mc2, color_ram[ci]};
                                    auto& c = pal_lab[pal_indices[bits]];
                                    auto& px = cells_lab[ci][pi];
                                    float dL = px.L-c.L, da = px.a-c.a, db = px.b-c.b;
                                    total += dL*dL + da*da + db*db;
                                }
                                if (total < best_bit_err) {
                                    best_bit_err = total;
                                    best_bits = bits;
                                }
                            }
                            byte |= static_cast<std::uint8_t>(
                                best_bits << (6 - col * 2));
                        }
                        new_pat[row] = byte;
                    }
                } else {
                    // Hires centroid
                    for (std::size_t row = 0; row < 8; ++row) {
                        std::uint8_t byte = 0;
                        for (std::size_t col = 0; col < 8; ++col) {
                            auto pi = row * 8 + col;
                            float err_bg = 0.0f, err_fg = 0.0f;
                            for (auto ci : pat_cells[p]) {
                                auto& px = cells_lab[ci][pi];
                                auto& bl = pal_lab[bg];
                                auto& fl = pal_lab[color_ram[ci]];
                                float dL, da, db;
                                dL = px.L-bl.L; da = px.a-bl.a; db = px.b-bl.b;
                                err_bg += dL*dL + da*da + db*db;
                                dL = px.L-fl.L; da = px.a-fl.a; db = px.b-fl.b;
                                err_fg += dL*dL + da*da + db*db;
                            }
                            if (err_fg < err_bg)
                                byte |= static_cast<std::uint8_t>(0x80 >> col);
                        }
                        new_pat[row] = byte;
                    }
                }

                if (new_pat != patterns[p]) {
                    patterns[p] = new_pat;
                    ++changes;
                }
            }
        }

        log_print("\r  Refine iter {}: {} changes", iter + 1, changes);
        log_flush();
        if (changes == 0) break;
    }
    log_println("");
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<CharsetResult> convert(const Image& image_in, const Palette& palette,
                              CharsetMode mode,
                              const dither::Settings& dither_settings) {
    bool multicolor = (mode == CharsetMode::multicolor);
    bool mixed = (mode == CharsetMode::mixed);
    // Mixed mode uses full 8-pixel-wide cells (like hires)
    std::size_t cell_w = (multicolor && !mixed) ? 4 : 8;
    std::size_t cell_h = 8;

    if (image_in.width() % cell_w != 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            std::format("Image width must be a multiple of {}", cell_w),
        }};
    }

    if (image_in.width() == 0 || image_in.height() == 0) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions, "Image has no pixels",
        }};
    }

    // Pad height to next multiple of 8 if needed
    auto padded_h = (image_in.height() + cell_h - 1) / cell_h * cell_h;
    const Image* image_ptr = &image_in;
    Image padded;
    if (image_in.height() != padded_h) {
        log_println("  Padding height {} -> {} (next multiple of 8)",
                     image_in.height(), padded_h);
        padded = Image(image_in.width(), padded_h);
        for (std::size_t y = 0; y < image_in.height(); ++y)
            for (std::size_t x = 0; x < image_in.width(); ++x)
                padded[x, y] = image_in[x, y];
        image_ptr = &padded;
    }
    const auto& image = *image_ptr;

    auto cols = image.width() / cell_w;
    auto rows = image.height() / cell_h;
    auto num_cells = cols * rows;

    // Phase 1: Select colors (shared + per-cell)
    std::uint8_t bg{}, mc1{}, mc2{};
    quantize::ScreenResult screen;
    std::vector<bool> cell_is_hires;

    if (mixed) {
        auto sel = select_colors_mixed(image, palette, cols, rows);
        screen = std::move(sel.screen);
        bg = sel.bg; mc1 = sel.mc1; mc2 = sel.mc2;
        cell_is_hires = std::move(sel.cell_is_hires);
    } else if (multicolor) {
        auto sel = select_colors_multicolor(image, palette, cols, rows);
        screen = std::move(sel.screen);
        bg = sel.bg; mc1 = sel.mc1; mc2 = sel.mc2;
    } else {
        screen = select_colors_hires(image, palette, cols, rows);
        bg = screen.background_color;
    }

    // Phase 2: Apply dithering
    if (dither_settings.method != dither::Method::none) {
        log_println("  Dithering...");
        if (mixed) {
            // Mixed mode: dither hires and MC cells in separate passes
            // Build a half-width image for MC cells (average pixel pairs)
            Image mc_image(cols * 4, rows * 8);
            for (std::size_t y = 0; y < rows * 8; ++y)
                for (std::size_t x = 0; x < cols * 4; ++x) {
                    auto p0 = image[x * 2, y];
                    auto p1 = image[x * 2 + 1, y];
                    mc_image[x, y] = {(p0.r + p1.r) * 0.5f,
                                      (p0.g + p1.g) * 0.5f,
                                      (p0.b + p1.b) * 0.5f};
                }

            // Build separate ScreenResults for each mode
            quantize::ScreenResult hi_screen, mc_screen;
            hi_screen.mode = vic2::Mode::hires;
            hi_screen.background_color = bg;
            hi_screen.cells.resize(num_cells);
            mc_screen.mode = vic2::Mode::multicolor;
            mc_screen.background_color = bg;
            mc_screen.cells.resize(num_cells);

            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                if (cell_is_hires[ci]) {
                    hi_screen.cells[ci] = screen.cells[ci];
                    // Dummy MC cell so dither doesn't crash on empty data
                    mc_screen.cells[ci].cell_colors = {bg, mc1, mc2, 0};
                    mc_screen.cells[ci].pixel_indices.resize(32, 0);
                } else {
                    mc_screen.cells[ci] = screen.cells[ci];
                    // Dummy hires cell
                    hi_screen.cells[ci].cell_colors = {bg, 0};
                    hi_screen.cells[ci].pixel_indices.resize(64, 0);
                }
            }

            vic2::ModeParams hi_params = {cols * 8, rows * 8, 8, 8, 2, false};
            vic2::ModeParams mc_params = {cols * 4, rows * 8, 4, 8, 4, true};
            dither::apply(image, hi_screen, palette, hi_params, dither_settings);
            dither::apply(mc_image, mc_screen, palette, mc_params, dither_settings);

            // Merge results back
            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                if (cell_is_hires[ci])
                    screen.cells[ci] = hi_screen.cells[ci];
                else
                    screen.cells[ci] = mc_screen.cells[ci];
            }
        } else {
            vic2::ModeParams params;
            if (multicolor)
                params = {cols * 4, rows * 8, 4, 8, 4, true};
            else
                params = {cols * 8, rows * 8, 8, 8, 2, false};
            dither::apply(image, screen, palette, params, dither_settings);
        }
    }

    // Phase 3: Encode patterns from dithered pixel assignments
    std::vector<CellQuant> cells;
    if (mixed)
        cells = encode_patterns_mixed(screen, num_cells, cell_is_hires);
    else if (multicolor)
        cells = encode_patterns_multicolor(screen, num_cells);
    else
        cells = encode_patterns_hires(screen, num_cells);

    // Phase 4: Deduplicate
    auto dedup = deduplicate(cells);
    auto unique_before = dedup.entries.size();

    log_println("  {} cells, {} unique patterns", num_cells, unique_before);

    std::size_t empty_count = 0;
    for (auto& e : dedup.entries)
        if (e.pattern == empty_pattern) ++empty_count;

    // Phase 5: Merge if > 256 (using perceptual OKLab distance)
    if (unique_before > 256) {
        log_println("  Merging {} -> 256 patterns...", unique_before);

        if (multicolor || mixed) {
            auto pal_lab = precompute_lab(palette);
            std::array<OKLab, 3> shared_lab = {
                pal_lab[bg], pal_lab[mc1], pal_lab[mc2]};

            float max_dist = 0.0f;
            for (std::size_t i = 0; i < pal_lab.size(); ++i)
                for (std::size_t j = i + 1; j < pal_lab.size(); ++j) {
                    float dL = pal_lab[i].L - pal_lab[j].L;
                    float da = pal_lab[i].a - pal_lab[j].a;
                    float db = pal_lab[i].b - pal_lab[j].b;
                    max_dist = std::max(max_dist, dL*dL + da*da + db*db);
                }

            merge_to_256(dedup, [&](std::size_t ai, std::size_t bi) {
                auto& ea = dedup.entries[ai];
                auto& eb = dedup.entries[bi];
                // Never merge across hires/MC boundary
                if (ea.is_hires != eb.is_hires)
                    return std::numeric_limits<float>::max();
                if (ea.is_hires)
                    return pattern_distance_hires(ea.pattern, eb.pattern);
                return pattern_distance_multicolor(
                    ea.pattern, eb.pattern, shared_lab, max_dist);
            });
        } else {
            merge_to_256(dedup, [&](std::size_t ai, std::size_t bi) {
                return pattern_distance_hires(
                    dedup.entries[ai].pattern, dedup.entries[bi].pattern);
            });
        }
    }

    // Phase 6: Extract into flat arrays for refinement
    std::vector<std::size_t> alive_indices;
    std::size_t empty_entry_idx = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < dedup.entries.size(); ++i) {
        if (!dedup.entries[i].alive) continue;
        if (dedup.entries[i].pattern == empty_pattern)
            empty_entry_idx = i;
        else
            alive_indices.push_back(i);
    }

    std::vector<std::uint8_t> entry_to_charset(dedup.entries.size(), 0);
    std::size_t next_idx = 0;
    if (empty_entry_idx != std::numeric_limits<std::size_t>::max()) {
        entry_to_charset[empty_entry_idx] = 0;
        next_idx = 1;
    }
    for (auto ei : alive_indices)
        entry_to_charset[ei] = static_cast<std::uint8_t>(next_idx++);

    auto chars_used = next_idx;

    std::array<Pattern, 256> flat_patterns{};
    std::array<bool, 256> flat_pattern_is_hires{};
    for (std::size_t i = 0; i < dedup.entries.size(); ++i) {
        if (!dedup.entries[i].alive) continue;
        auto idx = entry_to_charset[i];
        flat_patterns[idx] = dedup.entries[i].pattern;
        flat_pattern_is_hires[idx] = dedup.entries[i].is_hires;
    }

    std::vector<std::uint8_t> flat_assignments(num_cells);
    std::vector<std::uint8_t> flat_color_ram(num_cells);
    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        flat_assignments[ci] = entry_to_charset[dedup.cell_to_entry[ci]];
        flat_color_ram[ci] = cells[ci].color_ram;
    }

    // Phase 7: Iterative refinement
    bool has_dither = (dither_settings.method != dither::Method::none);
    log_println("  Refining charset ({})...",
                 has_dither ? "assignments only, preserving dither"
                            : "full k-means");

    if (mixed) {
        // For mixed mode, refinement must respect per-cell and per-pattern modes
        // Cells can only be assigned to patterns of matching mode
        refine_charset_mixed(image, palette, bg, mc1, mc2,
                             cols, rows, flat_patterns, flat_pattern_is_hires,
                             flat_assignments, flat_color_ram, cell_is_hires,
                             10, !has_dither);
    } else {
        refine_charset(image, palette, multicolor, bg, mc1, mc2,
                       cols, rows, flat_patterns, flat_assignments,
                       flat_color_ram, 10, !has_dither);
    }

    // Recount unique patterns after refinement
    std::set<Pattern> unique_after;
    for (std::size_t ci = 0; ci < num_cells; ++ci)
        unique_after.insert(flat_patterns[flat_assignments[ci]]);
    chars_used = unique_after.size();

    // Build result
    CharsetResult result;
    result.background = bg;
    result.mc1 = mc1;
    result.mc2 = mc2;
    result.cols = cols;
    result.rows = rows;
    result.multicolor = true; // VIC-II is in MC text mode for both MC and mixed
    result.mixed = mixed;
    if (mode == CharsetMode::hires) result.multicolor = false;
    result.unique_before_merge = unique_before;
    result.chars_used = chars_used;
    result.empty_cells = empty_count;
    result.charset_data.fill(0);

    if (mixed) {
        result.cell_is_hires.resize(num_cells);
        std::size_t hi_count = 0, mc_count = 0;
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            result.cell_is_hires[ci] = cell_is_hires[ci];
            if (cell_is_hires[ci]) ++hi_count; else ++mc_count;
        }
        result.hires_cells = hi_count;
        result.mc_cells = mc_count;
    }

    for (std::size_t p = 0; p < 256; ++p)
        for (std::size_t b = 0; b < 8; ++b)
            result.charset_data[p * 8 + b] = flat_patterns[p][b];

    result.screen_map = std::move(flat_assignments);
    result.color_ram = std::move(flat_color_ram);

    return result;
}

namespace {

void write_header_to(std::ostream& out, const CharsetResult& result,
                     std::string_view name) {
    auto num_cells = result.cols * result.rows;

    out << "#pragma once\n\n";
    out << "// Generated by png2c64\n";
    out << std::format("// Mode: {} charset\n",
                       result.mixed ? "mixed hires/multicolor"
                       : result.multicolor ? "multicolor" : "hires");
    out << std::format("// Grid: {}x{} ({} cells)\n",
                       result.cols, result.rows, num_cells);
    out << std::format("// Unique chars used: {} / 256\n\n",
                       result.chars_used);

    out << std::format("#define {}_COLS {}\n", name, result.cols);
    out << std::format("#define {}_ROWS {}\n", name, result.rows);
    out << std::format("#define {}_CHARS_USED {}\n", name, result.chars_used);
    out << std::format("#define {}_BACKGROUND {}\n", name, result.background);
    if (result.multicolor) {
        out << std::format("#define {}_MULTICOLOR1 {}\n", name, result.mc1);
        out << std::format("#define {}_MULTICOLOR2 {}\n", name, result.mc2);
    }

    out << std::format("\nstatic const unsigned char {}_charset[2048] = {{\n", name);
    for (std::size_t ci = 0; ci < 256; ++ci) {
        out << std::format("    // Char {}\n    ", ci);
        for (std::size_t b = 0; b < 8; ++b) {
            out << std::format("0x{:02x}", result.charset_data[ci * 8 + b]);
            if (ci < 255 || b < 7) out << ",";
            if (b < 7) out << " ";
        }
        out << "\n";
    }
    out << "};\n";

    out << std::format("\nstatic const unsigned char {}_screen[{}] = {{\n", name, num_cells);
    for (std::size_t r = 0; r < result.rows; ++r) {
        out << "    ";
        for (std::size_t c = 0; c < result.cols; ++c) {
            auto idx = r * result.cols + c;
            out << std::format("0x{:02x}", result.screen_map[idx]);
            if (idx < num_cells - 1) out << ",";
            if (c < result.cols - 1) out << " ";
        }
        out << "\n";
    }
    out << "};\n";

    out << std::format("\nstatic const unsigned char {}_color[{}] = {{\n", name, num_cells);
    for (std::size_t r = 0; r < result.rows; ++r) {
        out << "    ";
        for (std::size_t c = 0; c < result.cols; ++c) {
            auto idx = r * result.cols + c;
            out << std::format("0x{:02x}", result.color_ram[idx]);
            if (idx < num_cells - 1) out << ",";
            if (c < result.cols - 1) out << " ";
        }
        out << "\n";
    }
    out << "};\n";
}

} // namespace

Result<void> write_header(std::string_view path,
                          const CharsetResult& result,
                          std::string_view name) {
    std::ofstream out{std::string{path}};
    if (!out) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open header file: " + std::string(path),
        }};
    }
    write_header_to(out, result, name);
    return {};
}

std::string generate_header(const CharsetResult& result,
                            std::string_view name) {
    std::ostringstream out;
    write_header_to(out, result, name);
    return out.str();
}

Image render(const CharsetResult& result, const Palette& palette) {
    auto cols = result.cols;
    auto rows = result.rows;

    if (result.mixed) {
        // Mixed mode: 320-wide output, per-cell hires or MC decoding
        Image output(cols * 8, rows * 8);
        for (std::size_t ci = 0; ci < cols * rows; ++ci) {
            auto cx = ci % cols;
            auto cy = ci / cols;
            auto char_idx = result.screen_map[ci];
            auto cr = result.color_ram[ci];
            bool is_hi = result.cell_is_hires[ci];

            for (std::size_t row = 0; row < 8; ++row) {
                auto byte = result.charset_data[char_idx * 8 + row];
                if (is_hi) {
                    for (std::size_t col = 0; col < 8; ++col) {
                        bool is_fg = (byte >> (7 - col)) & 1;
                        auto pal_idx = is_fg ? cr : result.background;
                        output[cx * 8 + col, cy * 8 + row] = palette.colors[pal_idx];
                    }
                } else {
                    for (std::size_t col = 0; col < 4; ++col) {
                        auto bits = (byte >> (6 - col * 2)) & 0x03;
                        std::uint8_t pal_idx;
                        switch (bits) {
                        case 0: pal_idx = result.background; break;
                        case 1: pal_idx = result.mc1; break;
                        case 2: pal_idx = result.mc2; break;
                        default: pal_idx = cr; break;
                        }
                        auto color = palette.colors[pal_idx];
                        output[cx * 8 + col * 2, cy * 8 + row] = color;
                        output[cx * 8 + col * 2 + 1, cy * 8 + row] = color;
                    }
                }
            }
        }
        return output;
    }

    if (result.multicolor) {
        Image output(cols * 4 * 2, rows * 8);
        for (std::size_t ci = 0; ci < cols * rows; ++ci) {
            auto cx = ci % cols;
            auto cy = ci / cols;
            auto char_idx = result.screen_map[ci];
            auto pc = result.color_ram[ci];
            for (std::size_t row = 0; row < 8; ++row) {
                auto byte = result.charset_data[char_idx * 8 + row];
                for (std::size_t col = 0; col < 4; ++col) {
                    auto bits = (byte >> (6 - col * 2)) & 0x03;
                    std::uint8_t pal_idx;
                    switch (bits) {
                    case 0: pal_idx = result.background; break;
                    case 1: pal_idx = result.mc1; break;
                    case 2: pal_idx = result.mc2; break;
                    default: pal_idx = pc; break;
                    }
                    auto color = palette.colors[pal_idx];
                    auto out_x = cx * 8 + col * 2;
                    auto out_y = cy * 8 + row;
                    output[out_x, out_y] = color;
                    output[out_x + 1, out_y] = color;
                }
            }
        }
        return output;
    }

    Image output(cols * 8, rows * 8);
    for (std::size_t ci = 0; ci < cols * rows; ++ci) {
        auto cx = ci % cols;
        auto cy = ci / cols;
        auto char_idx = result.screen_map[ci];
        auto fg = result.color_ram[ci];
        for (std::size_t row = 0; row < 8; ++row) {
            auto byte = result.charset_data[char_idx * 8 + row];
            for (std::size_t col = 0; col < 8; ++col) {
                bool is_fg = (byte >> (7 - col)) & 1;
                auto pal_idx = is_fg ? fg : result.background;
                output[cx * 8 + col, cy * 8 + row] = palette.colors[pal_idx];
            }
        }
    }
    return output;
}

} // namespace png2c64::charset
