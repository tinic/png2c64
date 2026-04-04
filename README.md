# png2c64

Convert images to Commodore 64 VIC-II formats with perceptual color matching, dithering, and brute-force quantization.

Built for C64 demo scene production. All color operations use OKLab perceptual color space. Multithreaded.

## Examples

| | | |
|---|---|---|
| ![alien](examples/alien-c64.png) | ![dog](examples/dog-c64.png) | ![dragon](examples/dragon-c64.png) |
| ![face](examples/face-c64.png) | ![fantasy](examples/fantasy-c64.png) | ![golden3](examples/golden3-c64.png) |
| ![monster](examples/monster-c64.png) | ![ship](examples/ship-c64.png) | |

## Features

- **Bitmap modes** -- hires (320x200, 2 colors/cell) and multicolor (160x200, 4 colors/cell)
- **Sprite sheets** -- hires and multicolor, arbitrary grid dimensions
- **Character sets** -- 256-char charset generation with dedup, pattern merging, k-means refinement, and C header export
- **7 VIC-II palettes** -- Pepto, VICE, Colodore, Deekay, Godot, C64 Wiki, Levy
- **12 dither modes** -- ordered (Bayer, checker, clustered dot) and error diffusion (Floyd-Steinberg, Atkinson, Jarvis), including 2:1 pixel-ratio variants for multicolor
- **Perceptual preprocessing** -- OKLab-space palette range matching, plus brightness/contrast/saturation/gamma
- **Gallery mode** -- preview all dither methods or parameter sweeps inline in the terminal (iTerm2)
- **C header export** -- charset data, screen map, and color RAM as includable arrays

## Requirements

- GCC 15+ (uses C++26 / `-std=c++2c`)
- CMake 3.28+
- macOS or Linux

## Build

```bash
cmake -B build -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 .
cmake --build build
```

Run tests:
```bash
ctest --test-dir build --output-on-failure
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

### Gallery mode
Preview parameter variations inline in iTerm2 (or compatible terminals):
```bash
# Compare all 12 dither methods
png2c64 --gallery dither input.jpg

# Sweep gamma values
png2c64 --gallery gamma input.jpg

# Available: dither, brightness, contrast, saturation, gamma,
#            error-clamp, dither-strength
```

Gallery works with all modes including charset.

## Options

```
--mode <mode>              hires, multicolor, sprite-hi, sprite-mc,
                           charset-hi, charset-mc  (default: multicolor)
--palette <name>           pepto, vice, colodore, deekay, godot,
                           c64wiki, levy  (default: pepto)
--dither <method>          Dithering method (default: checker)
  Square-pixel:            none, bayer4, bayer8, fs, atkinson, sierra
  2:1 multicolor:          checker, bayer2x2, h2x4, clustered, fs-wide, jarvis
--dither-strength <float>  0.0-2.0 (default: 1.0)
--error-clamp <float>      Max error accumulation 0.1-2.0 (default: 0.8)
--no-serpentine            Disable bidirectional scanning
--brightness <float>       -1.0 to 1.0 (default: 0.0)
--contrast <float>         0.0 to 2.0 (default: 1.0)
--saturation <float>       0.0 to 2.0 (default: 1.0)
--gamma <float>            0.1 to 8.0 (default: 1.0)
--width <int>              Override target width
--height <int>             Override target height
--sprites-x <int>          Sprite sheet columns (default: 1)
--sprites-y <int>          Sprite sheet rows (default: 1)
--gallery <param>          Preview parameter variations in terminal
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
  dither.hpp/.cpp    12 dither algorithms (OKLab)
  charset.hpp/.cpp   Charset conversion + C header export
third_party/
  stb_image.h, stb_image_write.h
```

## License

MIT
