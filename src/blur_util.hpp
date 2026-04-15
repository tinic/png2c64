#pragma once

#include "color_space.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Shared helpers for the "blur" perceptual metric (Pappas-Neuhoff). Used
// by both bitmap (quantize.cpp) and charset (charset.cpp) paths. The
// metric scores a candidate cell rendering by blurring it with a 3×3
// binomial low-pass (HVS contrast sensitivity), then taking MSE against
// the blurred source. Two fast paths live here:
//
//   score_cell_blur_2color — closed form for 2-colour candidates
//      (hires / charset-hi). Expands MSE into ||u||²Σm² − 2·u·ΣDm + K
//      where m(q) = Σ_k w_k · B[taps[q].k] and B is the per-pixel
//      winner bitmap. Eliminates the per-candidate OKLab blur pass.
//
//   score_cell_blur_fused — generic k-colour scoring with a per-cell
//      pixel→palette sq-distance LUT + uint8 assignment scratch (cache-
//      friendly gather in the blur loop).

namespace png2c64::quantize::blur_util {

struct Tap {
    std::uint16_t q;
    float w;
};

// Build 3×3 binomial blur kernel taps for a cell_w × cell_h cell with
// replicate padding at the borders. Returns one 9-tap gather per output
// pixel, flattened row-major.
inline std::vector<std::array<Tap, 9>>
build_kernel_taps(std::size_t cell_w, std::size_t cell_h) {
    static constexpr std::array<std::array<float, 3>, 3> kBlur = {{
        {1.0f / 16, 2.0f / 16, 1.0f / 16},
        {2.0f / 16, 4.0f / 16, 2.0f / 16},
        {1.0f / 16, 2.0f / 16, 1.0f / 16},
    }};
    std::vector<std::array<Tap, 9>> taps(cell_w * cell_h);
    for (std::size_t py = 0; py < cell_h; ++py) {
        for (std::size_t px = 0; px < cell_w; ++px) {
            auto p_out = py * cell_w + px;
            std::size_t k = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(static_cast<int>(py) + dy, 0,
                                    static_cast<int>(cell_h) - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(static_cast<int>(px) + dx, 0,
                                        static_cast<int>(cell_w) - 1);
                    taps[p_out][k++] = {
                        static_cast<std::uint16_t>(
                            static_cast<std::size_t>(ny) * cell_w +
                            static_cast<std::size_t>(nx)),
                        kBlur[static_cast<std::size_t>(dy + 1)]
                             [static_cast<std::size_t>(dx + 1)]};
                }
            }
        }
    }
    return taps;
}

// Convolve a cell of OKLab pixels with the 3×3 binomial blur.
inline std::vector<color_space::OKLab> compute_blurred(
    std::span<const color_space::OKLab> cell_lab,
    const std::vector<std::array<Tap, 9>>& taps) {
    std::vector<color_space::OKLab> out(cell_lab.size());
    for (std::size_t p = 0; p < cell_lab.size(); ++p) {
        color_space::OKLab b{0, 0, 0};
        for (auto& tap : taps[p]) {
            auto& v = cell_lab[tap.q];
            b.L += tap.w * v.L;
            b.a += tap.w * v.a;
            b.b += tap.w * v.b;
        }
        out[p] = b;
    }
    return out;
}

// Convolve a whole OKLab image with the same 3×3 binomial blur, using
// replicate padding at the *image* borders (not cell borders). Per-cell
// slices of this result serve as the blur-metric source target, so
// adjacent cells see a coherent blurred gradient instead of each cell's
// own edge-clamped blur. Fixes visible 8×8 cell blocking in smooth
// gradients under --metric blur.
inline std::vector<color_space::OKLab> blur_image_replicate(
    std::span<const color_space::OKLab> in,
    std::size_t w, std::size_t h) {
    static constexpr std::array<std::array<float, 3>, 3> kBlur = {{
        {1.0f / 16, 2.0f / 16, 1.0f / 16},
        {2.0f / 16, 4.0f / 16, 2.0f / 16},
        {1.0f / 16, 2.0f / 16, 1.0f / 16},
    }};
    std::vector<color_space::OKLab> out(in.size());
    auto iw = static_cast<int>(w);
    auto ih = static_cast<int>(h);
    for (int y = 0; y < ih; ++y) {
        for (int x = 0; x < iw; ++x) {
            color_space::OKLab b{0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = std::clamp(y + dy, 0, ih - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = std::clamp(x + dx, 0, iw - 1);
                    auto& v = in[static_cast<std::size_t>(ny) * w +
                                 static_cast<std::size_t>(nx)];
                    float kw = kBlur[static_cast<std::size_t>(dy + 1)]
                                    [static_cast<std::size_t>(dx + 1)];
                    b.L += kw * v.L;
                    b.a += kw * v.a;
                    b.b += kw * v.b;
                }
            }
            out[static_cast<std::size_t>(y) * w +
                static_cast<std::size_t>(x)] = b;
        }
    }
    return out;
}

