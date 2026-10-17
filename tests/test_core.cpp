// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace discipline;
using namespace test;
namespace {
Frame silence(uint64_t) { return {}; }
const Host::Effect cable = [](Frame f, uint64_t) { return f; };
void warm(Host& host) { host.render(host.block * 8, silence); }
uint64_t first_sound(const std::vector<Frame>& audio, uint64_t from = 0) {
    for (uint64_t i = from; i < audio.size(); ++i) { if (std::fabs(audio[i].l) > 1e-6f) { return i; } }
    return UINT64_MAX;
}

void return_latency(const Library& lib)
{
    for (bool reverse : {false, true}) {
        for (uint32_t block : {64U, 128U, 256U}) {
            Host host(lib, 48000, block, reverse); warm(host);
            const uint64_t start = host.frame;
            const auto output = host.render(block * 8, silence, [=](Frame, uint64_t f) {
                return f == start || f == start + 3 * block ? Frame{0.25f, -0.125f} : Frame{};
            });
            const uint64_t delay = first_sound(output);
            require(delay == (reverse ? 0 : block), "return latency for host order/block");
            near(output[delay].l, 0.25f, 1e-6f, "return impulse level");
            near(output[3 * block + delay].l, 0.25f, 1e-6f, "constant return latency");
        }
    }
    std::cout << "PASS return_latency: 64/128/256 frames, both orders, random slices\n";
}

void echo_time(const Library& lib)
{
    const auto check_echo = [&](double rate, uint32_t block, float ms, bool sync, float beats, float bpm, bool reverse, bool nominal) {
        Host host(lib, rate, block, reverse, nominal);
        host.main->control(MainPort::size) = ms; host.main->control(MainPort::sync) = sync;
        host.main->control(MainPort::size_beats) = beats; host.main->control(MainPort::bpm) = bpm;
        warm(host);
        const uint64_t start = host.frame;
        const auto delay = static_cast<uint64_t>(std::llround(std::clamp(sync ? beats * 60.0 / bpm : ms * 0.001, 0.05, 60.0) * rate));
        const auto audio = host.render(delay + block * 2, [=](uint64_t f) { return f == start ? Frame{0.25f, -0.125f} : Frame{}; });
        const auto first = first_sound(audio, 1);
        require(first != UINT64_MAX && std::llabs(static_cast<int64_t>(first) - static_cast<int64_t>(delay)) <= 1,
                "echo time at " + std::to_string(rate) + " Hz / " + std::to_string(delay) + " frames; got " + std::to_string(first));
        near(audio[first].l, 0.25f, 1e-6f, "first echo left");
        near(audio[first].r, -0.125f, 1e-6f, "first echo right");
    };
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        for (float ms : {50.0f, 500.0f, 60000.0f}) { check_echo(rate, 128, ms, false, 1, 120, false, true); }
    }
    for (float bpm : {30.0f, 120.0f, 300.0f}) {
        for (float beats : {0.125f, 1.0f, 128.0f}) { check_echo(48000, 64, 500, true, beats, bpm, false, true); }
    }
    check_echo(48000, 256, 50, false, 1, 120, true, true);
    check_echo(48000, 256, 500, false, 1, 120, false, false);
    std::cout << "PASS echo_time: 50 ms/500 ms/60 s, 44.1/48/96 kHz, beats at 30/120/300 BPM, maximum-option fallback\n";
}

void routing(const Library& lib)
{
    for (int routing : {0, 1, 2}) {
        Host host(lib); host.main->control(MainPort::routing) = static_cast<float>(routing);
        host.main->control(MainPort::repeats) = 0.485f; warm(host);
        const uint64_t start = host.frame;
        const auto audio = host.render(2400 * 5, [=](uint64_t f) { return f == start ? Frame{0.25f, 0} : Frame{}; },
                                       [](Frame f, uint64_t) { return f * 0.5f; });
        for (int echo = 1; echo <= 4; ++echo) {
            const float expected = 0.125f * std::pow(routing == 2 ? 0.5f : 0.25f, echo - 1);
            near(audio[2400 * echo].l, expected, 2e-6f, "routing decay at exact Size boundary");
            if (echo > 1) { near(audio[2400 * echo - 128].l, 0, 1e-6f, "OUT must not shorten each repeat by B"); }
        }
    }
    // FEED enters memory before Clear's envelope; IN feeds the echo directly.
    float second[2]{};
    for (int routing : {0, 1}) {
        Host host(lib); host.main->control(MainPort::routing) = static_cast<float>(routing);
        host.main->control(MainPort::repeats) = 0.485f; warm(host);
        const uint64_t start = host.frame;
        const auto source = [=](uint64_t f) { return f == start ? Frame{0.25f, 0} : Frame{}; };
        std::vector<Frame> audio;
        while (audio.size() < 6000) {
            if (audio.size() == 2304) { host.main->control(MainPort::clear) = 1; }
            const auto part = host.render(128, source);
            audio.insert(audio.end(), part.begin(), part.end());
        }
        second[routing] = audio[4800].l;
    }
    near(second[0], 0.125f, 2e-6f, "Clear preserves echo feedback");
    require(second[1] < second[0] * 0.95f && second[1] > 0, "FEED actually traverses memory");
    std::cout << "PASS routing/clear: IN, OUT and FEED, exact repeat periods, memory-only Clear\n";
}

