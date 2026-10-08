#pragma once
#include <vector>
#include <span>
#include <array>
#include <cstdint>
namespace omni
{
struct Frame
{
    float l{}, r{};
};
// Single-owner, bounded timeline. Tags prevent old audio leaking through gaps or wraparound.
class Timeline
{
  public:
    explicit Timeline(size_t frames = 48000 * 5);
    void put(int64_t first, std::span<const Frame> audio) noexcept;
    void silence(int64_t first, size_t count) noexcept;
    bool get(int64_t frame, Frame &out) const noexcept;
    void clear() noexcept;

  private:
    std::vector<Frame> data_;
    std::vector<int64_t> tags_;
};
class SincSampler
{
  public:
    static constexpr int left_guard = 63, right_guard = 64;
    SincSampler();
    bool sample(const Timeline &, double position, Frame &) const noexcept;

  private:
    static constexpr int taps = 128, phases = 1024;
    using Kernel = std::array<std::array<float, taps>, phases + 1>;
    static const Kernel &kernel();
    const Kernel &kernel_;
};
struct RenderResult
{
    size_t missing_frames{}, headroom_samples{}, invalid_samples{};
    bool resynchronized{};
};
// Audio-thread owned. Smooth clock/gain changes and fade across missing packets;
// never clip float headroom before the Windows endpoint's master attenuation.
class ContinuousRenderer
{
  public:
    RenderResult render(const Timeline &, const SincSampler &, std::span<Frame>, double desired_position,
                        double clock_ratio, Frame gain) noexcept;
    void reset() noexcept;
    void silence(std::span<Frame>) noexcept;
    double position() const noexcept
    {
        return cursor_;
    }
    bool ready() const noexcept
    {
        return valid_;
    }

  private:
    double cursor_{}, ratio_{1}, correction_{}, fade_{};
    Frame gain_{1, 1}, last_{};
    unsigned tail_{};
    bool valid_{};
};
// Fits device frames to monotonic QPC seconds, rejecting outliers and clock resets.
class ClockFit
{
  public:
    bool observe(double frame, double qpc) noexcept;
    double time_of(double frame) const noexcept;
    double seconds_per_frame() const noexcept
    {
        return slope_;
    }
    bool ready() const noexcept
    {
        return count_ >= 8;
    }
    bool has_position() const noexcept
    {
        return count_ != 0;
    }
    void reset() noexcept;

  private:
    std::array<double, 64> x_{}, y_{};
    size_t count_{}, next_{};
    double slope_{1.0 / 48000}, anchor_x_{}, anchor_y_{}, last_x_{}, last_y_{};
};
struct CorrelationResult
{
    bool valid{};
    double lag_frames{}, coefficient{}, uncertainty_frames{};
};
std::vector<float> calibration_signal(unsigned seed = 1);
CorrelationResult correlate(std::span<const float> recorded, std::span<const float> reference, size_t min_lag,
                            size_t max_lag);
struct Verification
{
    bool passed{};
    double spread_ms{}, uncertainty_ms{};
};
Verification verify_alignment(std::span<const double> arrivals_ms, std::span<const double> uncertainty_ms);
} // namespace omni
