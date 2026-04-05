#pragma once

#include "quantize.hpp"
#include "types.hpp"
#include "vic2.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace png2c64::prg {

struct PrgData {
    std::vector<std::uint8_t> bytes; // Complete PRG file (load address + data)
};

// Generate a Koala-format PRG from a multicolor ScreenResult.
// Layout: $2000 bitmap (8000), $3F40 screen (1000), $4328 d800 (1000), $4710 bg
Result<PrgData> koala(const quantize::ScreenResult& screen);

// Generate a hires bitmap PRG from a hires ScreenResult.
// Layout: $2000 bitmap (8000), $3F40 screen (1000)
Result<PrgData> hires_bitmap(const quantize::ScreenResult& screen);

// Auto-detect mode and generate the appropriate PRG.
Result<PrgData> from_screen(const quantize::ScreenResult& screen);

// Generate raw Koala Paint format (.koa): $6000 + bitmap + screen + d800 + bg
Result<PrgData> koala_raw(const quantize::ScreenResult& screen);

// Generate raw Art Studio format (.hir): $2000 + bitmap + screen
Result<PrgData> hires_raw(const quantize::ScreenResult& screen);

// Write PRG data to a file.
Result<void> write(std::string_view path, const PrgData& prg);

} // namespace png2c64::prg