void size_no_pitch(const Library& lib)
{
    Host host(lib); host.main->control(MainPort::size) = 100;
    const auto sine = [](uint64_t f) { const float s = 0.1f * std::sin(6.283185307179586 * 1000.0 * f / 48000); return Frame{s, -s}; };
    host.render(48000, sine);
    for (float ms : {500.0f, 50.0f, 275.0f, 150.0f}) {
        host.main->control(MainPort::size) = ms;
        const auto start = host.frame;
        const auto audio = host.render(12000, sine);
        std::vector<double> crossings;
        float previous = audio[2999].l - sine(start + 2999).l;
        for (size_t i = 3000; i < audio.size(); ++i) {
            const float sample = audio[i].l - sine(start + i).l;
            if (previous <= 0 && sample > 0) { crossings.push_back(i - 1 + (-previous / (sample - previous))); }
            previous = sample;
        }
        require(crossings.size() > 100, "wet sine survives Size change");
        const double frequency = (crossings.size() - 1) * 48000 / (crossings.back() - crossings.front());
        require(std::fabs(frequency - 1000) <= 5, "Size change bends pitch outside fade");
    }
    std::cout << "PASS size_no_pitch: 1 kHz within 0.5% after four Size crossfades\n";
}

void cv(const Library& lib)
{
    Host host(lib); warm(host);
    const auto constant = [](Frame, uint64_t) { return Frame{0.1f, -0.1f}; };
    host.main->control(MainPort::level) = 0;
    std::fill(host.main->buffers[28].begin(), host.main->buffers[28].end(), 2);
    auto output = host.render(12000, silence, constant);
    near(output.back().l, 0.2f, 1e-5f, "positive Level CV clamps at full range");
    std::fill(host.main->buffers[28].begin(), host.main->buffers[28].end(), -2);
    output = host.render(12000, silence, constant);
    near(output.back().l, 0, 1e-6f, "negative Level CV clamps at zero");
    // A single-sample CV pulse must be observed even inside a long host slice.
    host.random_slices = false; host.main->buffers[28].assign(129, 0);
    host.main->buffers[28][7] = 1;
    output = host.render(128, silence, constant);
    require(output[7].l > output[6].l && output[8].l > 0 && output[8].l < output[7].l, "Level CV is sampled per sample and smoothed");
    for (float modulation : {-1.0f, 1.0f}) {
        Host h(lib); h.main->control(MainPort::size) = modulation < 0 ? 60000 : 50;
        std::fill(h.main->buffers[30].begin(), h.main->buffers[30].end(), modulation);
        const uint64_t delay = modulation < 0 ? 2400 : 2880000;
        warm(h); const auto start = h.frame;
        const auto audio = h.render(delay + 256, [=](uint64_t f) { return f == start ? Frame{0.25f, 0} : Frame{}; });
        require(first_sound(audio, 1) == delay, "Size CV spans logarithmic range: modulation " + std::to_string(modulation) +
                ", got " + std::to_string(first_sound(audio, 1)) + ", expected " + std::to_string(delay));
    }
    for (float modulation : {-1.0f, 1.0f}) {
        Host h(lib); h.main->control(MainPort::repeats) = modulation < 0 ? 1 : 0;
        std::fill(h.main->buffers[29].begin(), h.main->buffers[29].end(), modulation);
        warm(h); const auto start = h.frame;
        const auto audio = h.render(7500, [=](uint64_t f) { return f == start ? Frame{0.1f, 0} : Frame{}; });
        near(audio[4800].l, modulation < 0 ? 0.0f : 0.1f, 2e-6f, "Repeats CV clamps and controls feedback");
    }
    std::cout << "PASS cv: per-sample Level, Repeats bounds, logarithmic Size endpoints\n";
}

