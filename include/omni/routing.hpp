#pragma once
#include <omni/model.hpp>
#include <omni/dsp.hpp>
#include <array>
namespace omni
{
// Canonical PCM order: FL, FR, FC, LFE, BL, BR, SL, SR (Windows 7.1).
// A 5.1 side mask is mapped to SL/SR rather than silently treated as rear.
struct RoutingMatrix
{
    std::array<float, 8> left{}, right{};
    Frame apply(const std::array<float, 8> &) const noexcept;
};
RoutingMatrix routing_matrix(const Member &, unsigned channels, uint32_t mask);
std::array<float, 8> canonical_channels(const float *, unsigned channels, uint32_t mask) noexcept;
bool same_stream_topology(const Group &, const Group &);
} // namespace omni
