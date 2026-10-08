#pragma once
#include <omni/dsp.hpp>
namespace omni
{
struct ToneFit
{
    double residual{}, signal{}, amplitude{}, peak{}, ppm{};
};
// Offline diagnostic regression: independent DC, sine and cosine terms.
ToneFit fit_tone(std::span<const Frame>, int channel, double frequency, double ppm = 0);
} // namespace omni