// Copy a cell-sized region out of a flat (w × h) OKLab image.
inline std::vector<color_space::OKLab> copy_cell_from(
    std::span<const color_space::OKLab> image,
    std::size_t w,
    std::size_t cell_x, std::size_t cell_y,
    std::size_t cell_w, std::size_t cell_h) {
    std::vector<color_space::OKLab> out;
    out.reserve(cell_w * cell_h);
    auto px = cell_x * cell_w;
    auto py = cell_y * cell_h;
    for (std::size_t dy = 0; dy < cell_h; ++dy) {
        auto row = (py + dy) * w + px;
        for (std::size_t dx = 0; dx < cell_w; ++dx) {
            out.push_back(image[row + dx]);
        }
    }
    return out;
}

// Per-cell pixel → palette-color squared-OKLab-distance LUT. Built once
// per cell so every scoring call below avoids the 3-sub / 3-mult per
// (pixel, palette-color) pair.
static constexpr std::size_t kMaxPalette = 16;
using PixelDistLut = std::vector<std::array<float, kMaxPalette>>;

inline PixelDistLut build_pixel_dist_lut(
    std::span<const color_space::OKLab> cell_lab,
    std::span<const color_space::OKLab> pal_lab) {
    PixelDistLut lut(cell_lab.size());
    for (std::size_t p = 0; p < cell_lab.size(); ++p) {
        auto& px = cell_lab[p];
        for (std::size_t c = 0; c < pal_lab.size(); ++c) {
            auto& cl = pal_lab[c];
            float dL = px.L - cl.L, da = px.a - cl.a, db = px.b - cl.b;
            lut[p][c] = dL * dL + da * da + db * db;
        }
    }
    return lut;
}

// Cell-level invariants for the 2-colour closed-form blur score.
struct ClosedCtx {
    float sum_norm_src{};               // Σ ||blurred_src[q]||²
    color_space::OKLab sum_src{};       // Σ blurred_src[q]
};

inline ClosedCtx make_closed_ctx(
    std::span<const color_space::OKLab> blurred_src) {
    ClosedCtx ctx{};
    for (auto& b : blurred_src) {
        ctx.sum_norm_src += b.L * b.L + b.a * b.a + b.b * b.b;
        ctx.sum_src.L += b.L;
        ctx.sum_src.a += b.a;
        ctx.sum_src.b += b.b;
    }
    return ctx;
}

// Closed-form 2-colour blur score with an explicit per-pixel bitmap
// (B[p]=1 ⇔ pixel p is coloured cj). Used by refine_charset where the
// pattern is given (not derived via per-pixel nearest-colour). Same
// closed-form as score_cell_blur_2color, just skipping the bitmap build.
inline float score_cell_blur_2color_bitmap(
    std::uint64_t B,
    std::span<const color_space::OKLab> blurred_src,
    const std::vector<std::array<Tap, 9>>& kernel_taps,
    std::span<const color_space::OKLab> pal_lab,
    const ClosedCtx& ctx,
    std::uint8_t ci, std::uint8_t cj) {
    auto n_pix = blurred_src.size();
    auto& pc0 = pal_lab[ci];
    auto& pc1 = pal_lab[cj];
    float uL = pc1.L - pc0.L, ua = pc1.a - pc0.a, ub = pc1.b - pc0.b;
    float u_norm = uL * uL + ua * ua + ub * ub;

    float sum_m2 = 0, sum_m = 0;
    float sum_bm_L = 0, sum_bm_a = 0, sum_bm_b = 0;
    for (std::size_t q = 0; q < n_pix; ++q) {
        float m = 0;
        for (auto& tap : kernel_taps[q]) {
            if ((B >> tap.q) & 1ULL) m += tap.w;
        }
        sum_m2 += m * m;
        sum_m += m;
        sum_bm_L += blurred_src[q].L * m;
        sum_bm_a += blurred_src[q].a * m;
        sum_bm_b += blurred_src[q].b * m;
    }

    float uDm = uL * (sum_bm_L - pc0.L * sum_m)
              + ua * (sum_bm_a - pc0.a * sum_m)
              + ub * (sum_bm_b - pc0.b * sum_m);

    float pc0_norm = pc0.L * pc0.L + pc0.a * pc0.a + pc0.b * pc0.b;
    float sum_DD = ctx.sum_norm_src
                 - 2.0f * (pc0.L * ctx.sum_src.L + pc0.a * ctx.sum_src.a +
                            pc0.b * ctx.sum_src.b)
                 + static_cast<float>(n_pix) * pc0_norm;

    return u_norm * sum_m2 - 2.0f * uDm + sum_DD;
}

