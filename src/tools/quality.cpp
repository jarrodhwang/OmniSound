#include "quality.hpp"
#include <omni/ipc.hpp>
#include <omni/signal_quality.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>

namespace
{
using namespace omni;
constexpr double frequencies[3][2] = {{997, 1499}, {9973, 12011}, {18013, 19001}};
struct Recording
{
    std::string id;
    std::unique_ptr<CaptureStream> capture;
    std::vector<Frame> audio;
    uint64_t first{}, end{};
    double time{};
    unsigned discontinuities{}, timestamp_errors{};
    bool initialized{};
    void drain()
    {
        CapturePacket p;
        while (capture->next(p))
        {
            if (!initialized)
            {
                first = p.position;
                time = p.time;
                initialized = true;
            }
            discontinuities += (p.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            timestamp_errors += (p.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            if (p.position >= first && p.position - first < audio.size())
            {
                auto offset = static_cast<size_t>(p.position - first);
                auto n = std::min(static_cast<size_t>(p.frames), audio.size() - offset);
                if (!(p.flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    std::copy_n(p.data, n, audio.data() + offset);
                end = std::max(end, offset + n);
            }
            capture->release(p.frames);
        }
    }
};
Json analyse(const Recording &rec, double started, double seconds)
{
    Json bands = Json::array();
    for (int band = 0; band < 3; ++band)
    {
        std::vector<double> snrs[2], amplitudes[2];
        double worst_peak = 0, max_ppm = 0;
        unsigned dropout_windows = 0, windows = 0;
        constexpr size_t window = 4800;
        for (size_t first = 0; first + window <= rec.end; first += window)
        {
            double t = rec.time + static_cast<double>(first) / 48000 - started;
            // Exclude startup/frequency-change fades and fixed device latency.
            const double length = seconds / 3;
            if (t < band * length + 0.5 || t + 0.1 > (band + 1) * length - 0.25)
                continue;
            ++windows;
            bool dropout = false;
            for (int channel = 0; channel < 2; ++channel)
            {
                auto block = std::span(rec.audio).subspan(first, window);
                ToneFit best = fit_tone(block, channel, frequencies[band][channel], 0);
                for (int ppm = -5000; ppm <= 5000; ppm += 100)
                {
                    auto candidate = fit_tone(block, channel, frequencies[band][channel], ppm);
                    if (candidate.residual < best.residual)
                        best = candidate;
                }
                for (double increment : {10.0, 1.0, 0.1})
                {
                    const double centre = best.ppm;
                    for (int i = -10; i <= 10; ++i)
                    {
                        auto candidate =
                            fit_tone(block, channel, frequencies[band][channel], centre + i * increment);
                        if (candidate.residual < best.residual)
                            best = candidate;
                    }
                }
                dropout |= best.amplitude < 0.0001;
                double snr = best.signal > 1e-12
                                 ? 10 * std::log10(best.signal / std::max(best.residual, 1e-24))
                                 : -120;
                snrs[channel].push_back(snr);
                amplitudes[channel].push_back(best.amplitude);
                worst_peak = std::max(worst_peak, best.peak / std::max(best.amplitude, 1e-6));
                max_ppm = std::max(max_ppm, std::abs(best.ppm));
            }
            dropout_windows += dropout;
        }
        Json channels = Json::array();
        for (int channel = 0; channel < 2; ++channel)
        {
            std::sort(snrs[channel].begin(), snrs[channel].end());
            std::sort(amplitudes[channel].begin(), amplitudes[channel].end());
            channels.push_back(
                {{"frequencyHz", frequencies[band][channel]},
                 {"medianSignalToResidualDb",
                  snrs[channel].empty() ? -120 : snrs[channel][snrs[channel].size() / 2]},
                 {"worstSignalToResidualDb", snrs[channel].empty() ? -120 : snrs[channel].front()},
                 {"medianAmplitude",
                  amplitudes[channel].empty() ? 0 : amplitudes[channel][amplitudes[channel].size() / 2]}});
        }
        bands.push_back({{"channels", channels},
                         {"windows", windows},
                         {"dropoutWindows", dropout_windows},
                         {"maxResidualPeakToTone", worst_peak},
                         {"maxFittedRateDeviationPpm", max_ppm}});
    }
    return {{"endpointId", rec.id},
            {"name", describe_endpoint(rec.id).name},
            {"bands", bands},
            {"captureDiscontinuitiesIncludingStart", rec.discontinuities},
            {"captureTimestampErrors", rec.timestamp_errors}};
}
} // namespace

omni::Json check_audio_quality(bool raw_reference, double seconds)
{
    using namespace omni;
    if (!std::isfinite(seconds) || seconds < 6 || seconds > 60)
        throw std::invalid_argument("Quality check duration must be 6 to 60 seconds");
    const auto source = default_render_endpoint();
    const auto config = request(engine_pipe(true), {{"op", "load"}}).at("config").get<Config>();
    const auto before = request(engine_pipe(true), {{"op", "status"}});
    std::vector<Recording> recordings;
    std::vector<std::string> ids{source};
    for (const auto &group : config.groups)
        if (group.id == active_group_id(config))
            for (const auto &member : lite_mirror_targets(group, source).members)
                if (describe_endpoint(member.endpoint_id).active)
                    ids.push_back(member.endpoint_id);
    if (ids.size() < 2)
        throw std::runtime_error("Select an active Lite group with additional outputs first");
    for (const auto &id : ids)
    {
        Recording rec;
        rec.id = id;
        rec.audio.resize(static_cast<size_t>((seconds + 3) * 48000));
        rec.capture = std::make_unique<CaptureStream>(id, true);
        rec.capture->start();
        recordings.push_back(std::move(rec));
    }
    std::atomic<double> started{};
    std::atomic<bool> cancelled{}, raw_active{};
    std::atomic<uint64_t> frames_rendered{};
    std::exception_ptr error;
    std::jthread tone([&] {
        try
        {
            ComScope com;
            RenderStream render(source, raw_reference);
            raw_active = render.raw();
            render.start();
            AudioPriority priority;
            started = qpc_seconds();
            uint64_t frame = 0;
            const double length = seconds / 3;
            while (frame < static_cast<uint64_t>(seconds * 48000) && !cancelled)
            {
                if (WaitForSingleObject(render.event(), 100) != WAIT_OBJECT_0)
                    continue;
                auto n = render.available();
                if (!n)
                    continue;
                auto *out = render.acquire(n);
                for (UINT32 i = 0; i < n; ++i, ++frame)
                {
                    double time = static_cast<double>(frame) / 48000;
                    int band = std::min(2, static_cast<int>(time / length));
                    double local = time - band * length;
                    double envelope = std::max(0.0, std::min({1.0, local / .02, (length - local) / .02}));
                    // Low-level, uncompressed float PCM; independent stereo bands, no volume changes.
                    float amplitude = static_cast<float>((band ? .015 : .05) * envelope);
                    out[i] = {amplitude * static_cast<float>(
                                              std::sin(2 * std::numbers::pi * frequencies[band][0] * time)),
                              amplitude * static_cast<float>(
                                              std::sin(2 * std::numbers::pi * frequencies[band][1] * time))};
                }
                render.release(n);
                frames_rendered = frame;
            }
            Sleep(100); // Let the final queued source frames drain.
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });
    {
        AudioPriority priority;
        double end = qpc_seconds() + seconds + 1;
        while (qpc_seconds() < end)
        {
            HANDLE events[max_members + 1];
            for (size_t i = 0; i < recordings.size(); ++i)
                events[i] = recordings[i].capture->event();
            WaitForMultipleObjects(static_cast<DWORD>(recordings.size()), events, FALSE, 20);
            for (auto &rec : recordings)
                rec.drain();
        }
    }
    cancelled = true;
    tone.join();
    if (error)
        std::rethrow_exception(error);
    if (frames_rendered < static_cast<uint64_t>(seconds * 48000))
        throw std::runtime_error("Quality source did not finish rendering before the capture deadline");
    const auto after = request(engine_pipe(true), {{"op", "status"}});
    Json results = Json::array();
    for (const auto &rec : recordings)
        results.push_back(analyse(rec, started, seconds));
    return {{"format", "48000 Hz stereo float32 PCM"},
            {"durationSeconds", seconds},
            {"rawReferenceRequested", raw_reference},
            {"rawReferenceActive", raw_active.load()},
            {"referenceEndpointId", source},
            {"recordingsSaved", false},
            {"acousticMeasurement", false},
            {"requiresQuietOtherApplications", true},
            {"before", before},
            {"after", after},
            {"outputs", results}};
}
