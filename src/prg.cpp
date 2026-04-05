#include "prg.hpp"
#include "displayer_data.hpp"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace png2c64::prg {

namespace {

constexpr std::uint16_t displayer_load_addr = 0x0801;

void write_le16(std::vector<std::uint8_t>& out, std::uint16_t val) {
    out.push_back(static_cast<std::uint8_t>(val & 0xFF));
    out.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFF));
}

std::vector<std::uint8_t> encode_bitmap(
    const quantize::ScreenResult& screen,
    const vic2::ModeParams& params) {

    bool multicolor = params.has_shared_background;
    std::vector<std::uint8_t> bitmap(screen.cells.size() * 8);

    for (std::size_t ci = 0; ci < screen.cells.size(); ++ci) {
        auto& cell = screen.cells[ci];
        auto base = ci * 8;
        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t byte = 0;
            if (multicolor) {
                for (std::size_t col = 0; col < 4; ++col) {
                    auto pi = row * 4 + col;
                    byte |= static_cast<std::uint8_t>(
                        cell.pixel_indices[pi] << (6 - col * 2));
                }
            } else {
                for (std::size_t col = 0; col < 8; ++col) {
                    auto pi = row * 8 + col;
                    if (cell.pixel_indices[pi] == 1)
                        byte |= static_cast<std::uint8_t>(0x80 >> col);
                }
            }
            bitmap[base + row] = byte;
        }
    }
    return bitmap;
}

std::vector<std::uint8_t> encode_screen_koala(
    const quantize::ScreenResult& screen) {
    std::vector<std::uint8_t> scr(screen.cells.size());
    for (std::size_t i = 0; i < screen.cells.size(); ++i) {
        auto& cc = screen.cells[i].cell_colors;
        std::uint8_t hi = (cc.size() > 1) ? cc[1] : 0;
        std::uint8_t lo = (cc.size() > 2) ? cc[2] : 0;
        scr[i] = static_cast<std::uint8_t>((hi << 4) | (lo & 0x0F));
    }
    return scr;
}

std::vector<std::uint8_t> encode_d800_koala(
    const quantize::ScreenResult& screen) {
    std::vector<std::uint8_t> d800(screen.cells.size());
    for (std::size_t i = 0; i < screen.cells.size(); ++i) {
        auto& cc = screen.cells[i].cell_colors;
        d800[i] = (cc.size() > 3) ? cc[3] : 0;
    }
    return d800;
}

std::vector<std::uint8_t> encode_screen_hires(
    const quantize::ScreenResult& screen) {
    std::vector<std::uint8_t> scr(screen.cells.size());
    for (std::size_t i = 0; i < screen.cells.size(); ++i) {
        auto& cc = screen.cells[i].cell_colors;
        std::uint8_t fg = (cc.size() > 1) ? cc[1] : 0;
        std::uint8_t bg = (cc.size() > 0) ? cc[0] : 0;
        scr[i] = static_cast<std::uint8_t>((fg << 4) | (bg & 0x0F));
    }
    return scr;
}

// Build a complete PRG: displayer + image data (uncompressed, loads directly)
PrgData build_display_prg(const std::uint8_t* displayer, std::size_t disp_size,
                           const std::vector<std::uint8_t>& image_data,
                           std::uint16_t image_addr) {
    auto disp_data = displayer + 2; // skip 2-byte load address
    auto disp_data_size = disp_size - 2;

    auto disp_end = static_cast<std::uint16_t>(displayer_load_addr +
                                                disp_data_size);

    auto gap = (image_addr > disp_end)
        ? static_cast<std::size_t>(image_addr - disp_end) : 0;

    PrgData prg;
    write_le16(prg.bytes, displayer_load_addr);
    // Displayer code
    prg.bytes.insert(prg.bytes.end(), disp_data, disp_data + disp_data_size);
    // Zero-fill gap to image address
    prg.bytes.resize(prg.bytes.size() + gap, 0);
    // Image data
    prg.bytes.insert(prg.bytes.end(), image_data.begin(), image_data.end());

    return prg;
}

} // namespace

