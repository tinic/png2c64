#include "preprocess.hpp"
#include "color_space.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace png2c64::preprocess {

void apply(Image& image, const Settings& s) {
    for (auto& pixel : image.pixels()) {
        // 1. Gamma (in linear space, before anything else)
        if (s.gamma != 1.0f) {
            pixel.r = std::pow(std::max(pixel.r, 0.0f), s.gamma);
            pixel.g = std::pow(std::max(pixel.g, 0.0f), s.gamma);
            pixel.b = std::pow(std::max(pixel.b, 0.0f), s.gamma);
        }

        // 2-4. Brightness, contrast, saturation in OKLab space
        // where L is perceptually uniform and 0.5 is actual mid-grey.
        auto lab = color_space::linear_to_oklab(pixel);

        // Brightness: additive shift on L
        lab.L += s.brightness;

        // Contrast: scale around L=0.5 (perceptual mid-grey)
        lab.L = (lab.L - 0.5f) * s.contrast + 0.5f;

        // Saturation: scale chroma (a, b) around zero
        lab.a *= s.saturation;
        lab.b *= s.saturation;

        pixel = color_space::oklab_to_linear(lab).clamped();
    }
}

void match_palette_range(Image& image, const Palette& palette,
                         float percentile, float margin) {
    auto pixel_count = image.width() * image.height();
    if (pixel_count == 0 || palette.colors.empty()) return;

    // 1. Compute OKLab extent of the palette
    float pal_L_min = 1e9f, pal_L_max = -1e9f;
    float pal_a_min = 1e9f, pal_a_max = -1e9f;
    float pal_b_min = 1e9f, pal_b_max = -1e9f;

    for (auto& c : palette.colors) {
        auto lab = color_space::linear_to_oklab(c);
        pal_L_min = std::min(pal_L_min, lab.L);
        pal_L_max = std::max(pal_L_max, lab.L);
        pal_a_min = std::min(pal_a_min, lab.a);
        pal_a_max = std::max(pal_a_max, lab.a);
        pal_b_min = std::min(pal_b_min, lab.b);
        pal_b_max = std::max(pal_b_max, lab.b);
    }

    // Apply margin: shrink target range inward
    auto apply_margin = [margin](float lo, float hi) {
        float span = hi - lo;
        return std::pair{lo + span * margin, hi - span * margin};
    };

    auto [tgt_L_min, tgt_L_max] = apply_margin(pal_L_min, pal_L_max);
    auto [tgt_a_min, tgt_a_max] = apply_margin(pal_a_min, pal_a_max);
    auto [tgt_b_min, tgt_b_max] = apply_margin(pal_b_min, pal_b_max);

    // 2. Convert all image pixels to OKLab and collect per-channel values
    std::vector<color_space::OKLab> image_lab(pixel_count);
    std::vector<float> Ls(pixel_count), As(pixel_count), Bs(pixel_count);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto y = i / image.width();
        auto x = i % image.width();
        image_lab[i] = color_space::linear_to_oklab(image[x, y]);
        Ls[i] = image_lab[i].L;
        As[i] = image_lab[i].a;
        Bs[i] = image_lab[i].b;
    }

    // 3. Find image extent using percentiles (robust to outliers)
    auto percentile_range = [percentile](std::vector<float>& vals) {
        std::ranges::sort(vals);
        auto n = vals.size();
        auto lo_idx = static_cast<std::size_t>(
            static_cast<float>(n) * percentile);
        auto hi_idx = static_cast<std::size_t>(
            static_cast<float>(n) * (1.0f - percentile));
        lo_idx = std::min(lo_idx, n - 1);
        hi_idx = std::min(hi_idx, n - 1);
        return std::pair{vals[lo_idx], vals[hi_idx]};
    };

    auto [src_L_min, src_L_max] = percentile_range(Ls);
    auto [src_a_min, src_a_max] = percentile_range(As);
    auto [src_b_min, src_b_max] = percentile_range(Bs);

    // 4. Remap each pixel: src range -> target range per channel
    auto remap = [](float val, float src_lo, float src_hi,
                    float dst_lo, float dst_hi) -> float {
        float src_span = src_hi - src_lo;
        if (src_span < 1e-6f) {
            return (dst_lo + dst_hi) * 0.5f;
        }
        float t = (val - src_lo) / src_span;
        return dst_lo + t * (dst_hi - dst_lo);
    };

    for (std::size_t i = 0; i < pixel_count; ++i) {
        auto& lab = image_lab[i];
        lab.L = remap(lab.L, src_L_min, src_L_max, tgt_L_min, tgt_L_max);
        lab.a = remap(lab.a, src_a_min, src_a_max, tgt_a_min, tgt_a_max);
        lab.b = remap(lab.b, src_b_min, src_b_max, tgt_b_min, tgt_b_max);

        // Convert back to linear RGB and store
        auto y = i / image.width();
        auto x = i % image.width();
        image[x, y] = color_space::oklab_to_linear(lab).clamped();
    }
}

} // namespace png2c64::preprocess
