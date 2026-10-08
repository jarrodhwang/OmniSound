#include <omni/router.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace omni
{
namespace
{
struct Measurement
{
    double offset_ms{}, uncertainty_ms{}, coefficient{};
};
class Rig
{
    CaptureStream microphone_;
    std::vector<std::unique_ptr<RenderStream>> speakers_;
    ClockFit mic_clock_;
    std::atomic<bool> &cancel_;
    std::vector<float> recording_;
    uint64_t recording_base_{};
    bool have_base_{}, bad_recording_{};
    double last_capture_{};

  public:
    Rig(const Group &group, const std::string &microphone, std::atomic<bool> &cancel)
        : microphone_(microphone, false), cancel_(cancel), recording_(48000 * 4)
    {
        for (const auto &m : group.members)
        {
            auto s = std::make_unique<RenderStream>(m.endpoint_id);
            s->start();
            speakers_.push_back(std::move(s));
        }
        microphone_.start();
        auto end = qpc_seconds() + 1.0;
        while (qpc_seconds() < end)
            pump(-1, 0, {});
        for (auto &s : speakers_)
            if (!s->clock_ready())
                throw std::runtime_error("An output clock could not be measured reliably");
        if (!mic_clock_.ready())
            throw std::runtime_error("Microphone clock is unavailable");
    }
    void pump(int active, double scheduled, std::span<const float> signal)
    {
        if (cancel_)
            throw std::runtime_error("Calibration cancelled");
        HANDLE events[max_members + 1];
        DWORD count = 1;
        events[0] = microphone_.event();
        for (auto &s : speakers_)
            events[count++] = s->event();
        WaitForMultipleObjects(count, events, FALSE, 30);
        CapturePacket p;
        while (microphone_.next(p))
        {
            if (p.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                bad_recording_ = true;
            if ((p.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && have_base_)
                bad_recording_ = true;
            if (!(p.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))
                mic_clock_.observe(static_cast<double>(p.position), p.time);
            if (!have_base_)
            {
                recording_base_ = p.position;
                have_base_ = true;
            }
            if (p.position < recording_base_)
                bad_recording_ = true;
            else
            {
                auto index = p.position - recording_base_;
                for (UINT32 i = 0; i < p.frames && index + i < recording_.size(); ++i)
                {
                    float v = (p.flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : 0.5f * (p.data[i].l + p.data[i].r);
                    if (std::abs(v) > 0.98)
                        bad_recording_ = true;
                    recording_[static_cast<size_t>(index + i)] = v;
                }
            }
            last_capture_ = p.time;
            microphone_.release(p.frames);
        }
        for (size_t k = 0; k < speakers_.size(); ++k)
        {
            auto &s = *speakers_[k];
            s.update_clock();
            auto n = s.available();
            if (!n)
                continue;
            auto *out = s.acquire(n);
            if (static_cast<int>(k) != active || !s.clock_ready())
            {
                s.release(n, true);
                continue;
            }
            double t = s.next_time();
            for (UINT32 i = 0; i < n; ++i)
            {
                double position = (t + i * s.frame_seconds() - scheduled) * 48000;
                float v = 0;
                if (position >= 0 && position + 1 < signal.size())
                {
                    auto index = static_cast<size_t>(position);
                    float frac = static_cast<float>(position - index);
                    v = signal[index] * (1 - frac) + signal[index + 1] * frac;
                }
                out[i] = {v, v};
            }
            s.release(n);
        }
    }
    Measurement measure(size_t member, double advance_ms, unsigned seed)
    {
        std::fill(recording_.begin(), recording_.end(), 0.0f);
        have_base_ = false;
        bad_recording_ = false;
        auto signal = calibration_signal(seed);
        double target = qpc_seconds() + 1.2;
        double scheduled = target - advance_ms / 1000;
        double end = target + 1.6;
        while (qpc_seconds() < end)
            pump(static_cast<int>(member), scheduled, signal);
        if (bad_recording_ || !have_base_ || !mic_clock_.ready() || last_capture_ < end - 0.2)
            throw std::runtime_error(
                "Microphone recording clipped, dropped packets, or stopped. Lower the volume and retry.");
        double base_time = mic_clock_.time_of(static_cast<double>(recording_base_));
        auto lo = static_cast<size_t>(std::max(0.0, (scheduled - base_time - 0.10) * 48000));
        auto hi = static_cast<size_t>(std::max(0.0, (scheduled - base_time + 1.0) * 48000));
        hi = std::min(hi, recording_.size() - signal.size());
        auto result = correlate(recording_, signal, lo, hi);
        if (!result.valid)
            throw std::runtime_error("Test sound could not be measured with sufficient confidence. Use a "
                                     "quiet room and a fixed wired microphone.");
        double arrival = mic_clock_.time_of(static_cast<double>(recording_base_) + result.lag_frames);
        // Includes microphone transport delay, which is common to every output and cancels relatively.
        return {(arrival - target) * 1000, result.uncertainty_frames / 48.0 + 0.05, result.coefficient};
    }
};
} // namespace
Json calibrate_group(const Group &group, const std::string &microphone, std::atomic<bool> &cancel,
                     const std::function<void(const std::string &)> &progress)
{
    if (group.members.size() < 2)
        throw std::runtime_error("Automatic alignment requires at least two outputs");
    auto mic = describe_endpoint(microphone);
    if (!mic.capture || mic.bluetooth || !mic.active)
        throw std::runtime_error("Choose an active PC or USB microphone, not a Bluetooth microphone");
    std::vector<Endpoint> devices;
    for (const auto &m : group.members)
    {
        auto d = describe_endpoint(m.endpoint_id);
        if (!d.active || d.capture || !d.group_id.empty())
            throw std::runtime_error("All group outputs must be connected before calibration");
        devices.push_back(d);
    }
    DeviceWatcher watcher;
    auto revision = watcher.revision();
    Rig rig(group, microphone, cancel);
    auto updated = group;
    std::vector<double> offsets(group.members.size()), uncertainty(group.members.size());
    for (size_t i = 0; i < group.members.size(); ++i)
    {
        std::array<double, 3> values{};
        double u = 0;
        for (unsigned repeat = 0; repeat < 3; ++repeat)
        {
            progress("Measuring " + std::to_string(i + 1) + "/" + std::to_string(group.members.size()) +
                     " · " + std::to_string(repeat + 1) + "/3");
            auto m = rig.measure(i, 0, static_cast<unsigned>(i + 1) * 101 + repeat);
            values[repeat] = m.offset_ms;
            u = std::max(u, m.uncertainty_ms);
        }
        std::sort(values.begin(), values.end());
        offsets[i] = values[1];
        uncertainty[i] = u + (values[2] - values[0]) / 2;
        if (offsets[i] < -100 || offsets[i] > 1000 || uncertainty[i] > 0.25)
            throw std::runtime_error("Output latency is unstable or measurement uncertainty exceeds 0.25 ms");
    }
    double worst_spread = 0, worst_uncertainty = 0;
    bool passed = true;
    for (unsigned repeat = 0; repeat < 3; ++repeat)
    {
        std::vector<double> residuals, errors;
        for (size_t i = 0; i < group.members.size(); ++i)
        {
            progress("Verifying " + std::to_string(repeat + 1) + "/3 · " + std::to_string(i + 1) + "/" +
                     std::to_string(group.members.size()));
            auto m = rig.measure(i, offsets[i], static_cast<unsigned>(i + 1) * 701 + repeat);
            residuals.push_back(m.offset_ms);
            errors.push_back(m.uncertainty_ms + uncertainty[i]);
        }
        auto v = verify_alignment(residuals, errors);
        passed &= v.passed;
        worst_spread = std::max(worst_spread, v.spread_ms);
        worst_uncertainty = std::max(worst_uncertainty, v.uncertainty_ms);
    }
    if (watcher.revision() != revision)
        throw std::runtime_error("Audio devices changed during calibration; no result was saved");
    for (size_t i = 0; i < group.members.size(); ++i)
    {
        if (describe_endpoint(group.members[i].endpoint_id).fingerprint != devices[i].fingerprint)
            throw std::runtime_error("Audio format changed during calibration");
        updated.members[i].delay_ms = 0;
        updated.members[i].calibration = Calibration{offsets[i], worst_uncertainty,      worst_spread,
                                                     utc_now(),  devices[i].fingerprint, microphone,
                                                     passed};
    }
    return {
        {"group", updated},
        {"passed", passed},
        {"maxSpreadMs", worst_spread},
        {"uncertaintyMs", worst_uncertainty},
        {"note",
         "Acoustic measurement at the microphone position; continuous Bluetooth timing is not verified."}};
}
} // namespace omni