Result<PrgData> koala(const quantize::ScreenResult& screen) {
    if (screen.cells.size() != 1000) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Koala requires 40x25 = 1000 cells",
        }};
    }

    auto params = vic2::get_mode_params(vic2::Mode::multicolor);
    auto bitmap = encode_bitmap(screen, params);
    auto scr = encode_screen_koala(screen);
    auto d800 = encode_d800_koala(screen);

    std::vector<std::uint8_t> image_data;
    image_data.reserve(10001);
    image_data.insert(image_data.end(), bitmap.begin(), bitmap.end());
    image_data.insert(image_data.end(), scr.begin(), scr.end());
    image_data.insert(image_data.end(), d800.begin(), d800.end());
    image_data.push_back(screen.background_color & 0x0F);

    return build_display_prg(detail::koala_displayer,
                              sizeof(detail::koala_displayer),
                              image_data, 0x2000);
}

Result<PrgData> hires_bitmap(const quantize::ScreenResult& screen) {
    if (screen.cells.size() != 1000) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Hires bitmap requires 40x25 = 1000 cells",
        }};
    }

    auto params = vic2::get_mode_params(vic2::Mode::hires);
    auto bitmap = encode_bitmap(screen, params);
    auto scr = encode_screen_hires(screen);

    std::vector<std::uint8_t> image_data;
    image_data.reserve(9000);
    image_data.insert(image_data.end(), bitmap.begin(), bitmap.end());
    image_data.insert(image_data.end(), scr.begin(), scr.end());

    return build_display_prg(detail::hires_displayer,
                              sizeof(detail::hires_displayer),
                              image_data, 0x2000);
}

Result<PrgData> koala_raw(const quantize::ScreenResult& screen) {
    if (screen.cells.size() != 1000) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Koala requires 40x25 = 1000 cells",
        }};
    }

    auto params = vic2::get_mode_params(vic2::Mode::multicolor);
    auto bitmap = encode_bitmap(screen, params);
    auto scr = encode_screen_koala(screen);
    auto d800 = encode_d800_koala(screen);

    PrgData prg;
    prg.bytes.reserve(10003);
    write_le16(prg.bytes, 0x6000); // Koala Paint load address
    prg.bytes.insert(prg.bytes.end(), bitmap.begin(), bitmap.end());
    prg.bytes.insert(prg.bytes.end(), scr.begin(), scr.end());
    prg.bytes.insert(prg.bytes.end(), d800.begin(), d800.end());
    prg.bytes.push_back(screen.background_color & 0x0F);
    return prg;
}

Result<PrgData> hires_raw(const quantize::ScreenResult& screen) {
    if (screen.cells.size() != 1000) {
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "Hires bitmap requires 40x25 = 1000 cells",
        }};
    }

    auto params = vic2::get_mode_params(vic2::Mode::hires);
    auto bitmap = encode_bitmap(screen, params);
    auto scr = encode_screen_hires(screen);

    PrgData prg;
    prg.bytes.reserve(9002);
    write_le16(prg.bytes, 0x2000); // Art Studio load address
    prg.bytes.insert(prg.bytes.end(), bitmap.begin(), bitmap.end());
    prg.bytes.insert(prg.bytes.end(), scr.begin(), scr.end());
    return prg;
}

// ---------------------------------------------------------------------------
// FLI format encoding
// ---------------------------------------------------------------------------

