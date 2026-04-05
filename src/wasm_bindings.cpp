#include "api.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

using namespace emscripten;
using namespace png2c64::api;

// Convert JS options object to C++ Options
Options parse_js_options(val js_opts) {
    Options opts;
    if (js_opts.hasOwnProperty("mode"))
        opts.mode = js_opts["mode"].as<std::string>();
    if (js_opts.hasOwnProperty("palette"))
        opts.palette = js_opts["palette"].as<std::string>();
    if (js_opts.hasOwnProperty("dither"))
        opts.dither = js_opts["dither"].as<std::string>();
    if (js_opts.hasOwnProperty("gamma"))
        opts.gamma = js_opts["gamma"].as<float>();
    if (js_opts.hasOwnProperty("brightness"))
        opts.brightness = js_opts["brightness"].as<float>();
    if (js_opts.hasOwnProperty("contrast"))
        opts.contrast = js_opts["contrast"].as<float>();
    if (js_opts.hasOwnProperty("saturation"))
        opts.saturation = js_opts["saturation"].as<float>();
    if (js_opts.hasOwnProperty("ditherStrength"))
        opts.dither_strength = js_opts["ditherStrength"].as<float>();
    if (js_opts.hasOwnProperty("errorClamp"))
        opts.error_clamp = js_opts["errorClamp"].as<float>();
    if (js_opts.hasOwnProperty("adaptive"))
        opts.adaptive = js_opts["adaptive"].as<float>();
    if (js_opts.hasOwnProperty("serpentine"))
        opts.serpentine = js_opts["serpentine"].as<bool>();
    if (js_opts.hasOwnProperty("width"))
        opts.width = js_opts["width"].as<int>();
    if (js_opts.hasOwnProperty("height"))
        opts.height = js_opts["height"].as<int>();
    return opts;
}

// JS API: convert(Uint8Array, options) -> { png: Uint8Array, width, height, error }
val js_convert(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    for (std::size_t i = 0; i < length; ++i)
        input[i] = input_array[i].as<std::uint8_t>();

    auto opts = parse_js_options(js_opts);
    auto result = convert(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.png_data.empty()) {
        val png = val::global("Uint8Array").new_(result.png_data.size());
        for (std::size_t i = 0; i < result.png_data.size(); ++i)
            png.set(i, result.png_data[i]);
        obj.set("png", png);
    }

    return obj;
}

// JS API: convertRGBA(Uint8Array, options) -> { rgba: Uint8Array, width, height, error }
val js_convert_rgba(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    for (std::size_t i = 0; i < length; ++i)
        input[i] = input_array[i].as<std::uint8_t>();

    auto opts = parse_js_options(js_opts);
    auto result = convert_rgba(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.png_data.empty()) {
        val rgba = val::global("Uint8Array").new_(result.png_data.size());
        for (std::size_t i = 0; i < result.png_data.size(); ++i)
            rgba.set(i, result.png_data[i]);
        obj.set("rgba", rgba);
    }

    return obj;
}

// JS API: convertPRG(Uint8Array, options) -> { prg: Uint8Array, width, height, error }
val js_convert_prg(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    for (std::size_t i = 0; i < length; ++i)
        input[i] = input_array[i].as<std::uint8_t>();

    auto opts = parse_js_options(js_opts);
    auto result = convert_prg(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.png_data.empty()) {
        val prg = val::global("Uint8Array").new_(result.png_data.size());
        for (std::size_t i = 0; i < result.png_data.size(); ++i)
            prg.set(i, result.png_data[i]);
        obj.set("prg", prg);
    }

    return obj;
}

// JS API: convertHeader(Uint8Array, options, name) -> { header: string, width, height, error }
val js_convert_header(val input_array, val js_opts, std::string name) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    for (std::size_t i = 0; i < length; ++i)
        input[i] = input_array[i].as<std::uint8_t>();

    auto opts = parse_js_options(js_opts);
    auto result = convert_header(input.data(), input.size(), opts, name);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.png_data.empty()) {
        std::string text(result.png_data.begin(), result.png_data.end());
        obj.set("header", text);
    }

    return obj;
}

val js_convert_raw(val input_array, val js_opts, std::string format) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    for (std::size_t i = 0; i < length; ++i)
        input[i] = input_array[i].as<std::uint8_t>();

    auto opts = parse_js_options(js_opts);
    ConvertResult result;
    if (format == "koa")
        result = convert_koa(input.data(), input.size(), opts);
    else if (format == "hir")
        result = convert_hir(input.data(), input.size(), opts);
    else
        result = {{}, 0, 0, "Unknown format: " + format};

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.png_data.empty()) {
        val data = val::global("Uint8Array").new_(result.png_data.size());
        for (std::size_t i = 0; i < result.png_data.size(); ++i)
            data.set(i, result.png_data[i]);
        obj.set("data", data);
    }

    return obj;
}

EMSCRIPTEN_BINDINGS(png2c64) {
    function("convert", &js_convert);
    function("convertRGBA", &js_convert_rgba);
    function("convertPRG", &js_convert_prg);
    function("convertHeader", &js_convert_header);
    function("convertRaw", &js_convert_raw);
}
