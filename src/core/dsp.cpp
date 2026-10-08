#include <omni/dsp.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <complex>
#include <stdexcept>
#include <memory>
namespace omni
{
Timeline::Timeline(size_t n) : data_(n), tags_(n, std::numeric_limits<int64_t>::min())
{
    if (n < 64)
        throw std::invalid_argument("Timeline too short");
}
void Timeline::clear() noexcept
{
    std::fill(tags_.begin(), tags_.end(), std::numeric_limits<int64_t>::min());
}
void Timeline::put(int64_t first, std::span<const Frame> in) noexcept
{
    for (size_t i = 0; i < in.size(); ++i)
    {
        auto f = first + static_cast<int64_t>(i);
        if (f < 0)
            continue;
        auto p = static_cast<size_t>(f) % data_.size();
        data_[p] = in[i];
        tags_[p] = f;
    }
}
void Timeline::silence(int64_t first, size_t n) noexcept
{
    for (size_t i = 0; i < n; ++i)
    {
        auto f = first + static_cast<int64_t>(i);
        if (f < 0)
            continue;
        auto p = static_cast<size_t>(f) % data_.size();
        data_[p] = {};
        tags_[p] = f;
    }
}
bool Timeline::get(int64_t f, Frame &out) const noexcept
{
    if (f < 0)
        return false;
    auto p = static_cast<size_t>(f) % data_.size();
    if (tags_[p] != f)
        return false;
    out = data_[p];
    return true;
}
SincSampler::SincSampler() : kernel_(kernel()) {}
const SincSampler::Kernel &SincSampler::kernel()
{
    // Shared immutable table is initialized before streaming starts. Keep the
    // large filter off the audio thread's stack and out of per-group copies.
    static const auto table = [] {
        auto values = std::make_unique<Kernel>();
        for (int p = 0; p <= phases; ++p)
        {
            double total = 0;
            for (int t = 0; t < taps; ++t)
            {
                double x = t - left_guard - static_cast<double>(p) / phases;
                double sinc =
                    std::abs(x) < 1e-10 ? 1 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                double w = 0.42 - 0.5 * std::cos(2 * std::numbers::pi * t / (taps - 1)) +
                           0.08 * std::cos(4 * std::numbers::pi * t / (taps - 1));
                (*values)[p][t] = static_cast<float>(sinc * w);
                total += (*values)[p][t];
            }
            for (auto &v : (*values)[p])
                v = static_cast<float>(v / total);
        }
        return values;
    }();
    return *table;
}
bool SincSampler::sample(const Timeline &ring, double pos, Frame &out) const noexcept
{
    out = {};
    if (!std::isfinite(pos) || pos < 0 || pos > 9e15)
        return false;
    auto base = static_cast<int64_t>(std::floor(pos));
    if (pos == static_cast<double>(base))
        return ring.get(base, out); // Exact PCM needs no interpolation guards.
    if (pos < left_guard)
        return false;
    double phase = (pos - base) * phases;
    int p = std::min(phases - 1, static_cast<int>(phase));
    double fraction = phase - p;
    double left = 0, right = 0;
    for (int t = 0; t < taps; ++t)
    {
        double weight = kernel_[p][t] + fraction * (kernel_[p + 1][t] - kernel_[p][t]);
        if (std::abs(weight) < 1e-10)
            continue; // Blackman endpoints are exactly zero.
        Frame v;
        if (!ring.get(base + t - left_guard, v))
        {
            out = {};
            return false;
        }
        left += v.l * weight;
        right += v.r * weight;
    }
    out = {static_cast<float>(left), static_cast<float>(right)};
    return true;
}
void ContinuousRenderer::reset() noexcept
{
    valid_ = false;
}
void ContinuousRenderer::silence(std::span<Frame> output) noexcept
{
    if (!tail_ && (last_.l != 0 || last_.r != 0))
        tail_ = 128;
    for (auto &out : output)
    {
        float envelope = tail_ ? static_cast<float>(--tail_) / 128 : 0;
        out = {last_.l * envelope, last_.r * envelope};
    }
    if (!tail_)
        last_ = {};
    fade_ = 0;
    valid_ = false;
}
RenderResult ContinuousRenderer::render(const Timeline &timeline, const SincSampler &sampler,
                                        std::span<Frame> output, double desired, double ratio,
                                        Frame target_gain) noexcept
{
    RenderResult result;
    if (!std::isfinite(desired) || !std::isfinite(ratio) || ratio < .99 || ratio > 1.01)
    {
        desired = cursor_;
        ratio = ratio_;
        valid_ = false;
    }
    if (!valid_ || std::abs(desired - cursor_) > 480)
    {
        result.resynchronized = valid_;
        cursor_ = desired;
        fade_ = 0;
        if (tail_)
        {
            float remaining = static_cast<float>(tail_) / 128;
            last_ = {last_.l * remaining, last_.r * remaining};
        }
        tail_ = 128;
        valid_ = true;
    }
    // A long controller time constant rejects packet/QPC jitter rather than
    // frequency-modulating the programme on every small output event.
    const double correction = std::clamp((desired - cursor_) / (48000 * 2), -.0002, .0002);
    for (auto &out : output)
    {
        ratio_ += (ratio - ratio_) / 240000;
        correction_ += (correction - correction_) / 96000;
        gain_.l += (target_gain.l - gain_.l) / 240;
        gain_.r += (target_gain.r - gain_.r) / 240;
        Frame value;
        bool present = sampler.sample(timeline, cursor_, value);
        cursor_ += ratio_ + correction_;
        if (!present)
        {
            ++result.missing_frames;
            if (fade_ > 0)
                tail_ = 128;
            fade_ = 0;
        }
        if (tail_)
        {
            // Fade the previous output to zero, even when the old timeline has
            // been cleared. Holding it for <=2.7 ms prevents a one-sample edge.
            float envelope = static_cast<float>(--tail_) / 128;
            out = {last_.l * envelope, last_.r * envelope};
            if (!tail_)
                last_ = {};
            continue;
        }
        if (present)
            fade_ = std::min(1.0, fade_ + 1.0 / 128);
        auto finite_sample = [&](float sample) {
            if (!std::isfinite(sample))
            {
                ++result.invalid_samples;
                return 0.f;
            }
            if (std::abs(sample) > 1)
                ++result.headroom_samples;
            return sample;
        };
        out = {finite_sample(static_cast<float>(value.l * fade_) * gain_.l),
               finite_sample(static_cast<float>(value.r * fade_) * gain_.r)};
        last_ = out;
    }
    return result;
}
void ClockFit::reset() noexcept
{
    count_ = next_ = 0;
    slope_ = 1.0 / 48000;
    anchor_x_ = anchor_y_ = last_x_ = last_y_ = 0;
}
bool ClockFit::observe(double x, double y) noexcept
{
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    if (count_ && (x < last_x_ || y < last_y_))
    {
        reset();
        return false;
    }
    if (count_ && x == last_x_)
        return false;
    if (ready() && std::abs(y - time_of(x)) > 0.020)
    {
        reset();
        return false;
    }
    x_[next_] = x;
    y_[next_] = y;
    next_ = (next_ + 1) % x_.size();
    count_ = std::min(count_ + 1, x_.size());
    last_x_ = x;
    last_y_ = y;
    double mx = 0, my = 0;
    for (size_t i = 0; i < count_; ++i)
    {
        mx += x_[i] - x;
        my += y_[i] - y;
    }
    mx = mx / count_ + x;
    my = my / count_ + y;
    double xx = 0, xy = 0;
    for (size_t i = 0; i < count_; ++i)
    {
        xx += (x_[i] - mx) * (x_[i] - mx);
        xy += (x_[i] - mx) * (y_[i] - my);
    }
    if (xx > 1 && count_ >= 8)
    {
        double slope = xy / xx;
        if (slope < 0.995 / 48000 || slope > 1.005 / 48000)
        {
            reset();
            return false;
        }
        slope_ = slope;
    }
    anchor_x_ = mx;
    anchor_y_ = my;
    return true;
}
double ClockFit::time_of(double x) const noexcept
{
    return anchor_y_ + (x - anchor_x_) * slope_;
}
std::vector<float> calibration_signal(unsigned seed)
{
    std::vector<float> out(4096);
    uint32_t s = seed ? seed : 1;
    float prev = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        float n = (s & 1) ? 1.f : -1.f;
        double envelope =
            std::min({1.0, static_cast<double>(i) / 128, static_cast<double>(out.size() - 1 - i) / 128});
        out[i] = static_cast<float>((n - prev) * 0.06 * envelope);
        prev = n;
    }
    return out;
}
static void fft(std::vector<std::complex<double>> &a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        double angle = (inverse ? 2 : -2) * std::numbers::pi / len;
        std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j)
            {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto &v : a)
            v /= static_cast<double>(n);
}
CorrelationResult correlate(std::span<const float> rec, std::span<const float> ref, size_t lo, size_t hi)
{
    CorrelationResult result;
    if (ref.empty() || rec.size() < ref.size() || lo > hi || hi > rec.size() - ref.size())
        return result;
    size_t n = 1;
    while (n < rec.size() + ref.size())
        n <<= 1;
    std::vector<std::complex<double>> a(n), b(n);
    double refEnergy = 0;
    std::vector<double> energy(rec.size() + 1);
    for (size_t i = 0; i < rec.size(); ++i)
    {
        if (!std::isfinite(rec[i]))
            return result;
        a[i] = rec[i];
        energy[i + 1] = energy[i] + rec[i] * static_cast<double>(rec[i]);
    }
    for (size_t i = 0; i < ref.size(); ++i)
    {
        b[i] = ref[i];
        refEnergy += ref[i] * static_cast<double>(ref[i]);
    }
    if (refEnergy < 1e-12)
        return result;
    fft(a, false);
    fft(b, false);
    for (size_t i = 0; i < n; ++i)
        a[i] *= std::conj(b[i]);
    fft(a, true);
    auto score = [&](size_t i) {
        double e = energy[i + ref.size()] - energy[i];
        return e > 1e-12 ? std::abs(a[i].real()) / std::sqrt(e * refEnergy) : 0.0;
    };
    size_t best = lo;
    double peak = 0;
    for (size_t i = lo; i <= hi; ++i)
    {
        double c = score(i);
        if (c > peak)
        {
            peak = c;
            best = i;
        }
    }
    double second = 0;
    for (size_t i = lo; i <= hi; ++i)
        if (std::abs(static_cast<int64_t>(i) - static_cast<int64_t>(best)) > 48)
            second = std::max(second, score(i));
    double fraction = 0;
    if (best > lo && best < hi)
    {
        double l = score(best - 1), r = score(best + 1), d = l - 2 * peak + r;
        if (std::abs(d) > 1e-9)
            fraction = std::clamp(0.5 * (l - r) / d, -0.5, 0.5);
    }
    result.lag_frames = static_cast<double>(best) + fraction;
    result.coefficient = peak;
    result.uncertainty_frames = peak > 0 ? std::max(0.5, 4 * (1 - peak) / peak) : 48000;
    result.valid =
        peak >= 0.20 && peak > second * 1.8 && best > lo && best < hi && result.uncertainty_frames <= 12;
    return result;
}
Verification verify_alignment(std::span<const double> arrivals, std::span<const double> uncertainty)
{
    Verification v;
    if (arrivals.size() < 2 || arrivals.size() != uncertainty.size())
        return v;
    for (size_t i = 0; i < arrivals.size(); ++i)
    {
        if (!std::isfinite(arrivals[i]) || !std::isfinite(uncertainty[i]) || uncertainty[i] < 0)
            return v;
        v.uncertainty_ms = std::max(v.uncertainty_ms, uncertainty[i]);
    }
    auto [lo, hi] = std::minmax_element(arrivals.begin(), arrivals.end());
    v.spread_ms = *hi - *lo;
    v.passed = v.spread_ms + 2 * v.uncertainty_ms <= 1.0 && v.uncertainty_ms <= 0.25;
    return v;
}
} // namespace omni
