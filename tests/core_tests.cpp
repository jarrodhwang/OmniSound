#include <omni/model.hpp>
#include <omni/dsp.hpp>
#include <omni/signal_quality.hpp>
#include <omni/routing.hpp>
#include <omni/localization.hpp>
#include <iostream>
#include <cmath>
#include <limits>
#include <numbers>
#include <functional>
#include <algorithm>
using namespace omni;
static void require(bool b, const char *m)
{
    if (!b)
        throw std::runtime_error(m);
}
static void rejects(const std::function<void()> &f)
{
    bool rejected = false;
    try
    {
        f();
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    require(rejected, "Invalid input was accepted");
}
static Config config()
{
    return {1,
            0,
            {{"9b3e68c4-8507-4c20-a5a5-07ce99f4c3a4",
              "책상",
              {{"physical-A", 1, 0, {}}, {"physical-B", 0.7, 20, {}}},
              200}}};
}
int main()
{
    int passed = 0, failed = 0;
    auto test = [&](const char *name, auto f) {
        try
        {
            f();
            ++passed;
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception &e)
        {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("Versioned configuration and Unicode round trip", [] {
        auto c = config();
        validate(c);
        auto copy = Json::parse(Json(c).dump()).get<Config>();
        require(Json(c) == Json(copy), "Configuration changed on round trip");
    });
    test("Pro speaker routing preserves stereo and isolates discrete surround channels", [] {
        Member member;
        const std::array<float, 8> values{.2f, -.3f, .4f, .1f, .5f, .6f, .7f, .8f};
        const auto original = routing_matrix(member, 2, 3).apply(values);
        require(original.l == values[0] && original.r == values[1], "Stereo PCM was modified");
        member.routing = "swap";
        const auto swapped = routing_matrix(member, 2, 3).apply(values);
        require(swapped.l == values[1] && swapped.r == values[0], "Stereo channel swap failed");
        member.routing = "mono";
        const auto mono = routing_matrix(member, 2, 3).apply(values);
        require(std::abs(mono.l + .05f) < 1e-7 && mono.l == mono.r, "Mono sum failed");
        const std::array<float, 6> side_values{1, 2, 3, 4, 5, 6};
        const auto canonical = canonical_channels(side_values.data(), 6, 0x60f);
        require(canonical[4] == 0 && canonical[5] == 0 && canonical[6] == 5 && canonical[7] == 6,
                "5.1 side speakers were confused with rear speakers");
        member.routing = "side-left";
        require(routing_matrix(member, 6, 0x60f).apply(canonical).l == 5, "Discrete side isolation failed");
        member.routing = "back-left";
        require(routing_matrix(member, 6, 0x60f).apply(canonical).l == 0,
                "Missing input channel was synthesized");
        member.routing = "stereo";
        const std::array<float, 8> peak{1, 1, 1, 1, 1, 1, 1, 1};
        const auto mix = routing_matrix(member, 8, 0x63f).apply(peak);
        require(mix.l <= 1.000001 && mix.r <= 1.000001, "Surround downmix exceeded unity headroom");
        member.routing = "position";
        member.azimuth_deg = -30;
        const auto near = routing_matrix(member, 2, 3).apply(values);
        member.distance_m = 2;
        const auto far = routing_matrix(member, 2, 3).apply(values);
        require(std::abs(far.l - near.l * .5f) < 1e-7 && far.l == far.r,
                "Position distance attenuation failed");
    });
    test("Pro live controls keep stream topology and persist with legacy defaults", [] {
        auto before = config().groups.front(), after = before;
        after.members[0].routing = "position";
        after.members[0].azimuth_deg = -45;
        after.members[0].distance_m = 2;
        after.members[0].delay_ms = 12;
        after.safety_ms = 0;
        require(same_stream_topology(before, after), "Live controls would reopen streams");
        const auto restored = Json(after).get<Group>();
        require(Json(after) == Json(restored), "Pro controls did not persist");
        after.output_processing = "system";
        require(!same_stream_topology(before, after), "Processing change did not reopen streams");
        Json legacy = before;
        legacy.erase("outputProcessing");
        for (auto &member : legacy["members"])
        {
            member.erase("routing");
            member.erase("azimuthDegrees");
            member.erase("distanceMeters");
        }
        require(legacy.get<Group>().members[0].routing == "stereo", "Legacy stereo default changed");
        auto c = config();
        c.groups.front() = restored;
        validate(c);
        c.groups.front().members[0].distance_m = std::numeric_limits<double>::quiet_NaN();
        rejects([&] { validate(c); });
    });
    test("Five complete UI languages and unsupported locale fallback", [] {
        require(i18n::complete_catalog(), "Missing language translation");
        require(i18n::resolve_language("FR-ca") == "fr" && i18n::resolve_language("ja-JP") == "ja" &&
                    i18n::resolve_language("es-ES") == "en",
                "Locale fallback is incorrect");
        for (const auto *locale : {"en-US", "ko-KR", "fr-FR", "de-DE", "ja-JP"})
        {
            i18n::set_language(locale);
            require(!i18n::text("Save").empty() && i18n::text("DELL S2725DS") == "DELL S2725DS",
                    "Device identity was translated");
        }
        i18n::set_language("es-ES");
        require(i18n::text("저장") == "Save", "Unsupported language did not fall back to English");
    });
    test("Empty groups save and restore without disabling validation", [] {
        auto c = config();
        c.groups.front().members.clear();
        c.active_group_id = c.groups.front().id;
        validate(c);
        const auto restored = Json::parse(Json(c).dump()).get<Config>();
        require(restored.groups.front().members.empty() && active_group_id(restored) == c.active_group_id,
                "Empty active group changed on save");
        require(lite_mirror_targets(restored.groups.front(), "physical-A").members.empty(),
                "Empty group created a mirror target");
        c.groups.front().safety_ms = -1;
        rejects([&] { validate(c); });
        c.groups.front().safety_ms = 200;
        for (std::size_t i = 0; i <= max_members; ++i)
            c.groups.front().members.push_back({"physical-" + std::to_string(i), 1, 0, {}});
        rejects([&] { validate(c); });
    });
    test("Lite keeps the default output direct and mirrors only other members", [] {
        auto group = config().groups.front();
        const auto mirrored = lite_mirror_targets(group, "physical-A");
        require(mirrored.members.size() == 1 && mirrored.members.front().endpoint_id == "physical-B",
                "Lite did not exclude the already-playing default output");
        require(group.members.size() == 2, "Lite modified the saved group");
        const auto direct_only = lite_mirror_targets(group, "physical-A");
        require(lite_mirror_targets(direct_only, "physical-B").members.empty(),
                "Lite did not detect a group with no mirror target");
    });
    test("Only one active group persists and legacy settings choose the first group", [] {
        auto c = config();
        require(active_group_id(c) == c.groups.front().id, "Legacy active group was not restored");
        auto second = c.groups.front();
        second.id = "81d6e72b-90b6-4a54-9ffc-b7df174d6b90";
        second.name = "거실";
        c.groups.push_back(second);
        c.active_group_id = second.id;
        require(active_group_id(Json(c).get<Config>()) == second.id, "Active group changed after save");
        c.active_group_id = "unknown";
        rejects([&] { validate(c); });
    });
    test("Reject malformed, duplicate and unsafe settings", [] {
        auto c = config();
        c.version = 2;
        rejects([&] { validate(c); });
        c = config();
        c.groups.push_back(c.groups[0]);
        rejects([&] { validate(c); });
        c = config();
        c.groups[0].members.push_back(c.groups[0].members[0]);
        rejects([&] { validate(c); });
        c = config();
        c.groups[0].members[0].gain = std::numeric_limits<double>::quiet_NaN();
        rejects([&] { validate(c); });
        c = config();
        c.groups[0].members[0].delay_ms = -1;
        rejects([&] { validate(c); });
        c.groups[0].members[0].delay_ms = 2001;
        rejects([&] { validate(c); });
        c = config();
        c.groups[0].id = "../../escape";
        rejects([&] { validate(c); });
        c = config();
        c.groups[0].name = "bad\nname";
        rejects([&] { validate(c); });
    });
    test("Manual delay and Windows volume policy survive settings roundtrip", [] {
        auto c = config();
        c.groups[0].members[0].delay_ms = 2000;
        c.groups[0].members[0].follow_reference_volume = true;
        validate(c);
        auto restored = Json(c).get<Config>();
        require(restored.groups[0].members[0].follow_reference_volume &&
                    restored.groups[0].members[0].delay_ms == 2000,
                "Volume policy or signed delay lost");
        require(playback_delay_seconds(0.02, 0.02, 0) == 0.02, "Zero delay added unnecessary waiting");
        require(std::abs(playback_delay_seconds(0.08, 0.02, 100) - 0.18) < 1e-9, "Positive delay failed");
    });
    test("Never certify a failed calibration", [] {
        auto c = config();
        c.groups[0].members[0].calibration = Calibration{10, 0.1, 5, "now", "device", "mic", true};
        rejects([&] { validate(c); });
        c.groups[0].members[0].calibration = Calibration{10, 0.1, 0.9, "now", "device", "mic", true};
        rejects([&] { validate(c); });
    });
    test("Timeline wrap and gaps never replay stale frames", [] {
        Timeline ring(128);
        std::vector<Frame> data(128, {1, 2});
        ring.put(0, data);
        Frame v;
        require(ring.get(64, v) && v.r == 2, "Written sample lost");
        ring.put(256, data);
        require(!ring.get(64, v), "Stale sample replayed");
        require(!ring.get(200, v), "Gap filled with stale sample");
        ring.silence(300, 4);
        require(ring.get(300, v) && v.l == 0, "Silence failed");
        ring.clear();
        require(!ring.get(300, v), "Reset retained sample");
    });
    test("Fractional resampling preserves stereo and sinusoid phase", [] {
        Timeline ring(8192);
        SincSampler sampler;
        std::vector<Frame> data(4096);
        for (size_t i = 0; i < data.size(); ++i)
        {
            auto s = static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * i / 48000));
            data[i] = {s, -s};
        }
        ring.put(0, data);
        double max_error = 0;
        for (double p = 100.25; p < 3900; p += 1.00045)
        {
            Frame v;
            require(sampler.sample(ring, p, v), "Valid sample unavailable");
            max_error =
                std::max(max_error, std::abs(v.l - std::sin(2 * std::numbers::pi * 1000 * p / 48000)));
            require(std::abs(v.l + v.r) < 1e-6, "Channels mixed");
        }
        require(max_error < 0.001, "Resampler phase or amplitude error");
    });
    test("Unity sampling is exact and zero-weight endpoints do not cause dropouts", [] {
        Timeline ring(2048);
        SincSampler sampler;
        Frame original{.12345678f, -.87654321f}, sampled;
        ring.put(1000, std::span(&original, 1));
        require(sampler.sample(ring, 1000, sampled) && sampled.l == original.l && sampled.r == original.r,
                "Unity sampling modified PCM or required unused taps");
        require(!sampler.sample(ring, 1000.5, sampled), "Missing nonzero interpolation taps were ignored");
        ring.clear();
        std::vector<Frame> frames(127, {.25f, -.25f});
        ring.put(500 - SincSampler::left_guard, frames);
        require(sampler.sample(ring, 500.5, sampled) && std::abs(sampled.l - .25) < 1e-6,
                "Absent zero-weight outer tap caused a false underrun");
    });
    test("High-frequency stereo resampling preserves the source waveform", [] {
        Timeline ring(8192);
        SincSampler sampler;
        std::vector<Frame> data(8192);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = {static_cast<float>(.8 * std::sin(2 * std::numbers::pi * 20000 * i / 48000)),
                       static_cast<float>(.7 * std::sin(2 * std::numbers::pi * 17003 * i / 48000))};
        ring.put(0, data);
        double error = 0, signal = 0, max_error = 0;
        for (double p = 100.123456; p < 8000; p += 1.00037)
        {
            Frame v;
            require(sampler.sample(ring, p, v), "Complete high-frequency sample missing");
            double expected[] = {.8 * std::sin(2 * std::numbers::pi * 20000 * p / 48000),
                                 .7 * std::sin(2 * std::numbers::pi * 17003 * p / 48000)};
            double actual[] = {v.l, v.r};
            for (int c = 0; c < 2; ++c)
            {
                error += std::pow(actual[c] - expected[c], 2);
                signal += expected[c] * expected[c];
                max_error = std::max(max_error, std::abs(actual[c] - expected[c]));
            }
        }
        double snr = 10 * std::log10(signal / error);
        std::cout << "  High-frequency interpolation SNR: " << snr << " dB; max error: " << max_error << '\n';
        require(snr > 100 && max_error < .00001, "Resampling added high-frequency quantization noise");
    });
    test("Packet gaps and reconnects fade without one-sample edges", [] {
        Timeline ring(8192);
        SincSampler sampler;
        ContinuousRenderer renderer;
        std::vector<Frame> data(8192, {.7f, -.7f});
        ring.put(0, data);
        std::array<Frame, 256> out;
        auto result = renderer.render(ring, sampler, out, 100, 1, {1, 1});
        require(result.missing_frames == 0 && out.back().l > .69, "Initial fade failed");
        ring.clear();
        result = renderer.render(ring, sampler, out, renderer.position(), 1, {1, 1});
        require(result.missing_frames == out.size(), "Packet gap not detected");
        double worst = std::abs(.7 - out.front().l);
        for (size_t i = 1; i < out.size(); ++i)
            worst = std::max(worst, static_cast<double>(std::abs(out[i].l - out[i - 1].l)));
        require(worst < .006 && out.back().l == 0, "Gap produced an abrupt sample edge");
        ring.put(0, data);
        renderer.reset();
        result = renderer.render(ring, sampler, out, 1000, 1, {1, 1});
        require(out.front().l == 0 && out.back().l > .69 && !result.missing_frames,
                "Reconnect did not fade into valid audio");
        renderer.silence(out);
        require(out.front().l > .69 && out.back().l == 0, "Idle did not fade previous audio out");
        renderer.silence(out);
        require(std::all_of(out.begin(), out.end(), [](Frame f) { return f.l == 0 && f.r == 0; }),
                "Idle repeated the old audio tail");
        renderer.render(ring, sampler, out, 1500, 1, {1, 1});
        std::array<Frame, 32> short_block;
        renderer.silence(short_block);
        float previous = short_block.back().l;
        renderer.render(ring, sampler, short_block, 1800, 1, {1, 1});
        require(std::abs(short_block.front().l - previous) < .006,
                "Resume in a partial fade produced an abrupt edge");
    });
    test("Float headroom and gain transitions avoid premature clipping", [] {
        Timeline ring(8192);
        SincSampler sampler;
        ContinuousRenderer renderer;
        std::vector<Frame> data(8192, {.8f, -.8f});
        ring.put(0, data);
        std::array<Frame, 2048> out;
        auto result = renderer.render(ring, sampler, out, 100, 1, {2, 2});
        require(result.headroom_samples > 0 && result.invalid_samples == 0, "Float headroom not diagnosed");
        require(out.back().l > 1.59f && out.back().r < -1.59f, "Samples hard-clipped before endpoint volume");
        float previous = out.back().l;
        result = renderer.render(ring, sampler, out, renderer.position(), 1, {1, 1});
        require(std::abs(previous - out.front().l) < .005f && out.back().l < .801,
                "Volume change made an abrupt edge");
    });
    test("Clock jitter is smoothed without repeated source jumps", [] {
        Timeline ring(48000 * 2);
        SincSampler sampler;
        ContinuousRenderer renderer;
        std::vector<Frame> data(48000 * 2, {.1f, .2f});
        ring.put(0, data);
        std::array<Frame, 240> out;
        size_t missing = 0, jumps = 0;
        for (int i = 0; i < 300; ++i)
        {
            double desired = 1000 + i * 240 * 1.0004 + (i % 3 - 1) * 4;
            double noisy_ratio = 1.0004 + (i % 3 - 1) * .0005;
            auto result = renderer.render(ring, sampler, out, desired, noisy_ratio, {1, 1});
            missing += result.missing_frames;
            jumps += result.resynchronized;
        }
        require(!missing && !jumps, "Small clock jitter produced gaps or discontinuities");
        require(std::abs(renderer.position() - (1000 + 300 * 240 * 1.0004)) < 48,
                "Smoothing accumulated unbounded drift");
    });
    test("Quality analyser separates noninteger tone cycles from DC and distortion", [] {
        std::vector<Frame> audio(4800);
        for (size_t i = 0; i < audio.size(); ++i)
        {
            double t = static_cast<double>(i) / 48000;
            audio[i] = {
                static_cast<float>(.02 + .07 * std::sin(2 * std::numbers::pi * 997 * 1.000007 * t + .3)),
                static_cast<float>(.05 * std::sin(2 * std::numbers::pi * 19001 * t - .2))};
        }
        auto left = fit_tone(audio, 0, 997, 7), right = fit_tone(audio, 1, 19001);
        require(10 * std::log10(left.signal / left.residual) > 130 &&
                    10 * std::log10(right.signal / right.residual) > 130,
                "Window/DC bias was reported as hardware distortion");
        require(std::abs(left.amplitude - .07) < 1e-7, "Tone amplitude is biased");
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i].l += static_cast<float>(.007 * std::sin(2 * std::numbers::pi * 2991 * i / 48000));
        left = fit_tone(audio, 0, 997, 7);
        require(std::abs(10 * std::log10(left.signal / left.residual) - 20) < .1,
                "Known ten percent harmonic distortion was not detected");
        std::fill(audio.begin(), audio.end(), Frame{});
        auto silence = fit_tone(audio, 0, 997);
        require(silence.signal == 0 && silence.residual == 0 && std::isfinite(silence.amplitude),
                "Silence produced nonfinite metrics");
    });
    test("Independent clocks hold prediction over simulated 8 hours", [] {
        for (double ppm : {-500.0, 0.0, 500.0})
        {
            ClockFit fit;
            double rate = 48000 * (1 + ppm / 1e6);
            double max_error = 0;
            for (int i = 0; i < 8 * 60 * 60 * 10; ++i)
            {
                double time = i * 0.1;
                double frame = time * rate;
                fit.observe(frame, 100000 + time);
                if (fit.ready())
                    max_error =
                        std::max(max_error, std::abs(fit.time_of(frame + 480) - 100000 - time - 480 / rate));
            }
            require(fit.ready() && max_error < 0.00001, "Clock drift accumulated");
        }
    });
    test("Clock rejects reset, non-finite and timestamp outlier", [] {
        ClockFit fit;
        for (int i = 0; i < 32; ++i)
            fit.observe(i * 480, 10 + i * .01);
        require(fit.ready(), "Clock did not converge");
        require(!fit.observe(16000, 99), "Timestamp jump accepted");
        require(!fit.ready(), "Outlier did not clear estimate");
        require(!fit.observe(1, std::numeric_limits<double>::quiet_NaN()), "NaN timestamp accepted");
    });
    test("Clock converges despite timestamp quantization", [] {
        ClockFit fit;
        double worst = 0;
        for (int i = 0; i < 10000; ++i)
        {
            double t = i * 0.01;
            double frames = t * 48000 * 1.0004;
            double jitter = (i % 3 - 1) * 0.0001;
            fit.observe(frames, 1000 + t + jitter);
            if (i > 100)
            {
                require(fit.ready(), "Quantization prevented clock lock");
                worst = std::max(worst,
                                 std::abs(fit.time_of(frames + 480) - (1000 + t + 480 / (48000 * 1.0004))));
            }
        }
        require(worst < 0.00025, "Noisy clock prediction exceeded measurement budget");
    });
    test("Acoustic correlator finds delayed attenuated signal", [] {
        auto ref = calibration_signal(71);
        std::vector<float> rec(24000);
        constexpr size_t lag = 6123;
        uint32_t s = 3;
        for (auto &v : rec)
        {
            s = 1664525 * s + 1013904223;
            v = static_cast<float>((static_cast<int>(s >> 16) - 32768) * 0.0000001);
        }
        for (size_t i = 0; i < ref.size(); ++i)
            rec[lag + i] += ref[i] * 0.4f;
        auto r = correlate(rec, ref, 100, 15000);
        require(r.valid && std::abs(r.lag_frames - lag) < 0.1, "Delay measurement wrong");
    });
    test("Silence and ambiguous echoes cannot pass measurement", [] {
        auto ref = calibration_signal();
        std::vector<float> rec(24000);
        require(!correlate(rec, ref, 100, 16000).valid, "Silence certified");
        for (size_t i = 0; i < ref.size(); ++i)
        {
            rec[1000 + i] += ref[i];
            rec[12000 + i] += ref[i];
        }
        require(!correlate(rec, ref, 100, 16000).valid, "Ambiguous arrival certified");
    });
    test("One millisecond acceptance includes measurement uncertainty", [] {
        std::array<double, 3> a{0, .4, .7}, u{.05, .05, .05};
        require(verify_alignment(a, u).passed, "Good alignment rejected");
        a[2] = 1.2;
        require(!verify_alignment(a, u).passed, "Bad alignment certified");
        a[2] = .9;
        u[0] = .1;
        require(!verify_alignment(a, u).passed, "Uncertainty omitted");
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