void stale_and_nonfinite(const Library& lib)
{
    Host host(lib); warm(host);
    const auto constant = [](Frame, uint64_t) { return Frame{0.2f, -0.1f}; };
    host.render(1024, silence, constant); host.run_return = false;
    const auto loss = host.render(30000, silence, constant);
    require(host.main->control(MainPort::paired) == 0, "stopped Return becomes unpaired");
    for (size_t i = 1; i < loss.size(); ++i) { require(std::fabs(loss[i].l - loss[i - 1].l) < 0.004f, "return loss fade"); }
    host.run_return = true;
    auto audio = host.render(2048, silence, constant);
    near(audio.back().l, 0.2f, 1e-6f, "stopped Return realigns on recovery");
    require(host.main->control(MainPort::paired) == 1, "Return recovery status");
    const auto bad_frame = host.frame + 23;
    audio = host.render(2048, silence, [=](Frame, uint64_t f) {
        return f == bad_frame ? Frame{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()} : Frame{0.2f, -0.1f};
    });
    for (auto f : audio) { require(dsp::finite(f), "non-finite Return must not escape"); }
    near(audio.back().l, 0.2f, 1e-6f, "return recovers after NaN");
    const auto bad_input = host.frame + 11;
    audio = host.render(1024, [=](uint64_t f) { return f == bad_input ? Frame{INFINITY, NAN} : Frame{0.125f, -0.25f}; });
    for (auto f : audio) { require(dsp::finite(f), "non-finite input must not escape"); }
    std::cout << "PASS pair/nonfinite: stop, fade, re-align, NaN/Inf containment and recovery\n";
}

void repeats_max(const Library& lib)
{
    Host host(lib); host.main->control(MainPort::repeats) = 1; warm(host);
    uint32_t rng = 7; float maximum = 0; double tail_energy = 0;
    const uint64_t start = host.frame;
    const auto bursts = [&](uint64_t f) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const float sample = (f - start) % 24000 < 2400 ? (static_cast<int>(rng % 2000) - 1000) * 0.0002f : 0;
        return Frame{sample, -sample};
    };
    while (host.frame < start + 48000 * 60) {
        host.period(bursts, [](Frame f, uint64_t) { return f * 8; });
        for (uint32_t i = 0; i < host.block; ++i) {
            const float value = host.main->buffers[2][i]; require(std::isfinite(value), "unity feedback remains finite");
            maximum = std::max(maximum, std::fabs(value));
            if (host.frame > start + 48000 * 59) { tail_energy += value * value; }
        }
    }
    require(maximum < 4.3f && tail_energy > 0.001, "unity feedback is bounded and audible");
    host.main->control(MainPort::level) = 0; host.render(16000, silence);
    const auto signed_zeros = [](uint64_t f) { return f % 3 == 0 ? Frame{-0.0f, 0.0f} : Frame{0.125f, -0.25f}; };
    const auto frame = host.frame; const auto dry = host.render(1024, signed_zeros);
    for (size_t i = 0; i < dry.size(); ++i) { const auto expected = signed_zeros(frame + i); require(std::memcmp(&dry[i], &expected, sizeof(Frame)) == 0, "dry path bit-exact at Level zero"); }
    std::cout << "PASS repeats_max: 60 s, unity feedback with +18 dB effect, bounded audible output and exact dry\n";
}

void standalone(const Library& lib)
{
    // A fresh Discipline with the .ttl defaults and no Return anywhere is an
    // audible delay: 500 ms echoes that repeat, and a silent Send.
    for (float modifier : {0.0f, 5.0f}) {
        Host host(lib); host.run_return = false;
        auto& m = *host.main;
        m.control(MainPort::level) = 0.5f; m.control(MainPort::repeats) = 0.4f; m.control(MainPort::size) = 500;
        m.control(MainPort::routing) = 0; m.control(MainPort::memory_dry) = 1; m.control(MainPort::modify) = 0.5f;
        m.control(MainPort::modifier) = modifier; // Outside with no Return keeps the echo inside
        warm(host);
        const uint64_t start = host.frame;
        std::vector<Frame> audio; float send = 0;
        while (audio.size() < 24000 * 3 + 256) {
            const auto part = host.render(host.block, [=](uint64_t f) { return f == start ? Frame{0.25f, -0.25f} : Frame{}; });
            audio.insert(audio.end(), part.begin(), part.end());
            for (uint32_t i = 0; i < host.block; ++i) { send = std::max(send, std::fabs(host.main->buffers[4][i])); }
        }
        near(audio[0].l, 0.25f, 0, "dry passes");
        near(audio[24000].l, 0.125f, 1e-6f, "first echo at Size, at Level");
        near(audio[48000].l, 0.125f * (0.4f / 0.97f), 1e-6f, "second echo at Repeats");
        require(first_sound(audio, 1) == 24000, "nothing before the first echo");
        near(send, modifier == 5 ? 0.25f : 0, 0, "Send carries the echo only on Outside");
        require(m.control(MainPort::paired) == 0, "no Return found");
    }
    // With a Return running, switching to and from Outside is seamless when
    // the modules between them do nothing: it sounds like never switching.
    Host host(lib), still(lib);
    for (auto* h : {&host, &still}) {
        h->main->control(MainPort::modifier) = 0; h->main->control(MainPort::repeats) = 0.485f;
        h->main->control(MainPort::dry_kill) = 1;
    }
    const auto sine = [](uint64_t f) { const float s = 0.1f * std::sin(6.283185307179586 * 220.0 * f / 48000); return Frame{s, -s}; };
    host.render(48000, sine); still.render(48000, sine);
    for (float modifier : {5.0f, 0.0f, 5.0f}) {
        host.main->control(MainPort::modifier) = modifier;
        const auto audio = host.render(12000, sine), reference = still.render(12000, sine);
        for (size_t i = 0; i < audio.size(); ++i) { near(audio[i].l, reference[i].l, 1e-6f, "Outside switch is seamless"); }
        require(host.main->control(MainPort::paired) == (modifier == 5.0f ? 1.0f : 0.0f), "Return found only on Outside");
    }
    std::cout << "PASS standalone: default controls echo without a Return, Send only on Outside; Outside falls back without one and switches seamlessly\n";
}

