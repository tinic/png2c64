# png2c64

PNG/JPEG to Commodore 64 VIC-II image converter written in C++24.

## Build

```bash
cmake -B build -DCMAKE_C_COMPILER=gcc-15 -DCMAKE_CXX_COMPILER=g++-15 .
cmake --build build
ctest --test-dir build --output-on-failure
```

- Requires GCC 15 (`g++-15`, installed via Homebrew at `/opt/homebrew/bin/g++-15`)
- Uses `-std=c++2c` (C++26 draft), strict warnings-as-errors
- Warning flags in `cmake/CompilerWarnings.cmake`
- stb_image/stb_image_write vendored in `third_party/`, compiled as C (`stb_impl.c`) with `SYSTEM` include to suppress warnings
- The local clang LSP does NOT understand C++26 features — its diagnostics about `std::expected`, `std::print`, multidimensional `operator[]`, etc. are false positives. Always verify with the actual GCC 15 build.

## Project Structure

```
src/
  main.cpp          CLI, pipeline orchestration, iTerm2 display, gallery modes
  types.hpp         Color3f, Image (multidim operator[]), Palette, Result<T>, concepts
  color_space.hpp   sRGB<->linear, linear<->OKLab (all constexpr, compile-time LUT)
  vic2.hpp          Mode enum, ModeParams, sprite/charset helpers, is_double_wide()
  palette.hpp       Pepto (default), VICE, Colodore VIC-II palettes (constexpr)
  png_io.hpp/.cpp   Load/save/encode PNG via stb_image
  scale.hpp/.cpp    Separable bicubic (Mitchell-Netravali / Catmull-Rom)
  preprocess.hpp/.cpp  Brightness/contrast/saturation/gamma + OKLab palette range matching
  quantize.hpp/.cpp Brute force per-cell quantization, multithreaded (std::jthread + atomic counter)
  dither.hpp/.cpp   Ordered (Bayer, checker, h2x4, clustered) + error diffusion (FS, Atkinson, Sierra, fs-wide, Jarvis), all in OKLab space
  charset.hpp/.cpp  256-char charset conversion: dedup, Hamming-distance merge, C header export
```

## VIC-II Modes Supported

| `--mode`       | Resolution  | Cell    | Colors/cell | Background |
|----------------|-------------|---------|-------------|------------|
| `hires`        | 320x200     | 8x8     | 2 (any pair) | N/A |
| `multicolor`   | 160x200     | 4x8     | 4 (1 shared bg + 3/cell) | Brute force all 16 |
| `sprite-hi`    | 24xN x 21xM| 24x21   | 2 | N/A |
| `sprite-mc`    | 12xN x 21xM| 12x21   | 4 (1 shared bg + 3/cell) | Most common pixel |
| `charset-hi`   | any (w%8=0) | 8x8     | 2 (1 shared bg + 1/cell fg) | Brute force all 16 |
| `charset-mc`   | any (w%4=0) | 4x8     | 4 (3 shared + 1/cell) | Most common of winning triple |

Multicolor modes use 2:1 pixel ratio (double-wide display). `render_screen` uses `vic2::is_double_wide()` for pixel stretching.

## Architecture Notes

- **Error handling**: `Result<T> = std::expected<T, Error>` throughout, no exceptions
- **Color math**: All perceptual operations (dithering, quantization, palette matching) use OKLab color space
- **Preprocessing pipeline**: brightness/contrast/saturation/gamma (linear space) -> `match_palette_range()` (remaps image OKLab extent to palette OKLab extent using percentiles)
- **Quantization**: Brute force over all valid color combinations per cell. Multicolor screen mode tries all 16 backgrounds sequentially (cells parallelized within each). Hires parallelizes directly over cells.
- **Dithering**: Error diffusion operates entirely in OKLab space (error buffer, accumulation, and distribution all OKLab). Serpentine scanning + error clamping. `dither::apply()` receives `ModeParams` explicitly (critical for sprite modes where `get_mode_params(mode)` returns single-sprite dims, not sheet dims).
- **Charset mode**: Quantizes all cells, deduplicates identical 8-byte patterns, merges closest pairs (Hamming distance) if > 256 unique. Height padded to multiple of 8. Outputs C header with charset[2048], screen_map[], color_ram[], and #defines for shared colors.
- **Threading**: `std::jthread` workers with `std::atomic<size_t>` work counter for lock-free cell distribution. `std::atomic<float>` error accumulation via CAS loop.
- **Terminal preview**: iTerm2 inline image protocol (`ESC]1337;File=...`), 3x display scale, in-memory PNG encoding via `stbi_write_png_to_func`.

## Gallery Mode

`--gallery <param>` previews parameter variations in terminal:
- `dither` — all 12 dither methods (fast, reuses quantization)
- `brightness`, `contrast`, `saturation`, `gamma`, `error-clamp`, `dither-strength` — re-runs full pipeline per value

## Key Design Decisions

- `vic2::cells_x()` and `cells_y()` take `ModeParams&` (not `Mode`) — required for sprite/charset modes where screen dims are runtime
- Default dither: `checker` (2:1 aware). Default palette: `pepto`.
- Charset multicolor: `convert()` expects logical resolution (caller scales width/2 before calling)
- `CMakeLists.txt` has `add_example()` function for test images with per-image `GAMMA` and `DITHER_STRENGTH` params
- Example images in `examples/` (JPEG inputs, `-c64.png` outputs)
