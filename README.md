# png2c64

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Web App](https://img.shields.io/badge/Try_it-png2c64.app-brightgreen)](https://png2c64.app)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)](https://en.cppreference.com/w/cpp/26)

Convert images to Commodore 64 VIC-II formats.

Color operations in [OKLab](https://bottosson.github.io/posts/oklab/). Native CLI (multithreaded) and in-browser version at **[png2c64.app](https://png2c64.app)** (WASM).

## Examples

| | | |
|---|---|---|
| ![alien](examples/alien-c64.png) | ![dog](examples/dog-c64.png) | ![dragon](examples/dragon-c64.png) |
| ![face](examples/face-c64.png) | ![fantasy](examples/fantasy-c64.png) | ![golden3](examples/golden3-c64.png) |
| ![monster](examples/monster-c64.png) | ![ship](examples/ship-c64.png) | ![game](examples/game-c64.png) |

### PETSCII

| | |
|---|---|
| ![head input](examples/head.png) | ![head petscii](examples/head-petscii.png) |
| Input | PETSCII (40x25 text mode) |

## Dither Modes

All 30 modes shown with `--gamma 1 --dither-strength 0.7 --error-clamp 0.2`. Click to expand:

<details open><summary><b>none</b> — no dithering</summary>
<img src="examples/dither_gallery/none.png" width="640">
</details>

<details><summary><b>bayer4</b> — 4x4 ordered</summary>
<img src="examples/dither_gallery/bayer4.png" width="640">
</details>

<details><summary><b>bayer8</b> — 8x8 ordered</summary>
<img src="examples/dither_gallery/bayer8.png" width="640">
</details>

<details><summary><b>checker</b> — 2:1 checkerboard</summary>
<img src="examples/dither_gallery/checker.png" width="640">
</details>

<details><summary><b>bayer2x2</b> — 2:1 minimal ordered</summary>
<img src="examples/dither_gallery/bayer2x2.png" width="640">
</details>

<details><summary><b>h2x4</b> — 2:1 horizontal-biased</summary>
<img src="examples/dither_gallery/h2x4.png" width="640">
</details>

<details><summary><b>clustered</b> — 2:1 clustered dot</summary>
<img src="examples/dither_gallery/clustered.png" width="640">
</details>

<details><summary><b>line2</b> — horizontal 2-level</summary>
<img src="examples/dither_gallery/line2.png" width="640">
</details>

<details><summary><b>line-checker</b> — line-biased checker</summary>
<img src="examples/dither_gallery/line-checker.png" width="640">
</details>

<details><summary><b>line4</b> — horizontal 4-level</summary>
<img src="examples/dither_gallery/line4.png" width="640">
</details>

<details><summary><b>line8</b> — horizontal 8-level</summary>
<img src="examples/dither_gallery/line8.png" width="640">
</details>

<details><summary><b>fs</b> — Floyd-Steinberg</summary>
<img src="examples/dither_gallery/fs.png" width="640">
</details>

<details><summary><b>atkinson</b> — Atkinson (75% error)</summary>
<img src="examples/dither_gallery/atkinson.png" width="640">
</details>

<details><summary><b>sierra</b> — Sierra Lite</summary>
<img src="examples/dither_gallery/sierra.png" width="640">
</details>

<details><summary><b>fs-wide</b> — 2:1 Floyd-Steinberg</summary>
<img src="examples/dither_gallery/fs-wide.png" width="640">
</details>

<details><summary><b>jarvis</b> — Jarvis-Judice-Ninke</summary>
<img src="examples/dither_gallery/jarvis.png" width="640">
</details>

<details><summary><b>line-fs</b> — vertical error diffusion</summary>
<img src="examples/dither_gallery/line-fs.png" width="640">
</details>

<details><summary><b>ostromoukhov</b> — variable-coefficient error diffusion</summary>
<img src="examples/dither_gallery/ostromoukhov.png" width="640">
</details>

<details><summary><b>halftone8</b> — 8×8 45° clustered dot halftone</summary>
<img src="examples/dither_gallery/halftone8.png" width="640">
</details>

<details><summary><b>diagonal8</b> — 8×8 diagonal clustered dot</summary>
<img src="examples/dither_gallery/diagonal8.png" width="640">
</details>

<details><summary><b>spiral5</b> — 5×5 spiral clustered dot</summary>
<img src="examples/dither_gallery/spiral5.png" width="640">
</details>

<details><summary><b>hex8</b> — 8×8 hexagonal tiling</summary>
<img src="examples/dither_gallery/hex8.png" width="640">
</details>

<details><summary><b>hex5</b> — 5×5 hexagonal tiling</summary>
<img src="examples/dither_gallery/hex5.png" width="640">
</details>

<details><summary><b>blue-noise</b> — 64×64 precomputed blue noise</summary>
<img src="examples/dither_gallery/blue-noise.png" width="640">
</details>

<details><summary><b>ign</b> — Interleaved Gradient Noise (analytical)</summary>
<img src="examples/dither_gallery/ign.png" width="640">
</details>

<details><summary><b>r2</b> — Martin Roberts R2 low-discrepancy sequence</summary>
<img src="examples/dither_gallery/r2.png" width="640">
</details>

<details><summary><b>white-noise</b> — per-pixel random hash</summary>
<img src="examples/dither_gallery/white-noise.png" width="640">
</details>

<details><summary><b>crosshatch</b> — pen-and-ink crosshatching</summary>
<img src="examples/dither_gallery/crosshatch.png" width="640">
</details>

<details><summary><b>radial</b> — concentric circles</summary>
<img src="examples/dither_gallery/radial.png" width="640">
</details>

<details><summary><b>value-noise</b> — coherent organic noise</summary>
<img src="examples/dither_gallery/value-noise.png" width="640">
</details>

## Features

- **Bitmap modes**: hires (320×200, 2 colors/cell), multicolor (160×200, 4 colors/cell)
- **FLI / AFLI**: per-row color attributes, PRG export with displayer
- **PETSCII**: image approximation using the C64 ROM character set (brute-force glyph + fg/bg per cell)
- **Sprite sheets**: hires / multicolor, arbitrary grid dimensions
- **Character sets**: hires, multicolor, and mixed; dedup, pattern merging, k-means refinement, C header export
- **7 VIC-II palettes**: Pepto, VICE, Colodore, Deekay, Godot, C64 Wiki, Levy
- **30 dither methods**: ordered (Bayer, checker, clustered, line), halftone/non-rectangular (halftone8×8, spiral5×5, hex8×8), analytical (IGN, R2, blue noise, crosshatch, radial, value noise), error diffusion (Floyd-Steinberg, Atkinson, Sierra, Jarvis, Ostromoukhov), 2:1 and line-biased variants
- **Preprocessing**: OKLab brightness/contrast/saturation/gamma/hue, sharpen/blur, black/white point clipping, automatic palette-range matching
- **Per-cell metric**: MSE (default), Pappas-Neuhoff blur, or Wang SSIM — useful for PETSCII and charset modes
- **Interactive mode** (iTerm2, POSIX): live parameter tuning with inline preview
- **Gallery mode** (iTerm2): inline previews of dither or parameter sweeps
- **Export**: PNG preview, PRG with embedded displayer (bitmap / FLI / AFLI / PETSCII), Koala and Art Studio raw, C header (charset / sprite / PETSCII data)

## Requirements

- GCC 15+ (uses C++26 / `-std=c++2c`)
- CMake 3.28+
- macOS, Linux, or Windows (MSYS2 UCRT64)

Prebuilt Linux / macOS / Windows binaries are attached to each [GitHub release](https://github.com/tinic/png2c64/releases).

## Build

```bash
cmake -B build -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 .
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows, the same commands work under MSYS2 UCRT64 (`mingw-w64-ucrt-x86_64-gcc`, `mingw-w64-ucrt-x86_64-cmake`).

Check the build number:
```bash
./build/png2c64 --version
```

## Usage

### Multicolor bitmap (default)
```bash
png2c64 input.jpg output.png
png2c64 --gamma 2.0 --dither jarvis input.jpg output.png
```

### Hires bitmap
```bash
png2c64 --mode hires input.jpg output.png
```

### Sprite sheet
```bash
# 6x2 multicolor sprite sheet
png2c64 --mode sprite-mc --sprites-x 6 --sprites-y 2 input.png output.png

# Single hires sprite
png2c64 --mode sprite-hi input.png output.png
```

### FLI / AFLI
```bash
png2c64 --mode fli input.jpg output.png    # multicolor FLI, per-row colors
png2c64 --mode afli input.jpg output.png   # hires FLI, per-row colors
```

### PETSCII
```bash
# Uses C64 ROM charset to approximate the image in text mode
png2c64 --mode petscii --gamma 3.3 --dither-strength 0.7 input.jpg output.png
```

### Character set
```bash
# Multicolor charset from full-screen image -> C header
png2c64 --mode charset-mc --width 320 --height 200 --dither checker input.jpg output.h

# Hires charset, no dithering (full k-means optimization)
png2c64 --mode charset-hi --width 320 --height 200 --dither none input.jpg output.h
```

The charset output is a C header file containing:
```c
#define name_COLS 40
#define name_ROWS 25
#define name_BACKGROUND 0
#define name_MULTICOLOR1 6
#define name_MULTICOLOR2 14

static const unsigned char name_charset[2048] = { ... };
static const unsigned char name_screen[1000] = { ... };
static const unsigned char name_color[1000] = { ... };
```

### Interactive mode
Live parameter tuning with instant preview in iTerm2:
```bash
png2c64 --interactive input.jpg output.png
png2c64 --mode charset-mc --width 320 --height 200 --interactive input.jpg output.h
```

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `p`/`P` | palette next/prev | `d`/`D` | dither next/prev |
| `g`/`G` | gamma +/- | `s`/`S` | strength +/- |
| `b`/`B` | brightness +/- | `c`/`C` | contrast +/- |
| `t`/`T` | saturation +/- | `e`/`E` | error clamp +/- |
| `x` | serpentine toggle | `r` | reset all |
| `w` | save output | `q` | quit |

### Gallery mode
Preview parameter variations inline in iTerm2:
```bash
png2c64 --gallery dither input.jpg
png2c64 --gallery gamma input.jpg

# Available: dither, brightness, contrast, saturation, gamma,
#            error-clamp, dither-strength, adaptive
```

Gallery and interactive work with all modes including charset.

## Options

```
--mode <mode>              hires, multicolor, fli, afli, petscii,
                           sprite-hi, sprite-mc,
                           charset-hi, charset-mc, charset-mixed
                           (default: multicolor)
--palette <name>           pepto, vice, colodore, deekay, godot,
                           c64wiki, levy (default: colodore)
--dither <method>          Dithering method (default: checker)
  Square-pixel ordered:    none, bayer4, bayer8
  2:1 multicolor ordered:  checker, bayer2x2, h2x4, clustered
  Horizontal lines:        line2, line-checker, line4, line8
  Halftone / analytical:   halftone8, diagonal8, spiral5, hex8, hex5,
                           blue-noise, ign, r2, white-noise,
                           crosshatch, radial, value-noise
  Error diffusion:         fs, atkinson, sierra, fs-wide, jarvis,
                           line-fs, ostromoukhov
--dither-strength <float>  0.0–2.0 (default: 0.7)
--error-clamp <float>      Max error per OKLab channel (default: 0.1)
--adaptive <float>         Contrast-adaptive error diffusion 0.0–1.0
                           (default: 0.0; reduces dither in detail,
                           keeps it in flat areas)
--no-serpentine            Disable serpentine error-diffusion scan
--match-range              OKLab extent → palette extent remapping
--brightness <float>       –1.0 to 1.0 (default: 0.0)
--contrast <float>         0.0–2.0 (default: 1.0)
--saturation <float>       0.0–2.0 (default: 1.0)
--gamma <float>            0.1–8.0 (default: 1.5)
--hue-shift <float>        –180 to 180 degrees (default: 0)
--sharpen <float>          –1 to 2 (negative blurs, positive sharpens)
--black-point <float>      0.0–0.4 (clip darkest fraction)
--white-point <float>      0.0–0.4 (clip brightest fraction)
--metric mse|blur|ssim     Per-cell error metric. MSE is the default
                           and pairs with dither; blur / ssim score
                           against the continuous source (useful for
                           PETSCII and charset modes).
--graphics-only            PETSCII: restrict to graphics glyphs (no text)
--width <int>              Override output width
--height <int>             Override output height
--sprites-x <int>          Sprite sheet columns (default: 1)
--sprites-y <int>          Sprite sheet rows (default: 1)
--gallery <param>          dither | brightness | contrast | saturation |
                           gamma | error-clamp | dither-strength |
                           adaptive
--interactive              Live parameter tuning (iTerm2, POSIX only)
--version                  Print version and exit
```

## How it works

### Pipeline

```
Load image (PNG/JPEG/BMP/TGA via stb_image)
  -> Bicubic scale (separable Mitchell-Netravali)
  -> Preprocess (gamma, brightness, contrast, saturation)
  -> Match palette range (remap OKLab extent to target palette)
  -> Brute-force quantize (per-cell, all valid color combinations)
  -> Dither (error diffusion in OKLab space)
  -> Output (PNG + iTerm2 inline preview)
```

### Quantization

Each cell is quantized by exhaustively testing all valid color combinations:

| Mode | Combinations per cell | Total for screen |
|------|----------------------|-----------------|
| Hires | C(16,2) = 120 pairs | ~15M evaluations |
| Multicolor | 16 backgrounds x C(15,3) = 455 triples | ~931M evaluations |
| FLI | 16 bg x 15 colorram x C(14,2) x 8 rows | ~44K/cell |
| PETSCII | 16 bg x 256 chars x 15 fg | ~61M evaluations |
| Charset MC | C(16,3) = 560 shared triples x 13 per-cell | ~300M evaluations |

All distance computations use squared OKLab perceptual distance. Cell processing is parallelized across all CPU cores via `std::jthread`.

### Charset mode

1. **Color selection** -- brute-force optimal shared colors
2. **Dithering** -- error diffusion within cell color constraints
3. **Pattern encoding** -- 8-byte binary patterns from dithered assignments
4. **Deduplication** -- identical patterns share charset entries
5. **Merging** -- if > 256 unique: precompute pairwise OKLab distances, sort, merge closest pairs
6. **K-means refinement** -- iteratively reassign cells to best-matching patterns. With dithering: preserves dither patterns (assignments only). Without: full centroid recomputation.

### Dithering

Error diffusion operates entirely in OKLab space -- the error buffer, accumulation, and distribution all use OKLab values. This prevents the visible cell-boundary artifacts that occur when mixing RGB error propagation with OKLab color matching.

The 2:1 dither modes account for the double-wide multicolor pixel:
- **h2x4** -- 2x4 Bayer matrix that tiles as a perceptually square block at 2:1 display
- **clustered** -- dot pattern shaped for round appearance at 2:1
- **fs-wide** -- Floyd-Steinberg with vertical-biased weights
- **jarvis** -- Jarvis-Judice-Ninke 5x3 kernel, naturally handles 2:1 via wider reach

## Project structure

```
src/
  main.cpp           CLI, pipeline, iTerm2 display, gallery
  types.hpp          Color3f, Image, Palette, Result<T>
  color_space.hpp    sRGB/linear/OKLab (constexpr)
  vic2.hpp           VIC-II modes and constraints
  palette.hpp        7 palettes (constexpr, registry)
  png_io.hpp/.cpp    Load/save/encode via stb_image
  scale.hpp/.cpp     Bicubic scaling
  preprocess.hpp/.cpp  Color adjustments + OKLab range matching
  quantize.hpp/.cpp  Brute-force quantization (multithreaded)
  dither.hpp/.cpp    30 dither algorithms (OKLab)
  charset.hpp/.cpp   Charset conversion + C header export
  petscii_rom.hpp    C64 character ROM (256 chars, precomputed bit tables)
  prg.hpp/.cpp       PRG export with embedded displayers
  displayer_data.hpp Koala/hires/FLI/AFLI/PETSCII displayer binaries
third_party/
  stb_image.h, stb_image_write.h
```

## License

MIT
