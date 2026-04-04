#pragma once

#include "types.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

namespace png2c64::png_io {

Result<Image> load(std::string_view path);
Result<void> save(std::string_view path, const Image& image);

// Encode image to in-memory PNG bytes (for iTerm2 inline display etc.)
Result<std::vector<std::uint8_t>> encode(const Image& image);

} // namespace png2c64::png_io
