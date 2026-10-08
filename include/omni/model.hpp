#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
namespace omni
{
using Json = nlohmann::json;
inline constexpr int sample_rate = 48000;
inline constexpr unsigned max_groups = 16, max_members = 16;
inline constexpr std::size_t max_message_bytes = 1024 * 1024;
struct Calibration
{
    double acoustic_ms{}, uncertainty_ms{}, verified_spread_ms{};
    std::string measured_at, fingerprint, microphone_id;
    bool verified{};
};
struct Member
{
    std::string endpoint_id;
    double gain{1}, delay_ms{};
    std::optional<Calibration> calibration;
    bool follow_reference_volume{};
    std::string routing{"stereo"};
    double azimuth_deg{}, distance_m{1};
};
struct Group
{
    std::string id, name;
    std::vector<Member> members;
    double safety_ms{};
    std::string output_processing{"raw"};
};
struct Config
{
    int version{1};
    uint64_t revision{};
    std::vector<Group> groups;
    std::string active_group_id;
};
void to_json(Json &, const Calibration &);
void from_json(const Json &, Calibration &);
void to_json(Json &, const Member &);
void from_json(const Json &, Member &);
void to_json(Json &, const Group &);
void from_json(const Json &, Group &);
void to_json(Json &, const Config &);
void from_json(const Json &, Config &);
bool valid_id(const std::string &);
void validate(const Config &);
std::string config_signature(const Group &);
Group lite_mirror_targets(const Group &, const std::string &default_output_id);
double playback_delay_seconds(double target, double minimum, double offset_ms);
std::string active_group_id(const Config &);
} // namespace omni
