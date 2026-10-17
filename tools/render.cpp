// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "../tests/host.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void write16(std::ostream& out, uint16_t value) {
    const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8)}; out.write(bytes, 2);
}
void write32(std::ostream& out, uint32_t value) {
    const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8), static_cast<char>(value >> 16), static_cast<char>(value >> 24)}; out.write(bytes, 4);
}
struct Wave { uint32_t rate; std::vector<test::Frame> frames; };
Wave read_wave(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    uint8_t header[12]; input.read(reinterpret_cast<char*>(header), 12);
    test::require(input && !std::memcmp(header, "RIFF", 4) && !std::memcmp(header + 8, "WAVE", 4), "input must be a RIFF WAVE file");
    std::vector<uint8_t> format, data;
    while (input.peek() != std::char_traits<char>::eof()) {
        uint8_t chunk[8]; input.read(reinterpret_cast<char*>(chunk), 8);
        test::require(bool(input), "truncated WAV chunk");
        const uint32_t size = u32(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4) || !std::memcmp(chunk, "data", 4)) {
            test::require(size <= (1U << 30), "WAV chunk exceeds 1 GB");
            auto& target = !std::memcmp(chunk, "fmt ", 4) ? format : data;
            target.resize(size); input.read(reinterpret_cast<char*>(target.data()), size);
        } else { input.seekg(size, std::ios::cur); }
        test::require(bool(input), "truncated WAV data");
        if (size & 1) { input.ignore(); }
    }
    test::require(format.size() >= 16, "WAV format chunk missing");
    const uint16_t tag = u16(format.data()), channels = u16(format.data() + 2), bits = u16(format.data() + 14);
    const uint32_t rate = u32(format.data() + 4), bytes = bits / 8;
    test::require((channels == 1 || channels == 2) && rate >= 8000 && rate <= 384000 &&
                  ((tag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32)),
                  "WAV must be mono/stereo PCM 16/24/32-bit or 32-bit float, 8-384 kHz");
    test::require(u16(format.data() + 12) == channels * bytes && data.size() % (channels * bytes) == 0, "invalid WAV block alignment");
    const auto sample = [&](size_t index) {
        const auto* p = data.data() + index * bytes;
        float result;
        if (tag == 3) { const auto value = u32(p); std::memcpy(&result, &value, sizeof(result)); }
        else {
            uint32_t raw = 0;
            for (uint32_t i = 0; i < bytes; ++i) { raw |= uint32_t(p[i]) << (8 * i); }
            const int64_t signed_value = raw & (uint32_t(1) << (bits - 1)) ? int64_t(raw) - (int64_t(1) << bits) : raw;
            result = static_cast<float>(signed_value / std::ldexp(1.0, bits - 1));
        }
        test::require(std::isfinite(result), "non-finite WAV input sample"); return result;
    };
    Wave wave{rate, {}}; wave.frames.resize(data.size() / (channels * bytes));
    for (size_t i = 0; i < wave.frames.size(); ++i) {
        wave.frames[i] = {sample(i * channels), sample(i * channels + (channels == 2 ? 1 : 0))};
    }
    return wave;
}
float setting(char** argv, int argc, int index, float fallback, float minimum, float maximum)
{
    if (argc <= index) { return fallback; }
    size_t used = 0; const std::string text(argv[index]); const float value = std::stof(text, &used);
    test::require(used == text.size() && std::isfinite(value) && value >= minimum && value <= maximum, "invalid render setting: " + text);
    return value;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 9) {
        std::cerr << "usage: render discipline.so input.wav output.wav [size-ms [repeats [level [routing [tail-seconds]]]]]\n"; return 1;
    }
    try {
        test::require(std::strcmp(argv[2], argv[3]) != 0, "input and output paths must differ");
        const Wave input = read_wave(argv[2]);
        const float size = setting(argv, argc, 4, 500, 50, 60000), repeats = setting(argv, argc, 5, 0.4f, 0, 1);
        const float level = setting(argv, argc, 6, 0.5f, 0, 1), routing = setting(argv, argc, 7, 0, 0, 2);
        test::require(std::floor(routing) == routing, "routing must be 0 (In), 1 (Feed) or 2 (Out)");
        const float tail = setting(argv, argc, 8, std::min(120.0f, size * 0.008f), 0, 120);
        const uint64_t frames = input.frames.size() + static_cast<uint64_t>(std::llround(input.rate * double(tail)));
        test::require(frames <= (UINT32_MAX - 50) / 8, "render exceeds RIFF size limit");
        test::Library library(argv[1]); test::Host host(library, input.rate);
        host.main->control(discipline::MainPort::size) = size;
        host.main->control(discipline::MainPort::level) = level;
        host.main->control(discipline::MainPort::repeats) = repeats;
        host.main->control(discipline::MainPort::routing) = routing;
        std::ofstream output(argv[3], std::ios::binary);
        test::require(bool(output), "cannot open render output");
        output.write("RIFF", 4); write32(output, 50 + static_cast<uint32_t>(frames * 8)); output.write("WAVEfmt ", 8);
        write32(output, 18); write16(output, 3); write16(output, 2); write32(output, input.rate);
        write32(output, input.rate * 8); write16(output, 8); write16(output, 32);
        write16(output, 0); output.write("fact", 4); write32(output, 4); write32(output, static_cast<uint32_t>(frames));
        output.write("data", 4); write32(output, static_cast<uint32_t>(frames * 8));
        while (host.frame < frames) {
            const auto first = host.frame;
            host.period([&](uint64_t f) { return f < input.frames.size() ? input.frames[f] : test::Frame{}; }, [](test::Frame f, uint64_t) { return f; });
            for (uint32_t i = 0; i < host.block && first + i < frames; ++i) {
                for (uint32_t port : {2U, 3U}) {
                    uint32_t bits; const float sample = host.main->buffers[port][i]; std::memcpy(&bits, &sample, sizeof(bits)); write32(output, bits);
                }
            }
        }
        test::require(bool(output), "failed to write render");
        std::cout << "Rendered " << frames << " stereo frames at " << input.rate << " Hz\n";
    } catch (const std::exception& e) { std::cerr << "render: " << e.what() << '\n'; return 1; }
}
