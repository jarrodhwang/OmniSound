#include <omni/routing.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
namespace omni
{
namespace
{
constexpr std::array<uint32_t, 8> bits{1, 2, 4, 8, 16, 32, 512, 1024};
constexpr std::array<double, 8> angles{-30, 30, 0, 0, -150, 150, -90, 90};
void normalize(std::array<float, 8> &row)
{
    double sum = 0;
    for (auto v : row)
        sum += std::abs(v);
    if (sum > 1)
        for (auto &v : row)
            v = static_cast<float>(v / sum);
}
} // namespace
Frame RoutingMatrix::apply(const std::array<float, 8> &source) const noexcept
{
    double l = 0, r = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        const double v = std::isfinite(source[i]) ? source[i] : 0;
        l += v * left[i];
        r += v * right[i];
    }
    return {static_cast<float>(l), static_cast<float>(r)};
}
std::array<float, 8> canonical_channels(const float *source, unsigned channels, uint32_t mask) noexcept
{
    std::array<float, 8> result{};
    if (!source || channels > 8)
        return result;
    unsigned input = 0;
    for (unsigned bit = 0; bit < 32 && input < channels; ++bit)
        if (mask & (1u << bit))
        {
            for (size_t c = 0; c < bits.size(); ++c)
                if (bits[c] == (1u << bit))
                    result[c] = source[input];
            ++input;
        }
    return result;
}
RoutingMatrix routing_matrix(const Member &member, unsigned channels, uint32_t mask)
{
    if (channels != 2 && channels != 6 && channels != 8)
        throw std::invalid_argument("Pro input must be stereo, 5.1 or 7.1 PCM");
    RoutingMatrix matrix;
    static const std::array<const char *, 8> routes{"front-left", "front-right", "center",    "lfe",
                                                    "back-left",  "back-right",  "side-left", "side-right"};
    for (size_t c = 0; c < routes.size(); ++c)
        if (member.routing == routes[c])
        {
            if (mask & bits[c])
                matrix.left[c] = matrix.right[c] = 1;
            return matrix;
        }
    if (member.routing == "position")
    {
        for (size_t c = 0; c < 8; ++c)
            if ((mask & bits[c]) && c != 3)
            {
                const double separation = std::remainder(member.azimuth_deg - angles[c], 360.0);
                matrix.left[c] =
                    static_cast<float>(std::max(0.0, std::cos(separation * std::numbers::pi / 180)));
            }
        normalize(matrix.left);
        const float attenuation = static_cast<float>(1 / std::max(1.0, member.distance_m));
        for (auto &v : matrix.left)
            v *= attenuation;
        matrix.right = matrix.left;
        return matrix;
    }
    // Stereo is transparent for stereo input. Surround downmix deliberately
    // excludes LFE: bass management is not inferred from an unknown speaker.
    matrix.left[0] = matrix.right[1] = 1;
    if (channels > 2)
    {
        matrix.left[2] = matrix.right[2] = .70710678f;
        matrix.left[4] = matrix.right[5] = .70710678f;
        matrix.left[6] = matrix.right[7] = .70710678f;
        for (size_t c = 0; c < 8; ++c)
            if (!(mask & bits[c]))
                matrix.left[c] = matrix.right[c] = 0;
        normalize(matrix.left);
        normalize(matrix.right);
    }
    if (member.routing == "swap")
        std::swap(matrix.left, matrix.right);
    else if (member.routing == "left")
        matrix.right = matrix.left;
    else if (member.routing == "right")
        matrix.left = matrix.right;
    else if (member.routing == "mono")
    {
        for (size_t c = 0; c < 8; ++c)
            matrix.left[c] = (matrix.left[c] + matrix.right[c]) * .5f;
        matrix.right = matrix.left;
    }
    return matrix;
}
bool same_stream_topology(const Group &a, const Group &b)
{
    if (a.output_processing != b.output_processing || a.members.size() != b.members.size())
        return false;
    for (size_t i = 0; i < a.members.size(); ++i)
        if (a.members[i].endpoint_id != b.members[i].endpoint_id ||
            Json(a.members[i]).value("calibration", Json{}) !=
                Json(b.members[i]).value("calibration", Json{}))
            return false;
    return true;
}
} // namespace omni