// Closed-form 2-colour (hires / charset-hi) blur score.
//
// Rendered cell (per-pixel-nearest of {ci, cj}):
//   R[p] = pal[ci] + B[p] · (pal[cj] − pal[ci]),  B[p] ∈ {0,1}
// Blur is linear, so
//   Blur(R)_q = pal[ci] + u · m(q),  u = pal[cj] − pal[ci]
//   m(q) = Σ_k w_k · B[taps[q].k]
// MSE = ||u||² · Σ m² − 2 u·ΣDm + Σ||D||²,  D(q) = blurred_src[q] − pal[ci]
//   ΣDm  = Σ blurred_src·m − pc0·Σm
//   Σ||D||² = sum_norm_src − 2 pc0·sum_src + n·||pc0||²
inline float score_cell_blur_2color(
    const PixelDistLut& pixel_dist,
    std::span<const color_space::OKLab> blurred_src,
    const std::vector<std::array<Tap, 9>>& kernel_taps,
    std::span<const color_space::OKLab> pal_lab,
    const ClosedCtx& ctx,
    std::uint8_t ci, std::uint8_t cj) {
    auto n_pix = pixel_dist.size();
    auto& pc0 = pal_lab[ci];
    auto& pc1 = pal_lab[cj];
    float uL = pc1.L - pc0.L, ua = pc1.a - pc0.a, ub = pc1.b - pc0.b;
    float u_norm = uL * uL + ua * ua + ub * ub;

    std::uint64_t B = 0;
    for (std::size_t p = 0; p < n_pix; ++p) {
        if (pixel_dist[p][cj] < pixel_dist[p][ci])
            B |= std::uint64_t{1} << p;
    }

    float sum_m2 = 0, sum_m = 0;
    float sum_bm_L = 0, sum_bm_a = 0, sum_bm_b = 0;
    for (std::size_t q = 0; q < n_pix; ++q) {
        float m = 0;
        for (auto& tap : kernel_taps[q]) {
            if ((B >> tap.q) & 1ULL) m += tap.w;
        }
        sum_m2 += m * m;
        sum_m += m;
        sum_bm_L += blurred_src[q].L * m;
        sum_bm_a += blurred_src[q].a * m;
        sum_bm_b += blurred_src[q].b * m;
    }

    float uDm = uL * (sum_bm_L - pc0.L * sum_m)
              + ua * (sum_bm_a - pc0.a * sum_m)
              + ub * (sum_bm_b - pc0.b * sum_m);

    float pc0_norm = pc0.L * pc0.L + pc0.a * pc0.a + pc0.b * pc0.b;
    float sum_DD = ctx.sum_norm_src
                 - 2.0f * (pc0.L * ctx.sum_src.L + pc0.a * ctx.sum_src.a +
                            pc0.b * ctx.sum_src.b)
                 + static_cast<float>(n_pix) * pc0_norm;

    return u_norm * sum_m2 - 2.0f * uDm + sum_DD;
}

// Generic k-colour fused blur score. Per-pixel-nearest uses the LUT to
// skip OKLab sq-dist recomputes; stores the chosen palette OKLab
// directly in scratch so the blur loop does a single contiguous gather
// rather than an index+OKLab indirection.
inline float score_cell_blur_fused(
    const PixelDistLut& pixel_dist,
    std::span<const color_space::OKLab> blurred_src,
    const std::vector<std::array<Tap, 9>>& kernel_taps,
    std::span<const color_space::OKLab> pal_lab,
    std::span<const std::uint8_t> colors,
    std::vector<color_space::OKLab>& render_scratch) {
    auto n_pix = pixel_dist.size();
    render_scratch.resize(n_pix);
    for (std::size_t p = 0; p < n_pix; ++p) {
        auto& d = pixel_dist[p];
        float bd = d[colors[0]];
        std::uint8_t bi = colors[0];
        for (std::size_t c = 1; c < colors.size(); ++c) {
            float dc = d[colors[c]];
            if (dc < bd) { bd = dc; bi = colors[c]; }
        }
        render_scratch[p] = pal_lab[bi];
    }
    float err = 0;
    for (std::size_t q = 0; q < n_pix; ++q) {
        color_space::OKLab br{0, 0, 0};
        for (auto& tap : kernel_taps[q]) {
            auto& v = render_scratch[tap.q];
            br.L += tap.w * v.L;
            br.a += tap.w * v.a;
            br.b += tap.w * v.b;
        }
        float dL = blurred_src[q].L - br.L;
        float da = blurred_src[q].a - br.a;
        float db = blurred_src[q].b - br.b;
        err += dL * dL + da * da + db * db;
    }
    return err;
}

} // namespace png2c64::quantize::blur_util