// Encode FLI bitmap — same as multicolor bitmap (4 pixels/row, 2 bits each)
std::vector<std::uint8_t> encode_bitmap_fli(
    const quantize::ScreenResult& screen) {
    std::vector<std::uint8_t> bitmap(screen.cells.size() * 8);
    for (std::size_t ci = 0; ci < screen.cells.size(); ++ci) {
        auto& cell = screen.cells[ci];
        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t byte = 0;
            for (std::size_t col = 0; col < 4; ++col) {
                auto pi = row * 4 + col;
                if (pi < cell.pixel_indices.size())
                    byte |= static_cast<std::uint8_t>(
                        cell.pixel_indices[pi] << (6 - col * 2));
            }
            bitmap[ci * 8 + row] = byte;
        }
    }
    return bitmap;
}

// Encode AFLI bitmap — same as hires bitmap (8 pixels/row, 1 bit each)
std::vector<std::uint8_t> encode_bitmap_afli(
    const quantize::ScreenResult& screen) {
    std::vector<std::uint8_t> bitmap(screen.cells.size() * 8);
    for (std::size_t ci = 0; ci < screen.cells.size(); ++ci) {
        auto& cell = screen.cells[ci];
        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t byte = 0;
            for (std::size_t col = 0; col < 8; ++col) {
                auto pi = row * 8 + col;
                if (pi < cell.pixel_indices.size() &&
                    cell.pixel_indices[pi] == 1)
                    byte |= static_cast<std::uint8_t>(0x80 >> col);
            }
            bitmap[ci * 8 + row] = byte;
        }
    }
    return bitmap;
}

// Encode 8 screen RAMs for FLI mode.
// Screen RAM N contains the colors for row N within each 8-row character.
// Each byte: high nibble = screen_hi (bitpair 01), low nibble = screen_lo (bitpair 10)
// FLI cell_colors = [bg, colorram, r0_hi, r0_lo, r1_hi, r1_lo, ..., r7_hi, r7_lo]
std::array<std::vector<std::uint8_t>, 8> encode_screen_fli(
    const quantize::ScreenResult& screen) {
    auto num_cells = screen.cells.size();
    std::array<std::vector<std::uint8_t>, 8> screens;
    for (auto& s : screens) s.resize(num_cells);

    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cc = screen.cells[ci].cell_colors;
        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t hi = (cc.size() > 2 + row * 2) ? cc[2 + row * 2] : 0;
            std::uint8_t lo = (cc.size() > 3 + row * 2) ? cc[3 + row * 2] : 0;
            screens[row][ci] = static_cast<std::uint8_t>((hi << 4) | (lo & 0x0F));
        }
    }
    return screens;
}

// Encode color RAM for FLI (shared per cell, bitpair 11)
// FLI cell_colors[1] = colorram
std::vector<std::uint8_t> encode_colorram_fli(
    const quantize::ScreenResult& screen) {
    std::vector<std::uint8_t> d800(screen.cells.size());
    for (std::size_t i = 0; i < screen.cells.size(); ++i) {
        auto& cc = screen.cells[i].cell_colors;
        d800[i] = (cc.size() > 1) ? cc[1] : 0;
    }
    return d800;
}

// Encode 8 screen RAMs for AFLI mode.
// Each byte: high nibble = color for bit 1, low nibble = color for bit 0
// AFLI cell_colors = [r0_c0, r0_c1, r1_c0, r1_c1, ..., r7_c0, r7_c1]
std::array<std::vector<std::uint8_t>, 8> encode_screen_afli(
    const quantize::ScreenResult& screen) {
    auto num_cells = screen.cells.size();
    std::array<std::vector<std::uint8_t>, 8> screens;
    for (auto& s : screens) s.resize(num_cells);

    for (std::size_t ci = 0; ci < num_cells; ++ci) {
        auto& cc = screen.cells[ci].cell_colors;
        for (std::size_t row = 0; row < 8; ++row) {
            std::uint8_t c1 = (cc.size() > row * 2 + 1) ? cc[row * 2 + 1] : 0;
            std::uint8_t c0 = (cc.size() > row * 2) ? cc[row * 2] : 0;
            screens[row][ci] = static_cast<std::uint8_t>((c1 << 4) | (c0 & 0x0F));
        }
    }
    return screens;
}

