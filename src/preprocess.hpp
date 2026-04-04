#pragma once

#include "types.hpp"

namespace png2c64::preprocess {

struct Settings {
    float brightness = 0.0f;  // [-1, 1] additive
    float contrast   = 1.0f;  // [0, 2] multiplicative around 0.5
    float saturation = 1.0f;  // [0, 2] lerp toward luminance
    float gamma      = 1.0f;  // [0.1, 4.0] power curve
};

void apply(Image& image, const Settings& settings);

// Remap the image's OKLab extent to fit the palette's OKLab extent.
// percentile: 0.0-0.5, fraction of pixels to ignore at each tail (robustness).
//             e.g. 0.01 = use 1st-99th percentile of image range.
// margin:     0.0-1.0, shrink the target range inward by this fraction
//             (avoids pushing pixels to the palette extremes).
void match_palette_range(Image& image, const Palette& palette,
                         float percentile = 0.01f, float margin = 0.05f);

} // namespace png2c64::preprocess