void rt_timing(const Library& lib)
{
    Host host(lib); warm(host); host.measure = true; uint32_t rng = 12345;
    const uint64_t end = host.frame + 48000ULL * 600;
    std::cout << "RUN rt_timing: 10 simulated minutes of random controls\n" << std::flush;
    uint32_t period = 0;
    while (host.frame < end) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if (!(period++ % 32)) {
            host.main->control(MainPort::level) = (rng % 1001) * 0.001f;
            host.main->control(MainPort::repeats) = ((rng >> 10) % 1001) * 0.001f;
            host.main->control(MainPort::size) = 50 + (rng % 59951);
            host.main->control(MainPort::routing) = static_cast<float>(rng % 3);
            host.main->control(MainPort::clear) = (rng >> 8) & 1;
            host.main->control(MainPort::scan) = ((rng >> 4) % 1001) * 0.001f;
            host.main->control(MainPort::spread) = ((rng >> 6) % 1001) * 0.001f;
            host.main->control(MainPort::scan_mode) = (rng >> 11) & 1;
            host.main->control(MainPort::scan_hold) = (rng >> 14) & 1;
            host.main->control(MainPort::collect) = (rng >> 15) & 1;
            host.main->control(MainPort::loop) = (rng >> 16) & 1;
            host.main->control(MainPort::latch) = (rng >> 17) & 1;
            host.main->control(MainPort::memory_dry) = static_cast<float>((rng >> 18) % 3);
            host.main->control(MainPort::wipe) = (rng >> 20) & 1;
            host.main->control(MainPort::dry_kill) = static_cast<float>((rng >> 21) % 3);
            host.main->control(MainPort::always) = (rng >> 23) & 1;
            host.main->control(MainPort::trails) = (rng >> 24) & 1;
            host.main->control(MainPort::enabled) = (rng >> 25) & 1;
            host.main->control(MainPort::modifier) = static_cast<float>((rng >> 26) % 6);
            host.main->control(MainPort::modify) = ((rng >> 7) % 1001) * 0.001f;
        }
        // Modify CV sweeps per sample, so Speed and the gates change inside each period.
        const float depth = ((rng >> 3) % 2001) * 0.001f - 1;
        for (uint32_t i = 0; i < host.block; ++i) {
            host.main->buffers[static_cast<uint32_t>(MainPort::modify_cv)][i] = depth * static_cast<float>(std::sin((host.frame + i) * 0.002));
        }
        host.period([](uint64_t f) { const float s = 0.1f * std::sin(f * 0.03); return Frame{s, -s}; }, cable);
        for (uint32_t i = 0; i < host.block; ++i) { require(std::isfinite(host.main->buffers[2][i]), "finite output under random controls"); }
    }
    std::sort(host.run_times.begin(), host.run_times.end());
    std::cout << "PASS rt_timing: 128-frame periods, worst " << host.run_times.back()
              << " us, p99.9 " << host.run_times[static_cast<size_t>(host.run_times.size() * 0.999)]
              << " us (desktop scheduling, not a Pi xrun result)\n";
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: test_core discipline.so [--timing]\n"; return 1; }
    try {
        Library lib(argv[1]);
        if (argc == 3 && std::string(argv[2]) == "--timing") { rt_timing(lib); return 0; }
        return_latency(lib); echo_time(lib); routing(lib); size_no_pitch(lib); cv(lib);
        stale_and_nonfinite(lib); repeats_max(lib); standalone(lib);
    } catch (const std::exception& e) { std::cerr << "FAIL core: " << e.what() << '\n'; return 1; }
}
