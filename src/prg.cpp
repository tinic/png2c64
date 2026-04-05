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

Result<PrgData> from_screen(const quantize::ScreenResult& screen) {
    switch (screen.mode) {
    case vic2::Mode::multicolor:
        return koala(screen);
    case vic2::Mode::hires:
        return hires_bitmap(screen);
    default:
        return std::unexpected{Error{
            ErrorCode::invalid_dimensions,
            "PRG export only supports multicolor and hires bitmap modes",
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
