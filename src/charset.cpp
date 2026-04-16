#include "charset.hpp"
#include "blur_util.hpp"
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
    std::size_t cols, std::size_t rows,
    quantize::Metric metric = quantize::Metric::mse) {

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

    // Per-cell precompute for blur / ssim (mirrors select_colors_multicolor).
    namespace bu = quantize::blur_util;
    auto kernel_taps = bu::build_kernel_taps(8, 8);
    std::vector<std::vector<OKLab>> blur_src;
    std::vector<bu::PixelDistLut> pd_luts;
    std::vector<bu::ClosedCtx> closed_ctx;
    struct CellSsim { OKLab mu_s, var_s; };
    std::vector<CellSsim> ssim_pre;

    if (metric == quantize::Metric::blur) {
        blur_src.resize(num_cells);
        pd_luts.resize(num_cells);
        closed_ctx.resize(num_cells);
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            blur_src[ci] = bu::compute_blurred(cells_lab[ci], kernel_taps);
            pd_luts[ci] = bu::build_pixel_dist_lut(cells_lab[ci], pal_lab);
            closed_ctx[ci] = bu::make_closed_ctx(blur_src[ci]);
        }
    } else if (metric == quantize::Metric::ssim) {
        ssim_pre.resize(num_cells);
        constexpr float inv_n = 1.0f / 64.0f;
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            OKLab mu{0, 0, 0};
            for (auto& v : cells_lab[ci]) { mu.L += v.L; mu.a += v.a; mu.b += v.b; }
            mu.L *= inv_n; mu.a *= inv_n; mu.b *= inv_n;
            OKLab var{0, 0, 0};
            for (auto& v : cells_lab[ci]) {
                float dL = v.L - mu.L, da = v.a - mu.a, db = v.b - mu.b;
                var.L += dL * dL;
                var.a += da * da;
                var.b += db * db;
            }
            var.L *= inv_n; var.a *= inv_n; var.b *= inv_n;
            ssim_pre[ci].mu_s = mu;
            ssim_pre[ci].var_s = var;
        }
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
                float err;
                if (metric == quantize::Metric::blur) {
                    err = bu::score_cell_blur_2color(
                        pd_luts[ci], blur_src[ci], kernel_taps, pal_lab,
                        closed_ctx[ci], bg, fg);
                } else if (metric == quantize::Metric::ssim) {
                    constexpr float inv_n = 1.0f / 64.0f;
                    constexpr float C1 = 0.01f * 0.01f;
                    constexpr float C2 = 0.03f * 0.03f;
                    constexpr float kMseLambda = 1.0f;
                    auto& sp = ssim_pre[ci];
                    auto& bl = pal_lab[bg];
                    auto& fl = pal_lab[fg];
                    OKLab mu_r{0, 0, 0};
                    float mse = 0;
                    std::array<OKLab, 64> rendered;
                    for (std::size_t p = 0; p < 64; ++p) {
                        auto& px = cells_lab[ci][p];
                        float db_ = (px.L-bl.L)*(px.L-bl.L) + (px.a-bl.a)*(px.a-bl.a) + (px.b-bl.b)*(px.b-bl.b);
                        float df_ = (px.L-fl.L)*(px.L-fl.L) + (px.a-fl.a)*(px.a-fl.a) + (px.b-fl.b)*(px.b-fl.b);
                        auto& chosen = (df_ < db_) ? fl : bl;
                        rendered[p] = chosen;
                        mu_r.L += chosen.L;
                        mu_r.a += chosen.a;
                        mu_r.b += chosen.b;
                        float ddL = px.L - chosen.L;
                        float dda = px.a - chosen.a;
                        float ddb = px.b - chosen.b;
                        mse += ddL * ddL + dda * dda + ddb * ddb;
                    }
                    mu_r.L *= inv_n; mu_r.a *= inv_n; mu_r.b *= inv_n;
                    OKLab var_r{0, 0, 0}, cov{0, 0, 0};
                    for (std::size_t p = 0; p < 64; ++p) {
                        float drL = rendered[p].L - mu_r.L;
                        float dra = rendered[p].a - mu_r.a;
                        float drb = rendered[p].b - mu_r.b;
                        var_r.L += drL * drL;
                        var_r.a += dra * dra;
                        var_r.b += drb * drb;
                        cov.L += drL * (cells_lab[ci][p].L - sp.mu_s.L);
                        cov.a += dra * (cells_lab[ci][p].a - sp.mu_s.a);
                        cov.b += drb * (cells_lab[ci][p].b - sp.mu_s.b);
                    }
                    var_r.L *= inv_n; var_r.a *= inv_n; var_r.b *= inv_n;
                    cov.L *= inv_n; cov.a *= inv_n; cov.b *= inv_n;
                    auto ssim_ch = [&](float ms, float vs, float cv,
                                        float mr, float vr) {
                        float num = (2.0f * ms * mr + C1) * (2.0f * cv + C2);
                        float den = (ms * ms + mr * mr + C1) * (vs + vr + C2);
                        return num / den;
                    };
                    float ssim = ssim_ch(sp.mu_s.L, sp.var_s.L, cov.L, mu_r.L, var_r.L)
                               + ssim_ch(sp.mu_s.a, sp.var_s.a, cov.a, mu_r.a, var_r.a)
                               + ssim_ch(sp.mu_s.b, sp.var_s.b, cov.b, mu_r.b, var_r.b);
                    err = -ssim + kMseLambda * mse;
                } else {
                    // mse: original per-pixel-nearest of 2 colors.
                    err = 0.0f;
                    auto& bl = pal_lab[bg];
                    auto& fl = pal_lab[fg];
                    for (auto& px : cells_lab[ci]) {
                        float db = (px.L-bl.L)*(px.L-bl.L) + (px.a-bl.a)*(px.a-bl.a) + (px.b-bl.b)*(px.b-bl.b);
                        float df = (px.L-fl.L)*(px.L-fl.L) + (px.a-fl.a)*(px.a-fl.a) + (px.b-fl.b)*(px.b-fl.b);
                        err += std::min(db, df);
                    }
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
    std::size_t cols, std::size_t rows,
    quantize::Metric metric = quantize::Metric::mse) {

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

    // ---- Per-cell setup for blur / ssim metrics ----
    namespace bu = quantize::blur_util;
    auto kernel_taps = bu::build_kernel_taps(4, 8);

    // For blur: per-cell blurred source + pixel_dist LUT. Reused across
    // 560 triples × 8 pc choices. For ssim: per-cell μ_s, var_s,
    // total_sum, A_norm.
    std::vector<std::vector<OKLab>> blur_src;
    std::vector<bu::PixelDistLut> pd_luts;
    struct CellSsim {
        OKLab mu_s, var_s, total_sum;
        float A_norm;
    };
    std::vector<CellSsim> ssim_pre(metric == quantize::Metric::ssim ? num_cells : 0);

    if (metric == quantize::Metric::blur) {
        blur_src.resize(num_cells);
        pd_luts.resize(num_cells);
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            blur_src[ci] = bu::compute_blurred(cells_lab[ci], kernel_taps);
            pd_luts[ci] = bu::build_pixel_dist_lut(cells_lab[ci], pal_lab);
        }
    } else if (metric == quantize::Metric::ssim) {
        constexpr float inv_n = 1.0f / 32.0f;
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            OKLab mu{0, 0, 0};
            for (auto& v : cells_lab[ci]) {
                mu.L += v.L; mu.a += v.a; mu.b += v.b;
            }
            ssim_pre[ci].total_sum = mu;
            mu.L *= inv_n; mu.a *= inv_n; mu.b *= inv_n;
            OKLab var{0, 0, 0};
            float A = 0;
            for (auto& v : cells_lab[ci]) {
                float dL = v.L - mu.L;
                float da = v.a - mu.a;
                float db = v.b - mu.b;
                var.L += dL * dL;
                var.a += da * da;
                var.b += db * db;
                A += v.L * v.L + v.a * v.a + v.b * v.b;
            }
            var.L *= inv_n; var.a *= inv_n; var.b *= inv_n;
            ssim_pre[ci].mu_s = mu;
            ssim_pre[ci].var_s = var;
            ssim_pre[ci].A_norm = A;
        }
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

    // For blur metric only: per-cell err depends only on the 4-set
    // {c0,c1,c2,pc}. The same 4-set is reached from multiple
    // (triple, pc) splits (any element in 0..7 can play pc), so
    // dedupe — compute once per (cell, 4-set), reuse for every
    // triple that selects that 4-set. ~1.6× saving on charset-mc
    // (~2800 (triple, pc) combos collapse to ~1750 unique 4-sets).
    constexpr std::uint16_t kNoSet = 0xFFFF;
    std::array<std::uint16_t, 65536> set_index{};
    std::vector<std::uint16_t> set_masks;
    if (metric == quantize::Metric::blur) {
        std::ranges::fill(set_index, kNoSet);
        // Enumerate 4-sets that contain at least one element from 0..7.
        for (std::uint8_t a = 0; a < n; ++a)
            for (std::uint8_t b = a + 1; b < n; ++b)
                for (std::uint8_t c = b + 1; c < n; ++c)
                    for (std::uint8_t d = c + 1; d < n; ++d) {
                        if (a >= 8) continue;  // need ≥1 element in 0..7 (=> a<8)
                        auto mask = static_cast<std::uint16_t>(
                            (1u << a) | (1u << b) | (1u << c) | (1u << d));
                        set_index[mask] =
                            static_cast<std::uint16_t>(set_masks.size());
                        set_masks.push_back(mask);
                    }
    }
    auto num_sets = set_masks.size();
    // err_table[ci * num_sets + s]
    std::vector<float> err_table;
    if (metric == quantize::Metric::blur) {
        err_table.assign(num_cells * num_sets,
                          std::numeric_limits<float>::max());
        std::atomic<std::size_t> next_cell{0};
        auto build = [&] {
            std::vector<OKLab> assign_scratch;
            assign_scratch.reserve(32);
            while (true) {
                auto ci = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;
                for (std::size_t s = 0; s < num_sets; ++s) {
                    auto m = set_masks[s];
                    std::array<std::uint8_t, 4> colors{};
                    std::size_t k = 0;
                    for (std::uint8_t c = 0; c < n; ++c)
                        if (m & (1u << c)) colors[k++] = c;
                    err_table[ci * num_sets + s] = bu::score_cell_blur_fused(
                        pd_luts[ci], blur_src[ci], kernel_taps,
                        pal_lab, colors, assign_scratch);
                }
            }
        };
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), num_cells);
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(build);
        build();
    }

    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        std::vector<OKLab> assign_scratch;
        assign_scratch.reserve(32);
        while (true) {
            auto ti = next.fetch_add(1, std::memory_order_relaxed);
            if (ti >= candidates.size()) break;
            auto& cand = candidates[ti];
            cand.per_cell.resize(num_cells);
            cand.total_error = 0.0f;
            std::uint16_t triple_mask = static_cast<std::uint16_t>(
                (1u << cand.c0) | (1u << cand.c1) | (1u << cand.c2));
            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                float best_cell = std::numeric_limits<float>::max();
                std::uint8_t best_pc = 0;
                // VIC-II MC text mode: color RAM is 3 bits (0-7) + bit 3 = MC enable
                for (std::uint8_t pc = 0; pc < 8; ++pc) {
                    if (pc == cand.c0 || pc == cand.c1 || pc == cand.c2) continue;
                    std::array<std::uint8_t, 4> colors = {cand.c0, cand.c1, cand.c2, pc};

                    float err;
                    if (metric == quantize::Metric::blur) {
                        auto m = static_cast<std::uint16_t>(
                            triple_mask | (1u << pc));
                        err = err_table[ci * num_sets + set_index[m]];
                    } else if (metric == quantize::Metric::ssim) {
                        // Per-pixel-nearest pattern; SSIM + MSE hybrid
                        // (same recipe as petscii_pick_ssim).
                        constexpr float inv_n = 1.0f / 32.0f;
                        constexpr float C1 = 0.01f * 0.01f;
                        constexpr float C2 = 0.03f * 0.03f;
                        constexpr float kMseLambda = 1.0f;
                        auto& sp = ssim_pre[ci];
                        std::array<std::uint8_t, 32> assign;
                        std::array<std::size_t, 4> count{};
                        std::array<OKLab, 4> S{};   // per-color centered sum
                        OKLab mu_r{0, 0, 0};
                        for (std::size_t p = 0; p < 32; ++p) {
                            auto& px = cells_lab[ci][p];
                            float bd = std::numeric_limits<float>::max();
                            std::uint8_t bi = 0;
                            for (std::uint8_t c = 0; c < 4; ++c) {
                                auto& cl = pal_lab[colors[c]];
                                float d = (px.L-cl.L)*(px.L-cl.L)
                                        + (px.a-cl.a)*(px.a-cl.a)
                                        + (px.b-cl.b)*(px.b-cl.b);
                                if (d < bd) { bd = d; bi = c; }
                            }
                            assign[p] = bi;
                            ++count[bi];
                            S[bi].L += px.L - sp.mu_s.L;
                            S[bi].a += px.a - sp.mu_s.a;
                            S[bi].b += px.b - sp.mu_s.b;
                        }
                        // Rendered mean μ_r per channel.
                        for (std::uint8_t c = 0; c < 4; ++c) {
                            auto& cl = pal_lab[colors[c]];
                            float w = static_cast<float>(count[c]) * inv_n;
                            mu_r.L += w * cl.L;
                            mu_r.a += w * cl.a;
                            mu_r.b += w * cl.b;
                        }
                        // Rendered variance and covariance (closed form).
                        OKLab var_r{0, 0, 0};
                        OKLab cov{0, 0, 0};
                        for (std::uint8_t c = 0; c < 4; ++c) {
                            auto& cl = pal_lab[colors[c]];
                            float w = static_cast<float>(count[c]) * inv_n;
                            float dL = cl.L - mu_r.L;
                            float da = cl.a - mu_r.a;
                            float db = cl.b - mu_r.b;
                            var_r.L += w * dL * dL;
                            var_r.a += w * da * da;
                            var_r.b += w * db * db;
                            cov.L += (cl.L - mu_r.L) * S[c].L;
                            cov.a += (cl.a - mu_r.a) * S[c].a;
                            cov.b += (cl.b - mu_r.b) * S[c].b;
                        }
                        cov.L *= inv_n; cov.a *= inv_n; cov.b *= inv_n;
                        auto ssim_ch = [&](float ms, float vs, float cv,
                                            float mr, float vr) {
                            float num = (2.0f * ms * mr + C1)
                                      * (2.0f * cv + C2);
                            float den = (ms * ms + mr * mr + C1)
                                      * (vs + vr + C2);
                            return num / den;
                        };
                        float ssim = ssim_ch(sp.mu_s.L, sp.var_s.L, cov.L,
                                              mu_r.L, var_r.L)
                                   + ssim_ch(sp.mu_s.a, sp.var_s.a, cov.a,
                                              mu_r.a, var_r.a)
                                   + ssim_ch(sp.mu_s.b, sp.var_s.b, cov.b,
                                              mu_r.b, var_r.b);
                        // MSE on the per-pixel-nearest rendering.
                        float mse = 0.0f;
                        for (std::size_t p = 0; p < 32; ++p) {
                            auto& px = cells_lab[ci][p];
                            auto& cl = pal_lab[colors[assign[p]]];
                            float dL = px.L - cl.L;
                            float da = px.a - cl.a;
                            float db = px.b - cl.b;
                            mse += dL * dL + da * da + db * db;
                        }
                        err = -ssim + kMseLambda * mse;
                    } else {  // mse: original path
                        err = 0.0f;
                        for (auto& px : cells_lab[ci]) {
                            float best = std::numeric_limits<float>::max();
                            for (auto c : colors) {
                                auto& cl = pal_lab[c];
                                float d = (px.L-cl.L)*(px.L-cl.L) + (px.a-cl.a)*(px.a-cl.a) + (px.b-cl.b)*(px.b-cl.b);
                                if (d < best) best = d;
                            }
                            err += best;
                        }
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
    std::size_t cols, std::size_t rows,
    quantize::Metric metric = quantize::Metric::mse) {

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

    // Metric-aware precomputes for blur and ssim (mirrors the setup
    // used in select_colors_hires / select_colors_multicolor).
    namespace bu = quantize::blur_util;
    std::vector<std::array<bu::Tap, 9>> kernel_taps_hi;
    std::vector<std::array<bu::Tap, 9>> kernel_taps_mc;
    std::vector<std::vector<OKLab>> blurred_hi;
    std::vector<std::vector<OKLab>> blurred_mc;
    std::vector<bu::PixelDistLut> pd_luts_hi;
    std::vector<bu::PixelDistLut> pd_luts_mc;
    std::vector<bu::ClosedCtx> closed_ctx_hi;
    struct CellStats { OKLab mu_s, var_s; };
    std::vector<CellStats> ssim_hi;
    std::vector<CellStats> ssim_mc;
    if (metric == quantize::Metric::blur) {
        kernel_taps_hi = bu::build_kernel_taps(8, 8);
        kernel_taps_mc = bu::build_kernel_taps(4, 8);
        blurred_hi.resize(num_cells);
        blurred_mc.resize(num_cells);
        pd_luts_hi.resize(num_cells);
        pd_luts_mc.resize(num_cells);
        closed_ctx_hi.resize(num_cells);
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            blurred_hi[ci] = bu::compute_blurred(cells_hi[ci], kernel_taps_hi);
            blurred_mc[ci] = bu::compute_blurred(cells_mc[ci], kernel_taps_mc);
            pd_luts_hi[ci] = bu::build_pixel_dist_lut(cells_hi[ci], pal_lab);
            pd_luts_mc[ci] = bu::build_pixel_dist_lut(cells_mc[ci], pal_lab);
            closed_ctx_hi[ci] = bu::make_closed_ctx(blurred_hi[ci]);
        }
    } else if (metric == quantize::Metric::ssim) {
        ssim_hi.resize(num_cells);
        ssim_mc.resize(num_cells);
        auto stats_for = [](const std::vector<OKLab>& cell) {
            CellStats s{};
            float inv_n = 1.0f / static_cast<float>(cell.size());
            for (auto& v : cell) { s.mu_s.L += v.L; s.mu_s.a += v.a; s.mu_s.b += v.b; }
            s.mu_s.L *= inv_n; s.mu_s.a *= inv_n; s.mu_s.b *= inv_n;
            for (auto& v : cell) {
                float dL = v.L - s.mu_s.L, da = v.a - s.mu_s.a, db = v.b - s.mu_s.b;
                s.var_s.L += dL * dL;
                s.var_s.a += da * da;
                s.var_s.b += db * db;
            }
            s.var_s.L *= inv_n; s.var_s.a *= inv_n; s.var_s.b *= inv_n;
            return s;
        };
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            ssim_hi[ci] = stats_for(cells_hi[ci]);
            ssim_mc[ci] = stats_for(cells_mc[ci]);
        }
    }

    // Score (cell_index, candidate colours, is_hires) under the active
    // metric. For mse: per-pixel-nearest dist². For blur: build
    // per-pixel-nearest rendering, blur it, MSE vs precomputed blurred
    // source. For ssim: SSIM+MSE hybrid on the rendering.
    // Per-worker scratch for the fused blur scorer.
    thread_local std::vector<OKLab> assign_scratch;
    auto score_cell = [&](std::size_t ci,
                           std::span<const std::uint8_t> colors,
                           bool is_hi) -> float {
        auto& cell_lab = is_hi ? cells_hi[ci] : cells_mc[ci];
        if (metric == quantize::Metric::mse) {
            float err = 0;
            for (auto& px : cell_lab) {
                float best = std::numeric_limits<float>::max();
                for (auto c : colors) {
                    auto& cl = pal_lab[c];
                    float d = (px.L-cl.L)*(px.L-cl.L)
                            + (px.a-cl.a)*(px.a-cl.a)
                            + (px.b-cl.b)*(px.b-cl.b);
                    if (d < best) best = d;
                }
                err += best;
            }
            return err;
        }
        if (metric == quantize::Metric::blur) {
            if (is_hi && colors.size() == 2) {
                return bu::score_cell_blur_2color(
                    pd_luts_hi[ci], blurred_hi[ci], kernel_taps_hi, pal_lab,
                    closed_ctx_hi[ci], colors[0], colors[1]);
            }
            return bu::score_cell_blur_fused(
                is_hi ? pd_luts_hi[ci] : pd_luts_mc[ci],
                is_hi ? blurred_hi[ci] : blurred_mc[ci],
                is_hi ? kernel_taps_hi : kernel_taps_mc,
                pal_lab, colors, assign_scratch);
        }
        // ssim: reuse existing per-pixel-nearest path
        std::vector<OKLab> rendered(cell_lab.size());
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
            rendered[p] = pal_lab[colors[bi]];
        }
        // ssim hybrid
        constexpr float C1 = 0.01f * 0.01f;
        constexpr float C2 = 0.03f * 0.03f;
        constexpr float kMseLambda = 1.0f;
        auto& sp = is_hi ? ssim_hi[ci] : ssim_mc[ci];
        float inv_n = 1.0f / static_cast<float>(cell_lab.size());
        OKLab mu_r{0, 0, 0};
        float mse = 0;
        for (std::size_t p = 0; p < cell_lab.size(); ++p) {
            mu_r.L += rendered[p].L;
            mu_r.a += rendered[p].a;
            mu_r.b += rendered[p].b;
            float dL = cell_lab[p].L - rendered[p].L;
            float da = cell_lab[p].a - rendered[p].a;
            float db = cell_lab[p].b - rendered[p].b;
            mse += dL * dL + da * da + db * db;
        }
        mu_r.L *= inv_n; mu_r.a *= inv_n; mu_r.b *= inv_n;
        OKLab var_r{0, 0, 0}, cov{0, 0, 0};
        for (std::size_t p = 0; p < cell_lab.size(); ++p) {
            float drL = rendered[p].L - mu_r.L;
            float dra = rendered[p].a - mu_r.a;
            float drb = rendered[p].b - mu_r.b;
            var_r.L += drL * drL;
            var_r.a += dra * dra;
            var_r.b += drb * drb;
            cov.L += drL * (cell_lab[p].L - sp.mu_s.L);
            cov.a += dra * (cell_lab[p].a - sp.mu_s.a);
            cov.b += drb * (cell_lab[p].b - sp.mu_s.b);
        }
        var_r.L *= inv_n; var_r.a *= inv_n; var_r.b *= inv_n;
        cov.L *= inv_n; cov.a *= inv_n; cov.b *= inv_n;
        auto ssim_ch = [&](float ms, float vs, float cv, float mr, float vr) {
            float num = (2.0f * ms * mr + C1) * (2.0f * cv + C2);
            float den = (ms * ms + mr * mr + C1) * (vs + vr + C2);
            return num / den;
        };
        float ssim = ssim_ch(sp.mu_s.L, sp.var_s.L, cov.L, mu_r.L, var_r.L)
                   + ssim_ch(sp.mu_s.a, sp.var_s.a, cov.a, mu_r.a, var_r.a)
                   + ssim_ch(sp.mu_s.b, sp.var_s.b, cov.b, mu_r.b, var_r.b);
        return -ssim + kMseLambda * mse;
    };

    // Precompute best hires result per cell per background color.
    // hi_best[bg][ci] = {error, fg}  — avoids redoing 16×64 work per triple.
    struct HiresResult { float error; std::uint8_t fg; };
    std::vector<std::vector<HiresResult>> hi_best(n, std::vector<HiresResult>(num_cells));

    {
        std::atomic<std::size_t> next_bg{0};
        auto hi_worker = [&] {
            while (true) {
                auto bg = next_bg.fetch_add(1, std::memory_order_relaxed);
                if (bg >= n) break;
                for (std::size_t ci = 0; ci < num_cells; ++ci) {
                    float best_err = std::numeric_limits<float>::max();
                    std::uint8_t best_fg = 0;
                    // VIC-II MC text mode: hires fg comes from color RAM bits 0-2
                    // (bit 3 must be clear to select hires), so fg is limited to 0-7
                    for (std::uint8_t fg = 0; fg < 8; ++fg) {
                        if (fg == bg) continue;
                        std::array<std::uint8_t, 2> colors{
                            static_cast<std::uint8_t>(bg), fg};
                        float err = score_cell(ci, colors, true);
                        if (err < best_err) { best_err = err; best_fg = fg; }
                    }
                    hi_best[bg][ci] = {best_err, best_fg};
                }
            }
        };
        auto nt = std::min(static_cast<std::size_t>(hw_threads()),
                           static_cast<std::size_t>(n));
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(hi_worker);
        hi_worker();
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

    // For blur metric only: dedupe MC scoring across (triple, pc) splits
    // that produce the same 4-set (same trick as select_colors_multicolor).
    constexpr std::uint16_t kNoSet = 0xFFFF;
    std::array<std::uint16_t, 65536> mc_set_index{};
    std::vector<std::uint16_t> mc_set_masks;
    if (metric == quantize::Metric::blur) {
        std::ranges::fill(mc_set_index, kNoSet);
        for (std::uint8_t a = 0; a < n; ++a)
            for (std::uint8_t b = a + 1; b < n; ++b)
                for (std::uint8_t c = b + 1; c < n; ++c)
                    for (std::uint8_t d = c + 1; d < n; ++d) {
                        if (a >= 8) continue;  // need ≥1 pc-eligible (0..7)
                        auto mask = static_cast<std::uint16_t>(
                            (1u << a) | (1u << b) | (1u << c) | (1u << d));
                        mc_set_index[mask] = static_cast<std::uint16_t>(
                            mc_set_masks.size());
                        mc_set_masks.push_back(mask);
                    }
    }
    auto num_mc_sets = mc_set_masks.size();
    std::vector<float> mc_err_table;
    if (metric == quantize::Metric::blur) {
        mc_err_table.assign(num_cells * num_mc_sets,
                              std::numeric_limits<float>::max());
        std::atomic<std::size_t> next_cell{0};
        auto build = [&] {
            while (true) {
                auto ci = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;
                for (std::size_t s = 0; s < num_mc_sets; ++s) {
                    auto m = mc_set_masks[s];
                    std::array<std::uint8_t, 4> colors{};
                    std::size_t k = 0;
                    for (std::uint8_t c = 0; c < n; ++c)
                        if (m & (1u << c)) colors[k++] = c;
                    mc_err_table[ci * num_mc_sets + s] =
                        score_cell(ci, colors, false);
                }
            }
        };
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), num_cells);
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(build);
        build();
    }

    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        while (true) {
            auto ti = next.fetch_add(1, std::memory_order_relaxed);
            if (ti >= candidates.size()) break;
            auto& cand = candidates[ti];
            cand.per_cell.resize(num_cells);
            cand.is_hires.resize(num_cells);
            cand.total_error = 0.0f;
            std::uint16_t triple_mask = static_cast<std::uint16_t>(
                (1u << cand.c0) | (1u << cand.c1) | (1u << cand.c2));

            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                // Try MC: best per-cell color from 0-7
                float best_mc_err = std::numeric_limits<float>::max();
                std::uint8_t best_mc_pc = 0;
                for (std::uint8_t pc = 0; pc < 8; ++pc) {
                    if (pc == cand.c0 || pc == cand.c1 || pc == cand.c2) continue;
                    float err;
                    if (metric == quantize::Metric::blur) {
                        auto m = static_cast<std::uint16_t>(
                            triple_mask | (1u << pc));
                        err = mc_err_table[ci * num_mc_sets + mc_set_index[m]];
                    } else {
                        std::array<std::uint8_t, 4> colors = {
                            cand.c0, cand.c1, cand.c2, pc};
                        err = score_cell(ci, colors, false);
                    }
                    if (err < best_mc_err) { best_mc_err = err; best_mc_pc = pc; }
                }

                // Hires: look up precomputed best for each shared color as bg
                float best_hi_err = std::numeric_limits<float>::max();
                std::uint8_t best_hi_fg = 0;
                for (auto bg_cand : {cand.c0, cand.c1, cand.c2}) {
                    auto& hr = hi_best[bg_cand][ci];
                    if (hr.error < best_hi_err) {
                        best_hi_err = hr.error;
                        best_hi_fg = hr.fg;
                    }
                }

                // Normalize per-pixel: hires has 64 pixels, MC has 32
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

        // Re-evaluate mode decision with the actual bg/mc1/mc2 assignment.
        // The triple search may have picked hires with a different bg candidate.
        auto& hr = hi_best[bg_color][ci];
        float hi_avg = hr.error / 64.0f;

        // Recompute MC error with actual color ordering
        float best_mc_err = std::numeric_limits<float>::max();
        std::uint8_t best_mc_pc = 0;
        for (std::uint8_t pc = 0; pc < 8; ++pc) {
            if (pc == bg_color || pc == mc1_color || pc == mc2_color) continue;
            std::array<std::uint8_t, 4> colors = {bg_color, mc1_color, mc2_color, pc};
            float err = score_cell(ci, colors, false);
            if (err < best_mc_err) { best_mc_err = err; best_mc_pc = pc; }
        }
        float mc_avg = best_mc_err / 32.0f;

        bool is_hi = (hi_avg <= mc_avg);
        cell_is_hires[ci] = is_hi;

        if (is_hi) {
            auto fg = hr.fg;
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
            cell.cell_colors = {bg_color, mc1_color, mc2_color, best_mc_pc};
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
//
// Slot 0 of the output charset is reserved for an empty (all-background)
// pattern so nearly-empty cells can snap to it rather than forcing the
// quantizer to invent a stray-pixel pattern. Budget is therefore 255
// non-empty entries plus (optionally) the natural empty entry; the empty
// entry is protected from being merged away.
void merge_to_256(DeduplicatedCharset& dedup, auto dist_func) {
    std::vector<std::size_t> alive;
    bool has_empty = false;
    for (std::size_t i = 0; i < dedup.entries.size(); ++i)
        if (dedup.entries[i].alive) {
            alive.push_back(i);
            if (dedup.entries[i].pattern == empty_pattern) has_empty = true;
        }

    auto budget = has_empty ? std::size_t{256} : std::size_t{255};
    if (alive.size() <= budget) return;

    auto merges_needed = alive.size() - budget;

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
        // Never merge the reserved empty pattern into anything.
        if (dedup.entries[p.a].pattern == empty_pattern ||
            dedup.entries[p.b].pattern == empty_pattern) continue;

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

// ---------------------------------------------------------------------------
// Cost-aware pattern smoothing (denoise)
//
// After refinement, optionally flip bits whose combined score
//   smoothness_gain - lambda * avg_fidelity_cost
// is positive. Fidelity cost is the real OKLab error increase per cell
// using the pattern (averaged over cells — makes lambda image-scale-
// independent). Smoothness gain counts 4-connected edges that become
// same-class minus those that become different-class after the flip.
//
// Slot 0 (reserved empty) and unused patterns are skipped. DBS-style
// iteration until stable or 8 passes. Patterns are processed in
// parallel (one thread per pattern) — no cross-pattern state.
// ---------------------------------------------------------------------------
void denoise_patterns_cost_aware(
    const Image& image, const Palette& palette,
    bool multicolor, bool mixed,
    std::uint8_t bg, std::uint8_t mc1, std::uint8_t mc2,
    std::size_t cols, std::size_t rows,
    std::array<Pattern, 256>& patterns,
    const std::array<bool, 256>& pattern_is_hires,
    const std::vector<std::uint8_t>& assignments,
    const std::vector<std::uint8_t>& color_ram,
    const std::vector<bool>& cell_is_hires_in,
    float lambda_user) {

    if (lambda_user <= 0.0f) return;

    auto pal_lab = precompute_lab(palette);
    auto num_cells = cols * rows;

    // Two-axis tuning. Lambda (geometric) weights the fidelity cost; a
    // second `min_smooth` knob sets the minimum pattern-local smoothness
    // gain required to even consider a flip. Without min_smooth, "free"
    // flips (pixels whose fidelity delta is ~0 because the quantiser
    // sees either class as equally good) pass at any lambda, so the
    // low end of the strength slider still produces heavy smoothing.
    // Using min_smooth keeps strength≈0 nearly inert.
    constexpr float LAMBDA_MIN = 30.0f;     // at strength 1.0
    constexpr float LAMBDA_MAX = 10000.0f;  // at strength approaching 0
    float strength = std::clamp(lambda_user, 0.0f, 1.0f);
    float exponent = 1.0f - strength;
    float lambda = LAMBDA_MIN *
                   std::pow(LAMBDA_MAX / LAMBDA_MIN, exponent);
    // Integer minimum smoothness gain to permit a flip. Ramps from 4
    // (effectively off — flips only in near-uniform regions) down to 1
    // (any gain allowed) as strength → 1.
    int min_smooth_delta = static_cast<int>(
        std::round(4.0f * (1.0f - strength)));
    if (min_smooth_delta < 1) min_smooth_delta = 1;
    if (min_smooth_delta > 4) min_smooth_delta = 4;

    // Group cells by assigned pattern.
    std::array<std::vector<std::size_t>, 256> pat_cells;
    for (std::size_t ci = 0; ci < num_cells; ++ci)
        pat_cells[assignments[ci]].push_back(ci);

    // Precompute cell pixels in OKLab. Mixed: hires cells = 64 pixels,
    // MC cells = 32 pixels (horizontal pairs averaged). Non-mixed: all
    // cells have the same shape.
    auto cell_w_for = [&](std::size_t ci) -> std::size_t {
        if (mixed) return cell_is_hires_in[ci] ? 8 : 4;
        return multicolor ? 4 : 8;
    };
    std::vector<std::vector<OKLab>> cells_lab(num_cells);
    for (std::size_t cy = 0; cy < rows; ++cy) {
        for (std::size_t cx = 0; cx < cols; ++cx) {
            auto ci = cy * cols + cx;
            auto W = cell_w_for(ci);
            cells_lab[ci].reserve(W * 8);
            bool mc_cell = (W == 4);
            for (std::size_t dy = 0; dy < 8; ++dy) {
                for (std::size_t dx = 0; dx < W; ++dx) {
                    if (mc_cell) {
                        // Average pixel pair for MC cells
                        std::size_t base_x = mixed ? cx * 8 : cx * 4;
                        auto p0 = image[base_x + dx * 2, cy * 8 + dy];
                        auto p1 = image[base_x + dx * 2 + 1, cy * 8 + dy];
                        Color3f avg{(p0.r + p1.r) * 0.5f,
                                    (p0.g + p1.g) * 0.5f,
                                    (p0.b + p1.b) * 0.5f};
                        cells_lab[ci].push_back(color_space::linear_to_oklab(avg));
                    } else {
                        cells_lab[ci].push_back(color_space::linear_to_oklab(
                            image[cx * 8 + dx, cy * 8 + dy]));
                    }
                }
            }
        }
    }

    std::atomic<std::size_t> next_pat{1}; // slot 0 reserved
    std::atomic<std::size_t> total_flips{0};

    auto worker = [&] {
        while (true) {
            auto p = next_pat.fetch_add(1, std::memory_order_relaxed);
            if (p >= 256) break;
            if (pat_cells[p].empty()) continue;

            bool is_hi = mixed ? pattern_is_hires[p] : !multicolor;
            std::size_t W = is_hi ? 8 : 4;
            int classes = is_hi ? 2 : 4;
            std::size_t bits_per = is_hi ? 1 : 2;
            std::uint8_t mask = static_cast<std::uint8_t>((1u << bits_per) - 1);

            // Decode pattern to class grid [y][x].
            std::array<std::array<std::uint8_t, 8>, 8> grid{};
            for (std::size_t y = 0; y < 8; ++y)
                for (std::size_t x = 0; x < W; ++x) {
                    auto shift = (W - 1 - x) * bits_per;
                    grid[y][x] = static_cast<std::uint8_t>(
                        (patterns[p][y] >> shift) & mask);
                }

            auto class_color = [&](std::uint8_t klass, std::size_t ci)
                -> const OKLab& {
                if (is_hi)
                    return klass == 0 ? pal_lab[bg] : pal_lab[color_ram[ci]];
                switch (klass) {
                    case 0: return pal_lab[bg];
                    case 1: return pal_lab[mc1];
                    case 2: return pal_lab[mc2];
                    default: return pal_lab[color_ram[ci]];
                }
            };

            auto inv_n = 1.0f / static_cast<float>(pat_cells[p].size());
            std::size_t local_flips = 0;

            constexpr std::array<int, 4> dx{-1, 1, 0, 0};
            constexpr std::array<int, 4> dy{0, 0, -1, 1};

            for (int pass = 0; pass < 8; ++pass) {
                bool any_flip = false;
                for (std::size_t y = 0; y < 8; ++y) {
                    for (std::size_t x = 0; x < W; ++x) {
                        std::uint8_t cur = grid[y][x];
                        float best_score = 0.0f;
                        std::uint8_t best_class = cur;

                        for (int c2 = 0; c2 < classes; ++c2) {
                            if (c2 == cur) continue;

                            // Smoothness delta (pattern-local)
                            int smooth_delta = 0;
                            for (std::size_t k = 0; k < 4; ++k) {
                                auto nxs = static_cast<long long>(x) + dx[k];
                                auto nys = static_cast<long long>(y) + dy[k];
                                if (nxs < 0 ||
                                    nxs >= static_cast<long long>(W))
                                    continue;
                                if (nys < 0 || nys >= 8) continue;
                                auto nc = grid[static_cast<std::size_t>(nys)]
                                             [static_cast<std::size_t>(nxs)];
                                if (nc == cur) --smooth_delta;
                                if (nc == static_cast<std::uint8_t>(c2))
                                    ++smooth_delta;
                            }
                            // Reject flips whose smoothness gain is below
                            // the minimum permitted by current strength.
                            // Also the natural rejection: non-positive
                            // smooth_delta can never beat positive lambda
                            // * fid_avg.
                            if (smooth_delta < min_smooth_delta) continue;

                            // Fidelity delta per cell, averaged
                            float fid_sum = 0.0f;
                            auto pix_idx = y * W + x;
                            for (auto ci : pat_cells[p]) {
                                auto& px = cells_lab[ci][pix_idx];
                                auto& oc = class_color(cur, ci);
                                auto& nc = class_color(
                                    static_cast<std::uint8_t>(c2), ci);
                                float dOL = px.L - oc.L;
                                float dOa = px.a - oc.a;
                                float dOb = px.b - oc.b;
                                float dNL = px.L - nc.L;
                                float dNa = px.a - nc.a;
                                float dNb = px.b - nc.b;
                                float e_old =
                                    dOL * dOL + dOa * dOa + dOb * dOb;
                                float e_new =
                                    dNL * dNL + dNa * dNa + dNb * dNb;
                                fid_sum += e_new - e_old;
                            }
                            float fid_avg = fid_sum * inv_n;

                            float score = static_cast<float>(smooth_delta)
                                          - lambda * fid_avg;
                            if (score > best_score) {
                                best_score = score;
                                best_class = static_cast<std::uint8_t>(c2);
                            }
                        }

                        if (best_class != cur) {
                            grid[y][x] = best_class;
                            any_flip = true;
                            ++local_flips;
                        }
                    }
                }
                if (!any_flip) break;
            }

            // Re-encode
            for (std::size_t y = 0; y < 8; ++y) {
                std::uint8_t byte = 0;
                for (std::size_t x = 0; x < W; ++x) {
                    auto shift = (W - 1 - x) * bits_per;
                    byte |= static_cast<std::uint8_t>(grid[y][x] << shift);
                }
                patterns[p][y] = byte;
            }
            total_flips.fetch_add(local_flips, std::memory_order_relaxed);
        }
    };

    auto nt = std::min<std::size_t>(hw_threads(), 256);
    std::vector<std::jthread> threads;
    threads.reserve(nt - 1);
    for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(worker);
    worker();

    log_println("  Denoise (strength={:.2f}, lambda={:.1f}, min_smooth={}): {} bit flips",
                lambda_user, lambda, min_smooth_delta, total_flips.load());
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
    bool recompute_centroids,
    quantize::Metric metric = quantize::Metric::mse) {

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

    // ---- Per-cell metric precompute ----
    namespace bu = quantize::blur_util;
    std::vector<std::array<bu::Tap, 9>> kernel_taps;
    std::vector<std::vector<OKLab>> blurred_src;
    std::vector<bu::ClosedCtx> closed_ctx;  // hires blur: closed-form invariants
    struct CellSsim {
        OKLab mu_s, var_s, total_sum;
        float A_norm;
    };
    std::vector<CellSsim> ssim_pre;

    // MC-blur closed-form precompute (filled lazily once patterns are known).
    //
    // For a fixed pattern, B[p] ∈ {0,1,2,3} comes directly from the
    // pattern bits (not nearest-colour). Class-c map m_k(q) = Σ_{p∈class k}
    // K[q,p] is then pattern-only. Combined with per-cell B''[p] =
    // Σ_q K[q,p]·blurred_src[q] (the transpose-blur of the source), the
    // per-(cell, pattern, pc) error reduces to
    //   err = cp_const + Q_pat·||p_pc||² + p_pc·cp_lin
    // where cp_const, cp_lin are O(1)-evaluable per (cell, pattern). Drops
    // the per-call cost from ~1200 ops to ~5 (~12× on the assign loop).
    std::vector<std::array<OKLab, 64>> b_double_blurred;
    std::vector<float> sum_DD;
    std::array<float, 256> Q_pat{};
    std::array<OKLab, 256> V_pat{};
    std::array<float, 256> W_pat{};
    std::vector<float> cp_const;     // [ci * 256 + pat]
    std::vector<OKLab> cp_lin;       // [ci * 256 + pat]
    bool mc_blur_precompute_valid = false;
    auto mc_blur_build_precompute = [&] {
        // Per-cell: B''[p] = Σ_q K[q,p]·blurred_src[q]  and  sum_DD[ci].
        if (b_double_blurred.empty()) {
            b_double_blurred.assign(num_cells, std::array<OKLab, 64>{});
            sum_DD.assign(num_cells, 0.0f);
            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                auto& bsrc = blurred_src[ci];
                auto& bb = b_double_blurred[ci];
                for (std::size_t q = 0; q < pixels_per_cell; ++q) {
                    sum_DD[ci] += bsrc[q].L * bsrc[q].L
                                + bsrc[q].a * bsrc[q].a
                                + bsrc[q].b * bsrc[q].b;
                    for (auto& tap : kernel_taps[q]) {
                        bb[tap.q].L += tap.w * bsrc[q].L;
                        bb[tap.q].a += tap.w * bsrc[q].a;
                        bb[tap.q].b += tap.w * bsrc[q].b;
                    }
                }
            }
        }
        // Per-pattern: pat_class[pat][p] ∈ {0,1,2,3}, then derive m_k(q),
        // A_jk, then W_pat / V_pat / Q_pat.
        auto& p_bg  = pal_lab[bg];
        auto& p_mc1 = pal_lab[mc1];
        auto& p_mc2 = pal_lab[mc2];
        float bg_norm  = p_bg.L*p_bg.L + p_bg.a*p_bg.a + p_bg.b*p_bg.b;
        float mc1_norm = p_mc1.L*p_mc1.L + p_mc1.a*p_mc1.a + p_mc1.b*p_mc1.b;
        float mc2_norm = p_mc2.L*p_mc2.L + p_mc2.a*p_mc2.a + p_mc2.b*p_mc2.b;
        float bg_mc1 = p_bg.L*p_mc1.L + p_bg.a*p_mc1.a + p_bg.b*p_mc1.b;
        float bg_mc2 = p_bg.L*p_mc2.L + p_bg.a*p_mc2.a + p_bg.b*p_mc2.b;
        float mc1_mc2 = p_mc1.L*p_mc2.L + p_mc1.a*p_mc2.a + p_mc1.b*p_mc2.b;
        // Per-pattern class lists.
        std::array<std::array<std::array<std::uint8_t, 32>, 4>, 256> pat_class_pixels{};
        std::array<std::array<std::uint8_t, 4>, 256> pat_class_count{};
        for (std::size_t pi = 0; pi < 256; ++pi) {
            auto& pat = patterns[pi];
            for (std::size_t row = 0; row < 8; ++row) {
                for (std::size_t col = 0; col < 4; ++col) {
                    auto bits = static_cast<std::uint8_t>(
                        (pat[row] >> (6 - col * 2)) & 0x03);
                    auto p = static_cast<std::uint8_t>(row * 4 + col);
                    pat_class_pixels[pi][bits][pat_class_count[pi][bits]++] = p;
                }
            }
        }
        // Per-pattern m_k(q) and A_jk.
        for (std::size_t pi = 0; pi < 256; ++pi) {
            std::array<std::array<float, 32>, 4> m{};
            for (std::uint8_t k = 0; k < 4; ++k) {
                for (std::uint8_t i = 0; i < pat_class_count[pi][k]; ++i) {
                    auto p = pat_class_pixels[pi][k][i];
                    // Find taps[q] entries that read pixel p — equivalent to:
                    // m_k(q) += K[q, p] for each q.
                    for (std::size_t q = 0; q < pixels_per_cell; ++q)
                        for (auto& tap : kernel_taps[q])
                            if (tap.q == p) m[k][q] += tap.w;
                }
            }
            float A00 = 0, A11 = 0, A22 = 0, A33 = 0;
            float A01 = 0, A02 = 0, A03 = 0, A12 = 0, A13 = 0, A23 = 0;
            for (std::size_t q = 0; q < pixels_per_cell; ++q) {
                A00 += m[0][q]*m[0][q]; A11 += m[1][q]*m[1][q];
                A22 += m[2][q]*m[2][q]; A33 += m[3][q]*m[3][q];
                A01 += m[0][q]*m[1][q]; A02 += m[0][q]*m[2][q];
                A03 += m[0][q]*m[3][q]; A12 += m[1][q]*m[2][q];
                A13 += m[1][q]*m[3][q]; A23 += m[2][q]*m[3][q];
            }
            W_pat[pi] = A00 * bg_norm + A11 * mc1_norm + A22 * mc2_norm
                      + 2.0f * (A01 * bg_mc1 + A02 * bg_mc2 + A12 * mc1_mc2);
            V_pat[pi].L = A03 * p_bg.L + A13 * p_mc1.L + A23 * p_mc2.L;
            V_pat[pi].a = A03 * p_bg.a + A13 * p_mc1.a + A23 * p_mc2.a;
            V_pat[pi].b = A03 * p_bg.b + A13 * p_mc1.b + A23 * p_mc2.b;
            Q_pat[pi] = A33;
        }
        // Per (cell, pattern): DM_k via B''[p] gathered over class lists,
        // then cp_const = W_pat + R_cp, cp_lin = 2·V_pat + S_cp.
        cp_const.assign(num_cells * 256, 0.0f);
        cp_lin.assign(num_cells * 256, OKLab{0, 0, 0});
        std::atomic<std::size_t> next{0};
        auto build = [&] {
            while (true) {
                auto ci = next.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;
                auto& bb = b_double_blurred[ci];
                for (std::size_t pi = 0; pi < 256; ++pi) {
                    OKLab DM[4] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0}};
                    for (std::uint8_t k = 0; k < 4; ++k) {
                        for (std::uint8_t i = 0; i < pat_class_count[pi][k]; ++i) {
                            auto p = pat_class_pixels[pi][k][i];
                            DM[k].L += bb[p].L;
                            DM[k].a += bb[p].a;
                            DM[k].b += bb[p].b;
                        }
                    }
                    float R_cp = -2.0f * (DM[0].L * p_bg.L + DM[0].a * p_bg.a
                                                  + DM[0].b * p_bg.b
                                       + DM[1].L * p_mc1.L + DM[1].a * p_mc1.a
                                                  + DM[1].b * p_mc1.b
                                       + DM[2].L * p_mc2.L + DM[2].a * p_mc2.a
                                                  + DM[2].b * p_mc2.b)
                              + sum_DD[ci];
                    cp_const[ci * 256 + pi] = W_pat[pi] + R_cp;
                    cp_lin[ci * 256 + pi].L = 2.0f * V_pat[pi].L - 2.0f * DM[3].L;
                    cp_lin[ci * 256 + pi].a = 2.0f * V_pat[pi].a - 2.0f * DM[3].a;
                    cp_lin[ci * 256 + pi].b = 2.0f * V_pat[pi].b - 2.0f * DM[3].b;
                }
            }
        };
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), num_cells);
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(build);
        build();
        mc_blur_precompute_valid = true;
    };

    if (metric == quantize::Metric::blur) {
        kernel_taps = bu::build_kernel_taps(cell_w, 8);
        blurred_src.resize(num_cells);
        for (std::size_t ci = 0; ci < num_cells; ++ci)
            blurred_src[ci] = bu::compute_blurred(cells_lab[ci], kernel_taps);
        if (!multicolor) {
            closed_ctx.resize(num_cells);
            for (std::size_t ci = 0; ci < num_cells; ++ci)
                closed_ctx[ci] = bu::make_closed_ctx(blurred_src[ci]);
        }
    } else if (metric == quantize::Metric::ssim) {
        ssim_pre.resize(num_cells);
        float inv_n = 1.0f / static_cast<float>(pixels_per_cell);
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            OKLab mu{0, 0, 0};
            for (auto& v : cells_lab[ci]) {
                mu.L += v.L; mu.a += v.a; mu.b += v.b;
            }
            ssim_pre[ci].total_sum = mu;
            mu.L *= inv_n; mu.a *= inv_n; mu.b *= inv_n;
            OKLab var{0, 0, 0};
            float A = 0;
            for (auto& v : cells_lab[ci]) {
                float dL = v.L - mu.L;
                float da = v.a - mu.a;
                float db = v.b - mu.b;
                var.L += dL * dL;
                var.a += da * da;
                var.b += db * db;
                A += v.L * v.L + v.a * v.a + v.b * v.b;
            }
            var.L *= inv_n; var.a *= inv_n; var.b *= inv_n;
            ssim_pre[ci].mu_s = mu;
            ssim_pre[ci].var_s = var;
            ssim_pre[ci].A_norm = A;
        }
    }

    // Render a (pattern, colors) into an OKLab cell.
    auto render_cell = [&](const Pattern& pat, std::uint8_t pc,
                            std::array<OKLab, 64>& out) {
        if (multicolor) {
            std::array<OKLab, 4> colors = {
                pal_lab[bg], pal_lab[mc1], pal_lab[mc2], pal_lab[pc]};
            for (std::size_t row = 0; row < 8; ++row)
                for (std::size_t col = 0; col < 4; ++col) {
                    auto bits = static_cast<std::size_t>(
                        (pat[row] >> (6 - col * 2)) & 0x03);
                    out[row * 4 + col] = colors[bits];
                }
        } else {
            auto& bgl = pal_lab[bg];
            auto& fgl = pal_lab[pc];
            for (std::size_t row = 0; row < 8; ++row)
                for (std::size_t col = 0; col < 8; ++col) {
                    bool is_fg = ((pat[row] >> (7 - col)) & 1) != 0;
                    out[row * 8 + col] = is_fg ? fgl : bgl;
                }
        }
    };

    // Metric-aware per-cell error for a candidate (pattern, pc).
    // For blur: render → blur → MSE vs precomputed blurred source.
    // For ssim: render → SSIM (sum over L/a/b) + λ·MSE hybrid.
    // For mse: delegates to the original cell_pattern_error.
    // Fast O(1) per-(cell, pat, pc) using the MC-blur precompute. Only
    // valid when patterns haven't been mutated since precompute.
    auto cell_err_mc_blur_fast = [&](std::size_t ci, std::size_t pat_idx,
                                       std::uint8_t pc) {
        auto& p_pc = pal_lab[pc];
        float pc_norm = p_pc.L*p_pc.L + p_pc.a*p_pc.a + p_pc.b*p_pc.b;
        auto k = ci * 256 + pat_idx;
        auto& lin = cp_lin[k];
        return cp_const[k] + Q_pat[pat_idx] * pc_norm
             + p_pc.L * lin.L + p_pc.a * lin.a + p_pc.b * lin.b;
    };

    auto cell_err_metric = [&](std::size_t ci, const Pattern& pat,
                                std::uint8_t pc) -> float {
        if (metric == quantize::Metric::mse) {
            return cell_pattern_error(cells_lab[ci], pat, pal_lab,
                                       multicolor, bg, mc1, mc2, pc);
        }
        if (metric == quantize::Metric::blur && !multicolor) {
            // Closed-form 2-colour: bitmap B[p] = pattern bit at pixel p.
            std::uint64_t B = 0;
            for (std::size_t row = 0; row < 8; ++row) {
                auto byte = pat[row];
                for (std::size_t col = 0; col < 8; ++col) {
                    if ((byte >> (7 - col)) & 1)
                        B |= std::uint64_t{1} << (row * 8 + col);
                }
            }
            return bu::score_cell_blur_2color_bitmap(
                B, blurred_src[ci], kernel_taps, pal_lab,
                closed_ctx[ci], bg, pc);
        }
        std::array<OKLab, 64> rendered{};
        render_cell(pat, pc, rendered);
        if (metric == quantize::Metric::blur) {
            // MC: 4-colour pattern, no closed-form; blur the rendered cell
            // and MSE against the precomputed blurred source.
            float err = 0;
            auto& bsrc = blurred_src[ci];
            for (std::size_t p = 0; p < pixels_per_cell; ++p) {
                OKLab br{0, 0, 0};
                for (auto& tap : kernel_taps[p]) {
                    auto& v = rendered[tap.q];
                    br.L += tap.w * v.L;
                    br.a += tap.w * v.a;
                    br.b += tap.w * v.b;
                }
                float dL = bsrc[p].L - br.L;
                float da = bsrc[p].a - br.a;
                float db = bsrc[p].b - br.b;
                err += dL * dL + da * da + db * db;
            }
            return err;
        }
        // ssim hybrid.
        constexpr float C1 = 0.01f * 0.01f;
        constexpr float C2 = 0.03f * 0.03f;
        constexpr float kMseLambda = 1.0f;
        float inv_n = 1.0f / static_cast<float>(pixels_per_cell);
        auto& sp = ssim_pre[ci];
        OKLab mu_r{0, 0, 0};
        float mse = 0;
        for (std::size_t p = 0; p < pixels_per_cell; ++p) {
            mu_r.L += rendered[p].L;
            mu_r.a += rendered[p].a;
            mu_r.b += rendered[p].b;
            float dL = cells_lab[ci][p].L - rendered[p].L;
            float da = cells_lab[ci][p].a - rendered[p].a;
            float db = cells_lab[ci][p].b - rendered[p].b;
            mse += dL * dL + da * da + db * db;
        }
        mu_r.L *= inv_n; mu_r.a *= inv_n; mu_r.b *= inv_n;
        OKLab var_r{0, 0, 0}, cov{0, 0, 0};
        for (std::size_t p = 0; p < pixels_per_cell; ++p) {
            float drL = rendered[p].L - mu_r.L;
            float dra = rendered[p].a - mu_r.a;
            float drb = rendered[p].b - mu_r.b;
            var_r.L += drL * drL;
            var_r.a += dra * dra;
            var_r.b += drb * drb;
            cov.L += drL * (cells_lab[ci][p].L - sp.mu_s.L);
            cov.a += dra * (cells_lab[ci][p].a - sp.mu_s.a);
            cov.b += drb * (cells_lab[ci][p].b - sp.mu_s.b);
        }
        var_r.L *= inv_n; var_r.a *= inv_n; var_r.b *= inv_n;
        cov.L *= inv_n; cov.a *= inv_n; cov.b *= inv_n;
        auto ssim_ch = [&](float ms, float vs, float cv,
                            float mr, float vr) {
            float num = (2.0f * ms * mr + C1) * (2.0f * cv + C2);
            float den = (ms * ms + mr * mr + C1) * (vs + vr + C2);
            return num / den;
        };
        float ssim = ssim_ch(sp.mu_s.L, sp.var_s.L, cov.L, mu_r.L, var_r.L)
                   + ssim_ch(sp.mu_s.a, sp.var_s.a, cov.a, mu_r.a, var_r.a)
                   + ssim_ch(sp.mu_s.b, sp.var_s.b, cov.b, mu_r.b, var_r.b);
        return -ssim + kMseLambda * mse;
    };

    for (int iter = 0; iter < max_iters; ++iter) {
        std::size_t changes = 0;

        // (Re)build the MC-blur precompute when patterns may have changed.
        // First iter always builds; subsequent iters only rebuild after a
        // centroid recompute (Step 3) mutated patterns.
        bool use_mc_blur_fast = (metric == quantize::Metric::blur && multicolor);
        if (use_mc_blur_fast && !mc_blur_precompute_valid) {
            mc_blur_build_precompute();
        }

        // Step 1: Reassign each cell to best pattern (with current per-cell color)
        std::atomic<std::size_t> next_cell{0};
        std::atomic<std::size_t> atomic_changes{0};

        auto assign_worker = [&] {
            while (true) {
                auto ci = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;

                auto current_pc = color_ram[ci];
                float best_err;
                auto best_pat = assignments[ci];
                if (use_mc_blur_fast) {
                    best_err = cell_err_mc_blur_fast(ci, assignments[ci],
                                                       current_pc);
                    for (std::size_t p = 0; p < 256; ++p) {
                        if (p == assignments[ci]) continue;
                        float err = cell_err_mc_blur_fast(ci, p, current_pc);
                        if (err < best_err) {
                            best_err = err;
                            best_pat = static_cast<std::uint8_t>(p);
                        }
                    }
                } else {
                    best_err = cell_err_metric(ci, patterns[assignments[ci]],
                                                  current_pc);
                    for (std::size_t p = 0; p < 256; ++p) {
                        if (p == assignments[ci]) continue;
                        float err = cell_err_metric(ci, patterns[p], current_pc);
                        if (err < best_err) {
                            best_err = err;
                            best_pat = static_cast<std::uint8_t>(p);
                        }
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

                float err = use_mc_blur_fast
                    ? cell_err_mc_blur_fast(ci, assignments[ci], pc)
                    : cell_err_metric(ci, patterns[assignments[ci]], pc);
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
        //
        // For MSE: per-pixel decisions are independent, picked via simple
        //   sum-of-distances over cells assigned to each pattern.
        // For blur/ssim: per-pixel decisions are *coupled* (kernel for blur,
        //   joint stats for ssim), so we do a greedy single pass per pixel —
        //   try each candidate value, score the full cell metric summed over
        //   cells, keep the best. Slower per iter but converges in fewer LBG
        //   iterations because patterns aren't fighting the assign metric.
        if (recompute_centroids) {
            // Centroids modify patterns; the MC-blur precompute is now
            // stale and must be rebuilt next iter.
            mc_blur_precompute_valid = false;
            std::array<std::vector<std::size_t>, 256> pat_cells;
            for (std::size_t ci = 0; ci < num_cells; ++ci)
                pat_cells[assignments[ci]].push_back(ci);

            for (std::size_t p = 0; p < 256; ++p) {
                if (p == 0) continue; // slot 0 pinned as empty pattern
                if (pat_cells[p].empty()) continue;

                Pattern new_pat = patterns[p];
                if (metric == quantize::Metric::mse) {
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
                } else {
                    // Blur / SSIM: Direct Binary Search. Per-pixel greedy
                    // flips are coupled by the blur kernel, so a single
                    // pass is not enough — flipping pixel A may unlock
                    // a better choice at neighboring pixel B that the
                    // first pass evaluated against the old A. We iterate
                    // until a full pass yields no flips. Capped at 8
                    // passes (typically converges in 2-3).
                    auto bit_count = multicolor ? 4 : 2;
                    for (int dbs_pass = 0; dbs_pass < 8; ++dbs_pass) {
                        bool any_flip = false;
                        for (std::size_t row = 0; row < 8; ++row) {
                            for (std::size_t col = 0; col < cell_w; ++col) {
                                auto shift = multicolor
                                    ? static_cast<std::uint8_t>(6 - col * 2)
                                    : static_cast<std::uint8_t>(7 - col);
                                auto mask = static_cast<std::uint8_t>(
                                    multicolor ? (0x03u << shift) : (0x01u << shift));
                                auto current_bits = static_cast<std::uint8_t>(
                                    (new_pat[row] & mask) >> shift);
                                float best_total = std::numeric_limits<float>::max();
                                std::uint8_t best_bits = current_bits;
                                for (int bits = 0; bits < bit_count; ++bits) {
                                    Pattern trial = new_pat;
                                    trial[row] = static_cast<std::uint8_t>(
                                        (trial[row] & ~mask) |
                                        (static_cast<std::uint8_t>(bits) << shift));
                                    float total = 0.0f;
                                    for (auto ci : pat_cells[p]) {
                                        total += cell_err_metric(ci, trial,
                                                                  color_ram[ci]);
                                    }
                                    if (total < best_total) {
                                        best_total = total;
                                        best_bits = static_cast<std::uint8_t>(bits);
                                    }
                                }
                                if (best_bits != current_bits) {
                                    new_pat[row] = static_cast<std::uint8_t>(
                                        (new_pat[row] & ~mask) | (best_bits << shift));
                                    any_flip = true;
                                }
                            }
                        }
                        if (!any_flip) break;
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
    int max_iters, bool recompute_centroids,
    quantize::Metric metric = quantize::Metric::mse) {

    auto pal_lab = precompute_lab(palette);
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

    // Per-cell metric precompute. Mixed mode has two cell shapes (8×8
    // hires / 4×8 MC), so we keep separate kernel-tap tables and per-cell
    // SSIM stats, indexed by the cell's hires/MC mode.
    namespace bu = quantize::blur_util;
    auto kernel_taps_hi = bu::build_kernel_taps(8, 8);
    auto kernel_taps_mc = bu::build_kernel_taps(4, 8);

    std::vector<std::vector<OKLab>> blurred_src;
    std::vector<bu::ClosedCtx> closed_ctx;  // hires cells only
    struct CellSsim { OKLab mu_s, var_s; };
    std::vector<CellSsim> ssim_pre;
    // MC-cell blur precompute (same closed-form decomposition used by
    // refine_charset; see comment there). Built lazily once patterns
    // are known and rebuilt only after a centroid recompute.
    std::vector<std::array<OKLab, 32>> mc_b_double_blurred;
    std::vector<float> mc_sum_DD;
    std::array<float, 256> mc_Q_pat{};
    std::array<OKLab, 256> mc_V_pat{};
    std::array<float, 256> mc_W_pat{};
    std::vector<float> mc_cp_const;     // [ci * 256 + pat]; ci must be MC
    std::vector<OKLab> mc_cp_lin;
    bool mc_blur_precompute_valid = false;

    if (metric == quantize::Metric::blur) {
        blurred_src.resize(num_cells);
        closed_ctx.resize(num_cells);
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            auto& taps = cell_is_hires[ci] ? kernel_taps_hi : kernel_taps_mc;
            blurred_src[ci] = bu::compute_blurred(cells_lab[ci], taps);
            if (cell_is_hires[ci])
                closed_ctx[ci] = bu::make_closed_ctx(blurred_src[ci]);
        }
    } else if (metric == quantize::Metric::ssim) {
        ssim_pre.resize(num_cells);
        for (std::size_t ci = 0; ci < num_cells; ++ci) {
            std::size_t n_pix = cells_lab[ci].size();
            float inv_n = 1.0f / static_cast<float>(n_pix);
            OKLab mu{0, 0, 0};
            for (auto& v : cells_lab[ci]) { mu.L += v.L; mu.a += v.a; mu.b += v.b; }
            mu.L *= inv_n; mu.a *= inv_n; mu.b *= inv_n;
            OKLab var{0, 0, 0};
            for (auto& v : cells_lab[ci]) {
                float dL = v.L - mu.L, da = v.a - mu.a, db = v.b - mu.b;
                var.L += dL * dL;
                var.a += da * da;
                var.b += db * db;
            }
            var.L *= inv_n; var.a *= inv_n; var.b *= inv_n;
            ssim_pre[ci].mu_s = mu;
            ssim_pre[ci].var_s = var;
        }
    }

    // Render a (pattern, pc) into an OKLab cell using the cell's mode.
    auto render_cell = [&](std::size_t ci, const Pattern& pat,
                            std::uint8_t pc, std::array<OKLab, 64>& out) {
        bool is_hi = cell_is_hires[ci];
        if (!is_hi) {
            std::array<OKLab, 4> colors = {
                pal_lab[bg], pal_lab[mc1], pal_lab[mc2], pal_lab[pc]};
            for (std::size_t row = 0; row < 8; ++row)
                for (std::size_t col = 0; col < 4; ++col) {
                    auto bits = static_cast<std::size_t>(
                        (pat[row] >> (6 - col * 2)) & 0x03);
                    out[row * 4 + col] = colors[bits];
                }
        } else {
            auto& bgl = pal_lab[bg];
            auto& fgl = pal_lab[pc];
            for (std::size_t row = 0; row < 8; ++row)
                for (std::size_t col = 0; col < 8; ++col) {
                    bool is_fg = ((pat[row] >> (7 - col)) & 1) != 0;
                    out[row * 8 + col] = is_fg ? fgl : bgl;
                }
        }
    };

    // MC-blur precompute builder (mirrors refine_charset; only MC cells).
    auto mc_blur_build_precompute = [&] {
        if (mc_b_double_blurred.empty()) {
            mc_b_double_blurred.assign(num_cells, std::array<OKLab, 32>{});
            mc_sum_DD.assign(num_cells, 0.0f);
            for (std::size_t ci = 0; ci < num_cells; ++ci) {
                if (cell_is_hires[ci]) continue;
                auto& bsrc = blurred_src[ci];
                auto& bb = mc_b_double_blurred[ci];
                for (std::size_t q = 0; q < 32; ++q) {
                    mc_sum_DD[ci] += bsrc[q].L * bsrc[q].L
                                   + bsrc[q].a * bsrc[q].a
                                   + bsrc[q].b * bsrc[q].b;
                    for (auto& tap : kernel_taps_mc[q]) {
                        bb[tap.q].L += tap.w * bsrc[q].L;
                        bb[tap.q].a += tap.w * bsrc[q].a;
                        bb[tap.q].b += tap.w * bsrc[q].b;
                    }
                }
            }
        }
        auto& p_bg  = pal_lab[bg];
        auto& p_mc1 = pal_lab[mc1];
        auto& p_mc2 = pal_lab[mc2];
        float bg_norm  = p_bg.L*p_bg.L + p_bg.a*p_bg.a + p_bg.b*p_bg.b;
        float mc1_norm = p_mc1.L*p_mc1.L + p_mc1.a*p_mc1.a + p_mc1.b*p_mc1.b;
        float mc2_norm = p_mc2.L*p_mc2.L + p_mc2.a*p_mc2.a + p_mc2.b*p_mc2.b;
        float bg_mc1 = p_bg.L*p_mc1.L + p_bg.a*p_mc1.a + p_bg.b*p_mc1.b;
        float bg_mc2 = p_bg.L*p_mc2.L + p_bg.a*p_mc2.a + p_bg.b*p_mc2.b;
        float mc1_mc2 = p_mc1.L*p_mc2.L + p_mc1.a*p_mc2.a + p_mc1.b*p_mc2.b;
        std::array<std::array<std::array<std::uint8_t, 32>, 4>, 256> pat_class_pixels{};
        std::array<std::array<std::uint8_t, 4>, 256> pat_class_count{};
        for (std::size_t pi = 0; pi < 256; ++pi) {
            if (pattern_is_hires[pi]) continue;
            auto& pat = patterns[pi];
            for (std::size_t row = 0; row < 8; ++row) {
                for (std::size_t col = 0; col < 4; ++col) {
                    auto bits = static_cast<std::uint8_t>(
                        (pat[row] >> (6 - col * 2)) & 0x03);
                    auto p = static_cast<std::uint8_t>(row * 4 + col);
                    pat_class_pixels[pi][bits][pat_class_count[pi][bits]++] = p;
                }
            }
        }
        for (std::size_t pi = 0; pi < 256; ++pi) {
            if (pattern_is_hires[pi]) continue;
            std::array<std::array<float, 32>, 4> m{};
            for (std::uint8_t k = 0; k < 4; ++k) {
                for (std::uint8_t i = 0; i < pat_class_count[pi][k]; ++i) {
                    auto p = pat_class_pixels[pi][k][i];
                    for (std::size_t q = 0; q < 32; ++q)
                        for (auto& tap : kernel_taps_mc[q])
                            if (tap.q == p) m[k][q] += tap.w;
                }
            }
            float A00 = 0, A11 = 0, A22 = 0, A33 = 0;
            float A01 = 0, A02 = 0, A03 = 0, A12 = 0, A13 = 0, A23 = 0;
            for (std::size_t q = 0; q < 32; ++q) {
                A00 += m[0][q]*m[0][q]; A11 += m[1][q]*m[1][q];
                A22 += m[2][q]*m[2][q]; A33 += m[3][q]*m[3][q];
                A01 += m[0][q]*m[1][q]; A02 += m[0][q]*m[2][q];
                A03 += m[0][q]*m[3][q]; A12 += m[1][q]*m[2][q];
                A13 += m[1][q]*m[3][q]; A23 += m[2][q]*m[3][q];
            }
            mc_W_pat[pi] = A00 * bg_norm + A11 * mc1_norm + A22 * mc2_norm
                         + 2.0f * (A01 * bg_mc1 + A02 * bg_mc2 + A12 * mc1_mc2);
            mc_V_pat[pi].L = A03 * p_bg.L + A13 * p_mc1.L + A23 * p_mc2.L;
            mc_V_pat[pi].a = A03 * p_bg.a + A13 * p_mc1.a + A23 * p_mc2.a;
            mc_V_pat[pi].b = A03 * p_bg.b + A13 * p_mc1.b + A23 * p_mc2.b;
            mc_Q_pat[pi] = A33;
        }
        mc_cp_const.assign(num_cells * 256, 0.0f);
        mc_cp_lin.assign(num_cells * 256, OKLab{0, 0, 0});
        std::atomic<std::size_t> next{0};
        auto build = [&] {
            while (true) {
                auto ci = next.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;
                if (cell_is_hires[ci]) continue;
                auto& bb = mc_b_double_blurred[ci];
                for (std::size_t pi = 0; pi < 256; ++pi) {
                    if (pattern_is_hires[pi]) continue;
                    OKLab DM[4] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0}};
                    for (std::uint8_t k = 0; k < 4; ++k) {
                        for (std::uint8_t i = 0; i < pat_class_count[pi][k]; ++i) {
                            auto p = pat_class_pixels[pi][k][i];
                            DM[k].L += bb[p].L;
                            DM[k].a += bb[p].a;
                            DM[k].b += bb[p].b;
                        }
                    }
                    float R_cp = -2.0f * (DM[0].L * p_bg.L + DM[0].a * p_bg.a
                                                  + DM[0].b * p_bg.b
                                       + DM[1].L * p_mc1.L + DM[1].a * p_mc1.a
                                                  + DM[1].b * p_mc1.b
                                       + DM[2].L * p_mc2.L + DM[2].a * p_mc2.a
                                                  + DM[2].b * p_mc2.b)
                              + mc_sum_DD[ci];
                    mc_cp_const[ci * 256 + pi] = mc_W_pat[pi] + R_cp;
                    mc_cp_lin[ci * 256 + pi].L = 2.0f * mc_V_pat[pi].L - 2.0f * DM[3].L;
                    mc_cp_lin[ci * 256 + pi].a = 2.0f * mc_V_pat[pi].a - 2.0f * DM[3].a;
                    mc_cp_lin[ci * 256 + pi].b = 2.0f * mc_V_pat[pi].b - 2.0f * DM[3].b;
                }
            }
        };
        auto nt = std::min(static_cast<std::size_t>(hw_threads()), num_cells);
        std::vector<std::jthread> threads;
        threads.reserve(nt - 1);
        for (std::size_t t = 1; t < nt; ++t) threads.emplace_back(build);
        build();
        mc_blur_precompute_valid = true;
    };

    auto cell_err_mc_blur_fast = [&](std::size_t ci, std::size_t pat_idx,
                                       std::uint8_t pc) {
        auto& p_pc = pal_lab[pc];
        float pc_norm = p_pc.L*p_pc.L + p_pc.a*p_pc.a + p_pc.b*p_pc.b;
        auto k = ci * 256 + pat_idx;
        auto& lin = mc_cp_lin[k];
        return mc_cp_const[k] + mc_Q_pat[pat_idx] * pc_norm
             + p_pc.L * lin.L + p_pc.a * lin.a + p_pc.b * lin.b;
    };

    auto cell_err_metric = [&](std::size_t ci, const Pattern& pat,
                                std::uint8_t pc) -> float {
        if (metric == quantize::Metric::mse) {
            return cell_pattern_error(cells_lab[ci], pat, pal_lab,
                                       !cell_is_hires[ci],
                                       bg, mc1, mc2, pc);
        }
        if (metric == quantize::Metric::blur && cell_is_hires[ci]) {
            std::uint64_t B = 0;
            for (std::size_t row = 0; row < 8; ++row) {
                auto byte = pat[row];
                for (std::size_t col = 0; col < 8; ++col)
                    if ((byte >> (7 - col)) & 1)
                        B |= std::uint64_t{1} << (row * 8 + col);
            }
            return bu::score_cell_blur_2color_bitmap(
                B, blurred_src[ci], kernel_taps_hi, pal_lab,
                closed_ctx[ci], bg, pc);
        }
        std::size_t n_pix = cells_lab[ci].size();
        std::array<OKLab, 64> rendered{};
        render_cell(ci, pat, pc, rendered);
        if (metric == quantize::Metric::blur) {
            // MC cell: 4-colour pattern, no closed-form.
            float err = 0;
            auto& bsrc = blurred_src[ci];
            auto& taps = kernel_taps_mc;
            for (std::size_t p = 0; p < n_pix; ++p) {
                OKLab br{0, 0, 0};
                for (auto& tap : taps[p]) {
                    auto& v = rendered[tap.q];
                    br.L += tap.w * v.L;
                    br.a += tap.w * v.a;
                    br.b += tap.w * v.b;
                }
                float dL = bsrc[p].L - br.L;
                float da = bsrc[p].a - br.a;
                float db = bsrc[p].b - br.b;
                err += dL * dL + da * da + db * db;
            }
            return err;
        }
        // ssim hybrid
        constexpr float C1 = 0.01f * 0.01f;
        constexpr float C2 = 0.03f * 0.03f;
        constexpr float kMseLambda = 1.0f;
        float inv_n = 1.0f / static_cast<float>(n_pix);
        auto& sp = ssim_pre[ci];
        OKLab mu_r{0, 0, 0};
        float mse = 0;
        for (std::size_t p = 0; p < n_pix; ++p) {
            mu_r.L += rendered[p].L;
            mu_r.a += rendered[p].a;
            mu_r.b += rendered[p].b;
            float dL = cells_lab[ci][p].L - rendered[p].L;
            float da = cells_lab[ci][p].a - rendered[p].a;
            float db = cells_lab[ci][p].b - rendered[p].b;
            mse += dL * dL + da * da + db * db;
        }
        mu_r.L *= inv_n; mu_r.a *= inv_n; mu_r.b *= inv_n;
        OKLab var_r{0, 0, 0}, cov{0, 0, 0};
        for (std::size_t p = 0; p < n_pix; ++p) {
            float drL = rendered[p].L - mu_r.L;
            float dra = rendered[p].a - mu_r.a;
            float drb = rendered[p].b - mu_r.b;
            var_r.L += drL * drL;
            var_r.a += dra * dra;
            var_r.b += drb * drb;
            cov.L += drL * (cells_lab[ci][p].L - sp.mu_s.L);
            cov.a += dra * (cells_lab[ci][p].a - sp.mu_s.a);
            cov.b += drb * (cells_lab[ci][p].b - sp.mu_s.b);
        }
        var_r.L *= inv_n; var_r.a *= inv_n; var_r.b *= inv_n;
        cov.L *= inv_n; cov.a *= inv_n; cov.b *= inv_n;
        auto ssim_ch = [&](float ms, float vs, float cv,
                            float mr, float vr) {
            float num = (2.0f * ms * mr + C1) * (2.0f * cv + C2);
            float den = (ms * ms + mr * mr + C1) * (vs + vr + C2);
            return num / den;
        };
        float ssim = ssim_ch(sp.mu_s.L, sp.var_s.L, cov.L, mu_r.L, var_r.L)
                   + ssim_ch(sp.mu_s.a, sp.var_s.a, cov.a, mu_r.a, var_r.a)
                   + ssim_ch(sp.mu_s.b, sp.var_s.b, cov.b, mu_r.b, var_r.b);
        return -ssim + kMseLambda * mse;
    };

    for (int iter = 0; iter < max_iters; ++iter) {
        std::size_t changes = 0;

        // (Re)build MC-blur precompute when patterns may have changed.
        bool use_mc_blur_fast = (metric == quantize::Metric::blur);
        if (use_mc_blur_fast && !mc_blur_precompute_valid)
            mc_blur_build_precompute();

        // Step 1: Reassign each cell to best pattern of matching mode
        std::atomic<std::size_t> next_cell{0};
        std::atomic<std::size_t> atomic_changes{0};

        auto assign_worker = [&] {
            while (true) {
                auto ci = next_cell.fetch_add(1, std::memory_order_relaxed);
                if (ci >= num_cells) break;

                bool is_hi = cell_is_hires[ci];
                auto current_pc = color_ram[ci];
                bool use_fast = use_mc_blur_fast && !is_hi;
                float best_err;
                auto best_pat = assignments[ci];
                if (use_fast) {
                    best_err = cell_err_mc_blur_fast(
                        ci, assignments[ci], current_pc);
                } else {
                    best_err = cell_err_metric(ci, patterns[assignments[ci]],
                                                  current_pc);
                }

                for (std::size_t p = 0; p < 256; ++p) {
                    if (p == assignments[ci]) continue;
                    // Slot 0 is the reserved empty pattern (all bg) and is
                    // eligible for cells of either mode — an all-zero
                    // pattern renders identically as hires or MC.
                    if (p != 0 && pattern_is_hires[p] != is_hi) continue;
                    float err = use_fast
                        ? cell_err_mc_blur_fast(ci, p, current_pc)
                        : cell_err_metric(ci, patterns[p], current_pc);
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
            bool use_fast = use_mc_blur_fast && !is_hi;

            // Both hires and MC limited to 0-7 in MC text mode
            // (hires: bit 3 = MC enable flag; MC: only 3 bits available)
            auto pc_limit = std::uint8_t{8};
            for (std::uint8_t pc = 0; pc < pc_limit; ++pc) {
                if (is_hi && pc == bg) continue;
                if (!is_hi && (pc == bg || pc == mc1 || pc == mc2)) continue;

                float err = use_fast
                    ? cell_err_mc_blur_fast(ci, assignments[ci], pc)
                    : cell_err_metric(ci, patterns[assignments[ci]], pc);
                if (err < best_err) { best_err = err; best_pc = pc; }
            }

            if (best_pc != color_ram[ci]) {
                color_ram[ci] = best_pc;
                ++changes;
            }
        }

        // Step 3: Recompute centroids (when enabled)
        if (recompute_centroids) {
            mc_blur_precompute_valid = false;
            std::array<std::vector<std::size_t>, 256> pat_cells;
            for (std::size_t ci = 0; ci < num_cells; ++ci)
                pat_cells[assignments[ci]].push_back(ci);

            for (std::size_t p = 0; p < 256; ++p) {
                if (p == 0) continue; // slot 0 pinned as empty pattern
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
                              const dither::Settings& dither_settings,
                              quantize::Metric metric,
                              float denoise) {
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
        auto sel = select_colors_mixed(image, palette, cols, rows, metric);
        screen = std::move(sel.screen);
        bg = sel.bg; mc1 = sel.mc1; mc2 = sel.mc2;
        cell_is_hires = std::move(sel.cell_is_hires);
    } else if (multicolor) {
        auto sel = select_colors_multicolor(image, palette, cols, rows, metric);
        screen = std::move(sel.screen);
        bg = sel.bg; mc1 = sel.mc1; mc2 = sel.mc2;
    } else {
        screen = select_colors_hires(image, palette, cols, rows, metric);
        bg = screen.background_color;
    }

    // Phase 2: Apply dithering. Skipped when a perceptual metric is in
    // use — those score against the continuous source and don't want
    // an extra error-diffusion pass on top.
    if (dither_settings.method != dither::Method::none &&
        !dither::is_ordered(dither_settings.method)) {
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

    // Slot 0 is always reserved for the empty (all-background) pattern,
    // whether or not a natural empty entry exists in the dedup set.
    std::vector<std::uint8_t> entry_to_charset(dedup.entries.size(), 0);
    std::size_t next_idx = 1;
    if (empty_entry_idx != std::numeric_limits<std::size_t>::max())
        entry_to_charset[empty_entry_idx] = 0;
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
    // Slot 0 is the reserved empty pattern. Force its mode flag to MC so
    // mixed-mode MC cells can pick it via the MC-blur fast path precompute
    // (pattern_is_hires[0]=true would exclude it from the precompute).
    flat_patterns[0] = empty_pattern;
    flat_pattern_is_hires[0] = false;

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
                             10, !has_dither, metric);
    } else {
        refine_charset(image, palette, multicolor, bg, mc1, mc2,
                       cols, rows, flat_patterns, flat_assignments,
                       flat_color_ram, 10, !has_dither, metric);
    }

    // Phase 7b: cost-aware denoise + one more assignment pass so cells
    // settle on the smoothed patterns.
    if (denoise > 0.0f) {
        denoise_patterns_cost_aware(
            image, palette, multicolor, mixed, bg, mc1, mc2,
            cols, rows, flat_patterns, flat_pattern_is_hires,
            flat_assignments, flat_color_ram, cell_is_hires, denoise);

        if (mixed) {
            refine_charset_mixed(image, palette, bg, mc1, mc2,
                                 cols, rows, flat_patterns,
                                 flat_pattern_is_hires, flat_assignments,
                                 flat_color_ram, cell_is_hires,
                                 2, false, metric);
        } else {
            refine_charset(image, palette, multicolor, bg, mc1, mc2,
                           cols, rows, flat_patterns, flat_assignments,
                           flat_color_ram, 2, false, metric);
        }
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
