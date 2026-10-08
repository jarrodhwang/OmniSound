#include <omni/model.hpp>
#include <cmath>
#include <set>
#include <stdexcept>
#include <algorithm>
namespace omni
{
void to_json(Json &j, const Calibration &v)
{
    j = {{"acousticMs", v.acoustic_ms},
         {"uncertaintyMs", v.uncertainty_ms},
         {"verifiedSpreadMs", v.verified_spread_ms},
         {"measuredAt", v.measured_at},
         {"fingerprint", v.fingerprint},
         {"microphoneId", v.microphone_id},
         {"verified", v.verified}};
}
void from_json(const Json &j, Calibration &v)
{
    j.at("acousticMs").get_to(v.acoustic_ms);
    j.at("uncertaintyMs").get_to(v.uncertainty_ms);
    j.at("verifiedSpreadMs").get_to(v.verified_spread_ms);
    j.at("measuredAt").get_to(v.measured_at);
    j.at("fingerprint").get_to(v.fingerprint);
    j.at("microphoneId").get_to(v.microphone_id);
    j.at("verified").get_to(v.verified);
}
void to_json(Json &j, const Member &v)
{
    j = {{"endpointId", v.endpoint_id},   {"gain", v.gain},
         {"delayMs", v.delay_ms},         {"followReferenceVolume", v.follow_reference_volume},
         {"routing", v.routing},          {"azimuthDegrees", v.azimuth_deg},
         {"distanceMeters", v.distance_m}};
    if (v.calibration)
        j["calibration"] = *v.calibration;
}
void from_json(const Json &j, Member &v)
{
    j.at("endpointId").get_to(v.endpoint_id);
    v.gain = j.value("gain", 1.0);
    v.delay_ms = j.value("delayMs", 0.0);
    v.follow_reference_volume = j.value("followReferenceVolume", false);
    v.routing = j.value("routing", std::string("stereo"));
    v.azimuth_deg = j.value("azimuthDegrees", 0.0);
    v.distance_m = j.value("distanceMeters", 1.0);
    v.calibration.reset();
    if (j.contains("calibration") && !j.at("calibration").is_null())
        v.calibration = j.at("calibration").get<Calibration>();
}
void to_json(Json &j, const Group &v)
{
    j = {{"id", v.id},
         {"name", v.name},
         {"members", v.members},
         {"safetyMs", v.safety_ms},
         {"outputProcessing", v.output_processing}};
}
void from_json(const Json &j, Group &v)
{
    j.at("id").get_to(v.id);
    j.at("name").get_to(v.name);
    j.at("members").get_to(v.members);
    v.safety_ms = j.value("safetyMs", 0.0);
    v.output_processing = j.value("outputProcessing", std::string("raw"));
}
void to_json(Json &j, const Config &v)
{
    j = {{"version", v.version},
         {"revision", v.revision},
         {"groups", v.groups},
         {"activeGroupId", v.active_group_id}};
}
void from_json(const Json &j, Config &v)
{
    j.at("version").get_to(v.version);
    v.revision = j.value("revision", uint64_t{});
    j.at("groups").get_to(v.groups);
    v.active_group_id = j.value("activeGroupId", std::string{});
    validate(v);
}
bool valid_id(const std::string &id)
{
    if (id.size() != 36)
        return false;
    for (size_t i = 0; i < id.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (id[i] != '-')
                return false;
        }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    }
    return true;
}
static bool bounded(double v, double lo, double hi)
{
    return std::isfinite(v) && v >= lo && v <= hi;
}
void validate(const Config &c)
{
    if (!c.active_group_id.empty() && std::none_of(c.groups.begin(), c.groups.end(),
                                                   [&](const Group &g) { return g.id == c.active_group_id; }))
        throw std::invalid_argument("Active group no longer exists");
    if (c.version != 1 || c.groups.size() > max_groups)
        throw std::invalid_argument("Unsupported configuration version or too many groups");
    std::set<std::string> ids, names;
    for (const auto &g : c.groups)
    {
        if (!valid_id(g.id) || !ids.insert(g.id).second)
            throw std::invalid_argument("Invalid or duplicate group ID");
        if (g.name.empty() || g.name.size() > 192 || g.name.find_first_of("\r\n\t") != std::string::npos ||
            std::any_of(g.name.begin(), g.name.end(), [](unsigned char x) { return x < 32; }) ||
            !names.insert(g.name).second)
            throw std::invalid_argument("Invalid or duplicate group name");
        if (g.members.size() > max_members)
            throw std::invalid_argument("Too many output devices");
        if (!bounded(g.safety_ms, 0, 1000))
            throw std::invalid_argument("Safety buffer is out of range");
        if (g.output_processing != "raw" && g.output_processing != "system")
            throw std::invalid_argument("Unknown output processing mode");
        std::set<std::string> members;
        for (const auto &m : g.members)
        {
            if (m.endpoint_id.empty() || m.endpoint_id.size() > 1024 ||
                m.endpoint_id.find('\0') != std::string::npos || !members.insert(m.endpoint_id).second)
                throw std::invalid_argument("Invalid or duplicate endpoint");
            if (!bounded(m.gain, 0, 1) || !bounded(m.delay_ms, 0, 2000))
                throw std::invalid_argument("Gain or delay is out of range");
            static const std::set<std::string> routes{
                "stereo",      "swap",   "mono", "left",      "right",      "position",  "front-left",
                "front-right", "center", "lfe",  "back-left", "back-right", "side-left", "side-right"};
            if (!routes.contains(m.routing) || !bounded(m.azimuth_deg, -180, 180) ||
                !bounded(m.distance_m, .2, 20))
                throw std::invalid_argument("Invalid speaker routing or position");
            if (m.calibration)
            {
                const auto &a = *m.calibration;
                if (!bounded(a.acoustic_ms, -100, 1000) || !bounded(a.uncertainty_ms, 0, 100) ||
                    !bounded(a.verified_spread_ms, 0, 2000) || a.fingerprint.empty() ||
                    a.fingerprint.size() > 2048 || a.measured_at.size() > 64 ||
                    a.microphone_id.size() > 1024 ||
                    (a.verified &&
                     (a.verified_spread_ms + 2 * a.uncertainty_ms > 1 || a.uncertainty_ms > 0.25)))
                    throw std::invalid_argument("Invalid calibration result");
            }
        }
    }
}
std::string config_signature(const Group &g)
{
    return Json(g).dump();
}
Group lite_mirror_targets(const Group &group, const std::string &default_output_id)
{
    auto targets = group;
    std::erase_if(targets.members,
                  [&](const Member &member) { return member.endpoint_id == default_output_id; });
    return targets;
}
double playback_delay_seconds(double target, double minimum, double offset_ms)
{
    return std::max(minimum, target + offset_ms / 1000.0);
}
std::string active_group_id(const Config &config)
{
    return !config.active_group_id.empty() ? config.active_group_id
           : config.groups.empty()         ? std::string{}
                                           : config.groups.front().id;
}
} // namespace omni