Result<PrgData> fli(const quantize::ScreenResult& screen) {
    if (screen.cells.size() != 1000)
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "FLI requires 40x25 = 1000 cells"}};

    auto bitmap = encode_bitmap_fli(screen);
    auto screens = encode_screen_fli(screen);
    auto colorram = encode_colorram_fli(screen);

    // FLI data layout: colorram($3C00) + 8 screens($4000-$5FFF) + bitmap($6000) + bg($7F40)
    std::vector<std::uint8_t> image_data;
    // Color RAM: 1000 bytes at $3C00, padded to 1024
    image_data.insert(image_data.end(), colorram.begin(), colorram.end());
    image_data.resize(1024, 0); // pad to $4000

    // 8 screen RAMs: 1000 bytes each, padded to 1024
    for (std::size_t i = 0; i < 8; ++i) {
        image_data.insert(image_data.end(), screens[i].begin(), screens[i].end());
        image_data.resize(image_data.size() + (1024 - 1000), 0); // pad
    }

    // Bitmap: 8000 bytes at $6000
    image_data.insert(image_data.end(), bitmap.begin(), bitmap.end());

    // Pad to $7F40 and add background color
    auto target_size = static_cast<std::size_t>(0x7F40 - 0x3C00);
    image_data.resize(target_size, 0);
    image_data.push_back(screen.background_color & 0x0F);

    // Pad to $8000 — ensure $7FFF = 0x00 for VIC-II idle fetch
    image_data.resize(0x8000 - 0x3C00, 0);

    return build_display_prg(detail::fli_displayer,
                              sizeof(detail::fli_displayer),
                              image_data, 0x3C00);
}

Result<PrgData> afli(const quantize::ScreenResult& screen) {
    if (screen.cells.size() != 1000)
        return std::unexpected{Error{ErrorCode::invalid_dimensions,
            "AFLI requires 40x25 = 1000 cells"}};

    auto bitmap = encode_bitmap_afli(screen);
    auto screens = encode_screen_afli(screen);

    // AFLI data layout: 8 screens($4000-$5FFF) + bitmap($6000) + bg($7F40)
    std::vector<std::uint8_t> image_data;

    // 8 screen RAMs (1000 bytes each, padded to 1024)
    for (std::size_t i = 0; i < 8; ++i) {
        image_data.insert(image_data.end(), screens[i].begin(), screens[i].end());
        image_data.resize(image_data.size() + (1024 - 1000), 0);
    }

    // Bitmap: 8000 bytes at $6000
    image_data.insert(image_data.end(), bitmap.begin(), bitmap.end());

    // Pad to $7F40 and add background color
    image_data.resize(0x7F40 - 0x4000, 0);
    image_data.push_back(screen.background_color & 0x0F);

    // Pad to $8000 to ensure $7FFF is covered
    image_data.resize(0x8000 - 0x4000, 0);

    return build_display_prg(detail::afli_displayer,
                              sizeof(detail::afli_displayer),
                              image_data, 0x4000);
}

Result<PrgData> from_screen(const quantize::ScreenResult& screen) {
    switch (screen.mode) {
    case vic2::Mode::multicolor:
        return koala(screen);
    case vic2::Mode::hires:
        return hires_bitmap(screen);
    case vic2::Mode::fli:
        return fli(screen);
    case vic2::Mode::afli:
        return afli(screen);
    default:
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "PRG export not supported for this mode",
        }};
    }
}

Result<void> write(std::string_view path, const PrgData& prg) {
    std::ofstream out{std::string{path}, std::ios::binary};
    if (!out) {
        return std::unexpected{Error{
            ErrorCode::write_failed,
            "Failed to open: " + std::string(path),
        }};
    }
    out.write(reinterpret_cast<const char*>(prg.bytes.data()),
              static_cast<std::streamsize>(prg.bytes.size()));
    return {};
}

} // namespace png2c64::prg
