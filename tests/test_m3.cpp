// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include "../src/dsp/clock_out.hpp"
#include "../src/dsp/modifiers.hpp"
#include <cstring>
#include <iostream>

using namespace discipline;
using namespace test;
namespace {
Frame silence(uint64_t) { return {}; }
Frame dc(uint64_t) { return {0.125f, -0.0625f}; }
std::vector<float> clock_audio(Host& host, uint64_t frames) {
    std::vector<float> output; output.reserve(frames);
    while (output.size() < frames) {
        host.period(silence, [](Frame f, uint64_t) { return f; });
        for (uint32_t i = 0; i < host.block && output.size() < frames; ++i) { output.push_back(host.main->buffers[33][i]); }
    }
    return output;
}
void clocks(const Library& lib) {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        for (uint32_t block : {64U, 128U, 256U}) {
            Host h(lib, rate, block);
            const auto period = static_cast<uint64_t>(std::llround(rate * 0.05));
            const auto width = static_cast<uint64_t>(std::llround(rate * 0.001));
            const auto audio = clock_audio(h, period * 4);
            for (size_t i = 0; i < audio.size(); ++i) { near(audio[i], i % period < width ? 1 : 0, 0, "exact Size clock period and 1 ms width"); }
        }
    }
    Host h(lib, 1000, 16); auto audio = clock_audio(h, 32);
    h.main->control(MainPort::size) = 100;
    const auto changed = clock_audio(h, 320); audio.insert(audio.end(), changed.begin(), changed.end());
    for (size_t i = 0; i < audio.size(); ++i) {
        const bool edge = i == 0 || i == 50 || (i >= 100 && i % 100 == 0);
        near(audio[i], edge ? 1 : 0, 0, "Size clock adopts committed tap only at a wrap");
    }
    Host beats(lib, 48000, 128); beats.main->control(MainPort::sync) = 1;
    beats.main->control(MainPort::bpm) = 300; beats.main->control(MainPort::size_beats) = 0.5f;
    const auto synced = clock_audio(beats, 19200);
    for (size_t i = 0; i < synced.size(); ++i) { near(synced[i], i % 4800 < 48 ? 1 : 0, 0, "clock follows BPM and beats"); }
    Host longest(lib, 1000, 16); longest.main->control(MainPort::size) = 50;
    for (auto& cv : longest.main->buffers[30]) { cv = 1; }
    const auto slow = clock_audio(longest, 120032);
    for (size_t i = 0; i < slow.size(); ++i) { near(slow[i], i % 60000 == 0 ? 1 : 0, 0, "log Size CV reaches 60-second clock"); }
    Host control(lib, 1000, 16); clock_audio(control, 512);
    control.main->control(MainPort::enabled) = 0; control.main->control(MainPort::loop) = 1;
    control.main->control(MainPort::clear) = 1; control.main->control(MainPort::pair) = 2; control.ret->controls[2] = 2;
    const auto continued = clock_audio(control, 512);
    for (size_t i = 0; i < continued.size(); ++i) { near(continued[i], (i + 512) % 50 == 0 ? 1 : 0, 0, "bypass/Loop/Clear/Pair do not reset clock phase"); }
    control.main->activate(); const auto reset = clock_audio(control, 32);
    near(reset[0], 1, 0, "activation restarts the clock"); near(reset[1], 0, 0, "activation pulse width");
    std::cout << "PASS m3_clock: sample-exact pulses, 44.1/48/96 kHz, 64/128/256 frames, committed Size, tempo/CV and lifecycle\n";
}
void wet_only(Host& h, int mode, float amount) {
    h.main->control(MainPort::modifier) = static_cast<float>(mode); h.main->control(MainPort::modify) = amount;
    h.main->control(MainPort::dry_kill) = 1; h.main->control(MainPort::memory_dry) = 0;
}
void speed(const Library& lib) {
    const auto sine = [](uint64_t f) { const float x = static_cast<float>(0.05 * std::sin(f * (6.283185307179586 * 1000 / 48000))); return Frame{x, -0.5f * x}; };
    for (const auto& setting : {std::pair{1, 0.0f}, {1, 1.0f}, {1, 0.5f}, {2, 0.0f}, {2, 0.25f}, {2, 0.5f}, {2, 0.625f}, {2, 0.75f}, {2, 0.875f}, {2, 1.0f}}) {
        Host h(lib); wet_only(h, setting.first, setting.second); h.render(9600, sine);
        const auto audio = h.render(24000, sine);
        const double playback = dsp::Modifiers::playback_rate(setting.first, setting.second);
        float peak = 0; std::vector<double> crossings;
        for (size_t i = 1; i < audio.size(); ++i) {
            require(dsp::finite(audio[i]), "finite speed output"); peak = std::max(peak, std::fabs(audio[i].l));
            near(audio[i].r, -0.5f * audio[i].l, 1e-7f, "stereo-coherent playback");
            if (audio[i - 1].l <= 0 && audio[i].l > 0) {
                crossings.push_back(i - 1 + -audio[i - 1].l / (audio[i].l - audio[i - 1].l));
            }
        }
        if (playback == 0) { near(peak, 0, 0, "stopped playback produces no held DC"); continue; }
        require(peak > 0.02f && peak < 0.07f && crossings.size() > 50, "audible, bounded speed output");
        std::vector<double> intervals;
        for (size_t i = 1; i < crossings.size(); ++i) { intervals.push_back(crossings[i] - crossings[i - 1]); }
        std::sort(intervals.begin(), intervals.end());
        const double measured = 48000 / intervals[intervals.size() / 2];
        require(std::fabs(measured - 1000 * std::fabs(playback)) < 1000 * std::fabs(playback) * 0.005,
                "speed pitch differs from target: " + std::to_string(measured) + " / " + std::to_string(playback));
    }
    for (uint32_t block : {64U, 128U, 256U}) {
        Host h(lib, 48000, block); h.main->control(MainPort::size) = 100; wet_only(h, 2, 0.25f);
        const auto ramp = [](uint64_t f) { return Frame{static_cast<float>(f) * 1e-6f, 0}; };
        h.render(19200, ramp); const auto start = h.frame; const auto audio = h.render(9600, ramp);
        for (size_t i = 0; i < audio.size(); ++i) {
            const auto t = start + i, phase = t % 4800;
            if (phase == 200 || phase == 500 || phase == 3000) {
                near(audio[i].l, ramp((t / 4800) * 4800 - block - 1 - phase).l, 1e-7f, "reverse plays the complete captured Size window backwards");
            }
        }
    }
    for (float amount : {0.0f, 1.0f}) {
        Host h(lib, 1000, 16); h.main->control(MainPort::size) = 60000; wet_only(h, 1, amount);
        const auto ramp = [](uint64_t f) { return Frame{static_cast<float>(f) * 1e-7f, 0}; };
        h.render(180000, ramp); const auto start = h.frame; const auto audio = h.render(60000, ramp);
        for (size_t i : {1000U, 10000U, 31000U, 59000U}) {
            // Above 30 s, Speed captures two 30 s chunks per echo; each is
            // read during the next chunk, inside the 60 s echo line.
            const auto t = start + i, chunk = t / 30000 * 30000, phase = t % 30000;
            const auto index = amount == 0 ? (29999 - 4 * phase % 30000) : ((16 + 4 * phase) % 30000);
            near(audio[i].l, ramp(chunk - 16 - 30000 + index).l, 2e-7f, "60-second Size plays 30 s 4x/reverse chunks from the 60 s echo line");
        }
    }
    {
        Host h(lib, 1000, 16); h.main->control(MainPort::size) = 30000; wet_only(h, 2, 0.25f);
        const auto ramp = [](uint64_t f) { return Frame{static_cast<float>(f) * 1e-7f, 0}; };
        h.render(90000, ramp); const auto start = h.frame; const auto audio = h.render(30000, ramp);
        for (size_t i : {1000U, 15000U, 29000U}) {
            const auto t = start + i, period = t / 30000 * 30000, phase = t % 30000;
            near(audio[i].l, ramp(period - 16 - 30000 + 29999 - phase).l, 2e-7f, "30-second Size still reverses the complete window");
        }
    }
    Host changed(lib, 1000, 16); wet_only(changed, 2, 0.25f);
    changed.main->control(MainPort::size) = 100;
    const auto code = [](uint64_t f) { return Frame{static_cast<float>(f) * 1e-4f, 0}; };
    changed.render(160, code); changed.main->control(MainPort::size) = 200;
    const auto after = changed.render(256, code);
    for (uint64_t phase : {120U, 170U}) {
        near(after[200 + phase - 160].l, code(184 - 1 - phase).l, 1e-7f,
             "Size committed during Send lead updates the whole reverse window at the same boundary");
    }
    std::cout << "PASS m3_speed: stepped/smooth, stop without DC, measured pitch, stereo, full-window reverse, 30 s chunks above 30 s\n";
}
double amplitude(const std::vector<Frame>& audio, double frequency) {
    double re = 0, im = 0;
    for (size_t i = 0; i < audio.size(); ++i) {
        const double phase = 6.283185307179586 * frequency * i / 48000;
        re += audio[i].l * std::cos(phase); im += audio[i].l * std::sin(phase);
    }
    return 2 * std::hypot(re, im) / audio.size();
}
void antialias_and_neutral(const Library& lib) {
    Host h(lib); wet_only(h, 2, 1);
    const auto tones = [](uint64_t f) { return Frame{static_cast<float>(0.03 * (std::sin(f * (6.283185307179586 * 1000 / 48000)) + std::sin(f * (6.283185307179586 * 19000 / 48000)))), 0}; };
    h.render(9600, tones); const auto audio = h.render(48000, tones);
    const auto wanted = amplitude(audio, 2000), alias = amplitude(audio, 10000);
    require(wanted > 0.015 && alias < wanted * 0.1, "2x suppresses the 19 kHz alias by at least 20 dB");
    const auto neutral = [&](int mode, float amount) {
        Host n(lib); wet_only(n, mode, amount); n.main->control(MainPort::repeats) = 0.485f;
        return n.render(12000, [](uint64_t f) { const auto x = static_cast<float>(static_cast<int>((f * 16807) % 1000) - 500) * 0.00025f; return Frame{x, -x}; });
    };
    const auto plain = neutral(0, 0.5f);
    for (const auto& setting : {std::pair{2, 0.75f}, {3, 0.5f}, {4, 0.0f}}) {
        const auto tested = neutral(setting.first, setting.second);
        require(std::memcmp(plain.data(), tested.data(), plain.size() * sizeof(Frame)) == 0, "neutral modifier preserves delay and feedback bit-exactly");
    }
    std::cout << "PASS m3_alias/neutral: antialias filtering and bit-exact unity/neutral settings\n";
}
void rhythm(const Library& lib) {
    for (float amount : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        Host h(lib); h.main->control(MainPort::size) = 100; wet_only(h, 3, amount);
        h.render(19200, dc); const auto start = h.frame; const auto audio = h.render(9600, dc);
        for (size_t i = 0; i < audio.size(); ++i) {
            const auto phase = (start + i) % 4800;
            if (phase != 1200 && phase != 3600) { continue; }
            const bool kept = amount == 0.5f || (amount == 0.25f && phase == 3600) || (amount == 0.75f && phase == 1200);
            near(audio[i].l, kept ? 0.125f : 0, 1e-7f, "Trimmer cuts the correct Size-synchronized half");
        }
    }
    for (float amount : {0.5f, 1.0f}) {
        Host h(lib); h.main->control(MainPort::size) = 1000; wet_only(h, 4, amount);
        h.render(144000, dc); const auto audio = h.render(96000, dc);
        unsigned on = 0, off = 0, edges = 0;
        for (size_t i = 0; i < 48000; ++i) {
            require(std::memcmp(&audio[i], &audio[i + 48000], sizeof(Frame)) == 0, "Dropper pattern repeats at the same place in every echo");
            require(audio[i].l >= 0 && audio[i].l <= 0.125001f, "Dropper never boosts signal");
            on += audio[i].l > 0.1f; off += audio[i].l < 0.01f;
            if (i && audio[i - 1].l <= 0.0625f && audio[i].l > 0.0625f) { ++edges; }
        }
        require(on > 1000 && off > 1000, "Dropper contains both sound and gaps");
        if (amount == 1) { require(edges > 16, "maximum Dropper produces short grains rather than muting everything"); }
    }
    std::cout << "PASS m3_rhythm: Trimmer start/end cuts, neutral, bounded repeating Dropper and granular maximum\n";
}
void transitions_and_slices(const Library& lib) {
    const auto render = [&](bool sliced) {
        Host h(lib); h.random_slices = sliced; wet_only(h, 0, 0.5f); h.render(9600, dc);
        std::vector<Frame> result;
        for (const auto& setting : {std::pair{1, 1.0f}, {2, 0.0f}, {3, 0.25f}, {4, 0.95f}, {0, 0.5f}}) {
            h.main->control(MainPort::modifier) = setting.first; h.main->control(MainPort::modify) = setting.second;
            const auto audio = h.render(9600, dc); result.insert(result.end(), audio.begin(), audio.end());
        }
        return result;
    };
    const auto a = render(false), b = render(true);
    require(std::memcmp(a.data(), b.data(), a.size() * sizeof(Frame)) == 0, "all modifiers and transitions are slice independent");
    float previous = a.front().l;
    for (auto f : a) { require(dsp::finite(f) && std::fabs(f.l - previous) < 0.005f, "mode/amount changes stay bounded without a DC click"); previous = f.l; }
    Host fast(lib); wet_only(fast, 1, 1); fast.main->control(MainPort::size) = 100;
    const auto saw = [](uint64_t f) { return Frame{static_cast<float>(f % 4800) * (0.1f / 4800), 0}; };
    fast.render(19200, saw); const auto wraps = fast.render(19200, saw);
    for (size_t i = 1; i < wraps.size(); ++i) {
        // The source itself has a discontinuity each quarter at 4x. Inspect
        // the Size restart, where an interrupted wrap fade must mask it.
        if (i % 4800 >= 64) { continue; }
        require(std::fabs(wraps[i].l - wraps[i - 1].l) < 0.005f, "4x window wrap/restart step at " + std::to_string(i) + ": " + std::to_string(wraps[i - 1].l) + " -> " + std::to_string(wraps[i].l));
    }
    std::cout << "PASS m3_transitions: mode/amount smoothing, bounded DC steps, bit-exact host-slice independence\n";
}
void routing_and_discard(const Library& lib) {
    for (int routing : {0, 1, 2}) {
        Host h(lib, 1000, 16); wet_only(h, 0, 0.5f);
        h.main->control(MainPort::size) = 100; h.main->control(MainPort::repeats) = 1;
        h.main->control(MainPort::routing) = static_cast<float>(routing);
        const auto phrase = [](uint64_t f) { return f < 100 ? Frame{0.01f, -0.005f} : Frame{}; };
        h.render(512, phrase);
        h.main->control(MainPort::modifier) = 3; h.main->control(MainPort::modify) = 0;
        const auto muted = h.render(1024, phrase);
        near(muted.back().l, 0, 1e-8f, "Trimmer mutes direct echoes in every routing");
        h.main->control(MainPort::modifier) = 0;
        const auto restored = h.render(512, phrase);
        float peak = 0;
        for (size_t i = restored.size() - 200; i < restored.size(); ++i) { peak = std::max(peak, std::fabs(restored[i].l)); }
        if (routing == 2) { require(peak > 0.005f, "OUT modifier leaves internal repeats intact"); }
        else { near(peak, 0, 1e-7f, "IN/FEED modifier acts on recirculating Return"); }
    }
    Host h(lib); wet_only(h, 2, 0.25f); h.main->control(MainPort::repeats) = 1;
    h.render(19200, dc); h.main->control(MainPort::enabled) = 0;
    h.render(128, silence); h.main->control(MainPort::enabled) = 1;
    const auto emptied = h.render(19200, silence);
    float peak = 0;
    for (size_t i = 4800; i < emptied.size(); ++i) { peak = std::max(peak, std::fabs(emptied[i].l)); }
    near(peak, 0, 0, "short bypass invalidates captured reverse windows as well as the echo");
    std::cout << "PASS m3_routing/discard: OUT preserves repeats, IN/FEED process them, bypass discards captured windows\n";
}
void modify_cv(const Library& lib) {
    constexpr auto cv_port = static_cast<uint32_t>(MainPort::modify_cv);
    const auto noise = [](uint64_t f) { const auto x = static_cast<float>(static_cast<int>((f * 16807) % 1000) - 500) * 0.00025f; return Frame{x, -x}; };
    const auto render = [&](int mode, float knob, const std::function<float(uint64_t)>& cv, bool sliced) {
        Host h(lib); h.random_slices = sliced; wet_only(h, mode, knob); h.main->control(MainPort::repeats) = 0.485f;
        h.main->control(MainPort::size) = 100;
        std::vector<Frame> result;
        while (result.size() < 24000) {
            for (uint32_t i = 0; i < h.block; ++i) { h.main->buffers[cv_port][i] = cv(h.frame + i); }
            h.period(noise, [](Frame f, uint64_t) { return f; });
            for (uint32_t i = 0; i < h.block; ++i) { result.push_back({h.main->buffers[2][i], h.main->buffers[3][i]}); }
        }
        return result;
    };
    const auto same = [](const std::vector<Frame>& a, const std::vector<Frame>& b) {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Frame)) == 0;
    };
    const auto none = [](uint64_t) { return 0.0f; };
    const auto constant = [](float value) { return [value](uint64_t) { return value; }; };
    // Knob plus CV, then clamped, in Modify's 0..1 range.
    require(same(render(2, 0.25f, constant(0.5f), false), render(0, 0.5f, none, false)), "Modify CV reaches Smooth unity bit-exactly");
    require(same(render(3, 0.75f, constant(-0.25f), false), render(0, 0.5f, none, false)), "Modify CV reaches Trimmer noon bit-exactly");
    require(same(render(1, 0.0f, constant(6.0f / 7), false), render(1, 6.0f / 7, none, false)), "Modify CV selects the same Stepped rate as the knob");
    require(same(render(3, 0.75f, constant(1), false), render(3, 1, none, false)), "Modify CV clamps at 1");
    require(same(render(2, 0.25f, constant(-1), false), render(2, 0, none, false)), "Modify CV clamps at 0");
    require(same(render(4, 0.0f, constant(2), false), render(4, 1, none, false)), "Modify CV input is limited to +1");
    require(!same(render(3, 0.5f, constant(0.25f), false), render(0, 0.5f, none, false)), "Modify CV changes the selected modifier");
    // A per-sample LFO must not depend on host slicing (no per-run sampling).
    const auto lfo = [](uint64_t f) { return 0.2f * static_cast<float>(std::sin(f * 0.0021)); };
    for (int mode : {1, 2, 3, 4}) {
        require(same(render(mode, 0.6f, lfo, false), render(mode, 0.6f, lfo, true)), "Modify CV is read per sample, independent of slices");
    }
    std::cout << "PASS m3_modify_cv: knob plus CV, clamping, exact neutral/stepped settings, per-sample and slice independent\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_m3 discipline.so"); Library lib(argv[1]);
        clocks(lib); speed(lib); antialias_and_neutral(lib); rhythm(lib); transitions_and_slices(lib); routing_and_discard(lib); modify_cv(lib);
    } catch (const std::exception& e) { std::cerr << "FAIL Milestone 3: " << e.what() << '\n'; return 1; }
}
