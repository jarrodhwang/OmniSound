#include <omni/signal_quality.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
namespace omni
{
ToneFit fit_tone(std::span<const Frame> audio, int channel, double frequency, double ppm)
{
    if (audio.size() < 32 || channel < 0 || channel > 1 || !std::isfinite(frequency) || frequency < 1 ||
        frequency > 23999 || !std::isfinite(ppm) || std::abs(ppm) > 10000)
        throw std::invalid_argument("Invalid tone analysis window");
    double mean = 0;
    for (auto v : audio)
    {
        if (!std::isfinite(v.l) || !std::isfinite(v.r))
            throw std::invalid_argument("Nonfinite captured audio");
        mean += channel ? v.r : v.l;
    }
    const double n = static_cast<double>(audio.size());
    mean /= n;
    const double angle = 2 * std::numbers::pi * frequency * (1 + ppm / 1e6) / 48000;
    const double cs = std::cos(angle), sn = std::sin(angle);
    double s = 0, c = 1, ss = 0, cc = 0, sc = 0, vs = 0, vc = 0, sum_s = 0, sum_c = 0;
    for (auto v : audio)
    {
        double x = (channel ? v.r : v.l) - mean;
        ss += s * s;
        cc += c * c;
        sc += s * c;
        vs += x * s;
        vc += x * c;
        sum_s += s;
        sum_c += c;
        double next = s * cs + c * sn;
        c = c * cs - s * sn;
        s = next;
    }
    ss -= sum_s * sum_s / n;
    cc -= sum_c * sum_c / n;
    sc -= sum_s * sum_c / n;
    double determinant = ss * cc - sc * sc;
    if (!std::isfinite(determinant) || determinant < 1e-16)
        throw std::invalid_argument("Tone analysis window is ill-conditioned");
    double a = (vs * cc - vc * sc) / determinant;
    double b = (vc * ss - vs * sc) / determinant;
    double dc = mean - (a * sum_s + b * sum_c) / n;
    ToneFit result{};
    result.amplitude = std::hypot(a, b);
    result.ppm = ppm;
    s = 0;
    c = 1;
    for (auto v : audio)
    {
        double model = a * s + b * c;
        double residual = (channel ? v.r : v.l) - dc - model;
        result.signal += model * model;
        result.residual += residual * residual;
        result.peak = std::max(result.peak, std::abs(residual));
        double next = s * cs + c * sn;
        c = c * cs - s * sn;
        s = next;
    }
    return result;
}
} // namespace omni
