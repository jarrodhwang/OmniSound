#include <omni/router.hpp>
#include <omni/ipc.hpp>
#include <mutex>
#include <map>
#include <iostream>
using namespace omni;
namespace
{
std::atomic<bool> stopping{};
Config load_config(bool lite)
{
    return lite ? load_lite_config() : request(service_pipe, {{"op", "load"}}).at("config").get<Config>();
}
Config save_config(bool lite, Config config, uint64_t expected_revision)
{
    if (lite)
        return save_lite_config(std::move(config), expected_revision);
    return request(service_pipe, {{"op", "save"}, {"config", config}}).at("config").get<Config>();
}
BOOL WINAPI console_signal(DWORD)
{
    stopping = true;
    return TRUE;
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    try
    {
        bool lite = false, once = false, login = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg(argv[i]);
            lite |= arg == L"--lite";
            once |= arg == L"--once";
            login |= arg == L"--login";
        }
        if (argc > 1 && std::wstring(argv[1]) == L"--help")
        {
            std::cout << "OmniSoundEngine: per-user WASAPI router. --lite mirrors the Windows default "
                         "output without a virtual driver; --once validates configuration.\n";
            return 0;
        }
        ComScope com;
        if (login && !login_start_enabled(lite))
            return 0;
        const auto instance_name =
            L"Local\\OmniSound.Engine." + std::wstring(lite ? L"Lite." : L"Driver.") + user_sid();
        Handle mutex(CreateMutexW(nullptr, FALSE, instance_name.c_str()));
        if (GetLastError() == ERROR_ALREADY_EXISTS)
            return 0;
        SetConsoleCtrlHandler(console_signal, TRUE);
        DeviceWatcher watcher;
        std::map<std::string, std::unique_ptr<GroupRouter>> routers;
        std::mutex control;
        // A persisted measurement is historical. Device changes while the engine was stopped
        // cannot be observed, so only measurements made in this run may claim current validity.
        std::map<std::string, std::string> calibrated_in_session;
        Json snapshot = {{"state", "starting"}, {"groups", Json::array()}}, job = {{"state", "idle"}};
        bool start_job = false;
        std::string job_type, job_group, job_endpoint;
        std::atomic<bool> cancel{};
        std::atomic<bool> busy{};
        std::atomic<bool> reload{};
        Handle control_wake(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        std::thread job_thread;
        PipeServer server(engine_pipe(lite), false, [&](const Json &r, const Caller &) -> Json {
            auto op = r.at("op").get<std::string>();
            std::lock_guard lock(control);
            if (op == "startup")
            {
                set_login_start(lite, r.at("enabled").get<bool>());
                snapshot["startWithWindows"] = login_start_enabled(lite);
                return {{"ok", true},
                        {"enabled", login_start_enabled(lite)},
                        {"launchCommand", utf8(login_launch_command(lite))}};
            }
            if (lite && op == "load")
                return {{"config", load_lite_config()}};
            if (lite && op == "save")
            {
                auto next = r.at("config").get<Config>();
                const auto expected = next.revision;
                auto saved = save_lite_config(std::move(next), expected);
                reload = true;
                SetEvent(control_wake.get());
                return {{"config", saved}};
            }
            if (lite && op == "activate")
            {
                auto next = load_lite_config();
                const auto id = r.at("groupId").get<std::string>();
                if (std::none_of(next.groups.begin(), next.groups.end(),
                                 [&](const Group &g) { return g.id == id; }))
                    throw std::runtime_error("그룹을 먼저 저장하세요.");
                if (active_group_id(next) != id)
                {
                    const auto expected = next.revision;
                    next.active_group_id = id;
                    next = save_lite_config(std::move(next), expected);
                    reload = true;
                    SetEvent(control_wake.get());
                }
                return {{"config", next}};
            }
            if (op == "status")
                return {{"engine", snapshot}, {"job", job}};
            if (op == "reload")
            {
                reload = true;
                SetEvent(control_wake.get());
                return {{"ok", true}};
            }
            if (op == "shutdown")
            {
                stopping = true;
                cancel = true;
                return {{"ok", true}};
            }
            if (op == "cancel")
            {
                cancel = true;
                return {{"ok", true}};
            }
            if (op == "test" || op == "calibrate")
            {
                if (busy.exchange(true))
                    throw std::runtime_error("An audio operation is already running");
                try
                {
                    job_type = op;
                    job_endpoint = r.at(op == "test" ? "endpointId" : "microphoneId").get<std::string>();
                    job_group = r.value("groupId", std::string{});
                    if (job_endpoint.size() > 1024)
                        throw std::runtime_error("Invalid endpoint");
                    job = {{"state", "running"},
                           {"type", op},
                           {"groupId", job_group},
                           {"progress", "Preparing…"}};
                    cancel = false;
                    start_job = true;
                }
                catch (...)
                {
                    busy = false;
                    throw;
                }
                return {{"ok", true}};
            }
            throw std::runtime_error("Unknown engine command");
        });
        Config config;
        uint64_t revision = 0, device_revision = watcher.revision();
        bool loaded = false;
        std::vector<Endpoint> device_cache;
        std::string lite_source;
        std::unique_ptr<VolumeWatcher> reference_watch;
        std::map<std::string, std::string> volume_applied;
        double poll = 0;
        while (!stopping)
        {
            if (process_session() != WTSGetActiveConsoleSessionId())
            {
                routers.clear();
                cancel = true;
                {
                    std::lock_guard lock(control);
                    calibrated_in_session.clear();
                    snapshot = {{"state", "inactive-session"}, {"groups", Json::array()}};
                }
                Sleep(500);
                continue;
            }
            bool launch = false;
            std::string type, group_id, endpoint;
            {
                std::lock_guard lock(control);
                if (start_job)
                {
                    calibrated_in_session.clear();
                    launch = true;
                    start_job = false;
                    type = job_type;
                    group_id = job_group;
                    endpoint = job_endpoint;
                }
            }
            if (launch)
            {
                routers.clear();
                if (job_thread.joinable())
                    job_thread.join();
                job_thread = std::thread([&, type, group_id, endpoint] {
                    try
                    {
                        ComScope thread_com;
                        if (type == "test")
                        {
                            play_test(endpoint, cancel);
                            std::lock_guard lock(control);
                            job = {{"state", cancel ? "cancelled" : "complete"}, {"type", type}};
                        }
                        else
                        {
                            auto fresh = load_config(lite);
                            auto it = std::find_if(fresh.groups.begin(), fresh.groups.end(),
                                                   [&](const Group &g) { return g.id == group_id; });
                            if (it == fresh.groups.end())
                                throw std::runtime_error("Group no longer exists");
                            Group calibration_group = *it;
                            if (lite)
                            {
                                const auto source = default_render_endpoint();
                                if (!describe_endpoint(source).group_id.empty())
                                    throw std::runtime_error(
                                        "Choose a physical Windows default output before Lite calibration");
                                calibration_group = lite_mirror_targets(calibration_group, source);
                                if (calibration_group.members.size() < 2)
                                    throw std::runtime_error("Lite calibration requires two mirrored outputs "
                                                             "in addition to its direct primary output");
                            }
                            auto result = calibrate_group(calibration_group, endpoint, cancel,
                                                          [&](const std::string &p) {
                                                              std::lock_guard lock(control);
                                                              job["progress"] = p;
                                                          });
                            auto measured = result.at("group").get<Group>();
                            auto updated = fresh;
                            for (auto &g : updated.groups)
                                if (g.id == group_id)
                                {
                                    if (!lite)
                                        g = measured;
                                    else
                                        for (auto &member : g.members)
                                        {
                                            auto value = std::find_if(
                                                measured.members.begin(), measured.members.end(),
                                                [&](const Member &candidate) {
                                                    return candidate.endpoint_id == member.endpoint_id;
                                                });
                                            if (value != measured.members.end())
                                            {
                                                member.delay_ms = value->delay_ms;
                                                member.calibration = value->calibration;
                                            }
                                        }
                                }
                            if (cancel)
                                throw std::runtime_error("Calibration cancelled");
                            auto saved = save_config(lite, std::move(updated), fresh.revision);
                            auto saved_group = std::find_if(saved.groups.begin(), saved.groups.end(),
                                                            [&](const Group &g) { return g.id == group_id; });
                            if (saved_group == saved.groups.end())
                                throw std::runtime_error("Saved calibration group is missing");
                            std::lock_guard lock(control);
                            calibrated_in_session[group_id] = config_signature(*saved_group);
                            job = {{"state", "complete"},
                                   {"type", type},
                                   {"groupId", group_id},
                                   {"result", result}};
                        }
                    }
                    catch (const std::exception &e)
                    {
                        std::lock_guard lock(control);
                        job = {{"state", cancel ? "cancelled" : "error"},
                               {"type", type},
                               {"groupId", group_id},
                               {"message", e.what()}};
                    }
                    busy = false;
                });
            }
            if (reload.exchange(false))
                poll = 0;
            if (qpc_seconds() >= poll && !busy)
            {
                poll = qpc_seconds() + 1;
                try
                {
                    auto next = load_config(lite);
                    auto dev_revision = watcher.revision();
                    bool device_changed = dev_revision != device_revision;
                    bool source_changed = false;
                    if (lite)
                    {
                        std::string source;
                        try
                        {
                            source = default_render_endpoint();
                            if (!describe_endpoint(source).group_id.empty())
                                source.clear(); // Never capture a virtual OmniSound endpoint into itself.
                        }
                        catch (...)
                        {
                            source.clear();
                        }
                        source_changed = source != lite_source;
                        if (source_changed)
                        {
                            reference_watch.reset();
                            if (!source.empty())
                                reference_watch = std::make_unique<VolumeWatcher>(source, [&] {
                                    reload = true;
                                    SetEvent(control_wake.get());
                                });
                            routers.clear();
                            std::lock_guard lock(control);
                            calibrated_in_session.clear();
                        }
                        lite_source = std::move(source);
                        device_changed |= source_changed;
                    }
                    if (device_changed)
                    {
                        std::lock_guard lock(control);
                        calibrated_in_session.clear();
                    }
                    if (!loaded || next.revision != revision || device_changed)
                        device_cache = endpoints();
                    const auto &devs = device_cache;
                    if (!loaded || next.revision != revision || device_changed)
                    {
                        // A device shared with the previous active group may have received
                        // a different master volume. Apply this group's settings again.
                        volume_applied.clear();
                        for (auto it = routers.begin(); it != routers.end();)
                        {
                            auto replacement =
                                std::find_if(next.groups.begin(), next.groups.end(),
                                             [&](const Group &g) { return g.id == it->first; });
                            if ((lite && it->first != active_group_id(next)) ||
                                replacement == next.groups.end() ||
                                (lite
                                     ? config_signature(*replacement) != config_signature(it->second->group())
                                     : !same_stream_topology(*replacement, it->second->group())))
                                it = routers.erase(it);
                            else
                            {
                                it->second->update_controls(*replacement);
                                if (device_changed)
                                    it->second->invalidate_measurement();
                                ++it;
                            }
                        }
                        config = std::move(next);
                        revision = config.revision;
                        device_revision = dev_revision;
                        loaded = true;
                    }
                    std::map<std::string, Group> route_groups;
                    std::map<std::string, std::string> route_sources;
                    std::map<std::string, std::string> unavailable;
                    for (const auto &g : config.groups)
                    {
                        if (lite && g.id != active_group_id(config))
                        {
                            unavailable[g.id] = "inactive";
                            continue;
                        }
                        if (g.members.empty())
                        {
                            unavailable[g.id] = lite ? "direct-only" : "no-outputs";
                            continue;
                        }
                        std::string source;
                        Group routing_group = g;
                        if (lite)
                        {
                            source = lite_source;
                            if (source.empty())
                            {
                                unavailable[g.id] = "source-unavailable";
                                continue;
                            }
                            // Keep the Windows default endpoint as the direct, original playback path.
                            routing_group = lite_mirror_targets(routing_group, source);
                            if (routing_group.members.empty())
                            {
                                unavailable[g.id] = "direct-only";
                                continue;
                            }
                        }
                        else
                        {
                            auto virtual_endpoint =
                                std::find_if(devs.begin(), devs.end(),
                                             [&](const Endpoint &d) { return d.group_id == g.id; });
                            if (virtual_endpoint == devs.end())
                            {
                                unavailable[g.id] = "driver-unavailable";
                                continue;
                            }
                            source = virtual_endpoint->id;
                        }
                        route_groups.emplace(g.id, std::move(routing_group));
                        route_sources.emplace(g.id, std::move(source));
                        const auto signature = config_signature(g);
                        for (const auto &member : route_groups.at(g.id).members)
                        {
                            const auto key = g.id + member.endpoint_id;
                            if (member.follow_reference_volume || volume_applied[key] != signature ||
                                device_changed)
                                try
                                {
                                    const auto level = member.follow_reference_volume
                                                           ? endpoint_volume(route_sources.at(g.id))
                                                           : static_cast<float>(member.gain);
                                    set_endpoint_volume(member.endpoint_id, level);
                                    volume_applied[key] = signature;
                                }
                                catch (const std::exception &e)
                                {
                                    log_event("device-volume-error", e.what());
                                }
                        }
                    }
                    for (const auto &g : config.groups)
                    {
                        auto route = route_groups.find(g.id);
                        if (route == route_groups.end())
                        {
                            routers.erase(g.id);
                            continue;
                        }
                        auto found = routers.find(g.id);
                        if (found != routers.end())
                        {
                            auto status = found->second->status();
                            if (std::any_of(status.at("members").begin(), status.at("members").end(),
                                            [](const Json &member) {
                                                return !member.value("calibrationValid", false);
                                            }))
                            {
                                std::lock_guard lock(control);
                                calibrated_in_session.erase(g.id);
                            }
                            if (status["state"] == "error" ||
                                status.value("sourceEndpointId", std::string{}) != route_sources.at(g.id) ||
                                std::any_of(devs.begin(), devs.end(), [&](const Endpoint &endpoint) {
                                    return endpoint.id == route_sources.at(g.id) &&
                                           endpoint.fingerprint != found->second->source_fingerprint();
                                }))
                            {
                                routers.erase(found);
                                std::lock_guard lock(control);
                                calibrated_in_session.erase(g.id);
                            }
                        }
                        if (!routers.contains(g.id))
                        {
                            bool current_measurement = false;
                            {
                                std::lock_guard lock(control);
                                auto record = calibrated_in_session.find(g.id);
                                current_measurement = record != calibrated_in_session.end() &&
                                                      record->second == config_signature(g);
                            }
                            routers.emplace(g.id, std::make_unique<GroupRouter>(route->second,
                                                                                route_sources.at(g.id),
                                                                                !current_measurement, lite));
                        }
                        if (lite)
                            routers.at(g.id)->source_gain(loopback_unity_gain(lite_source));
                        else
                        {
                            routers.at(g.id)->update_controls(route->second);
                            routers.at(g.id)->source_gain(loopback_unity_gain(route_sources.at(g.id), true));
                        }
                    }
                    Json groups = Json::array();
                    for (const auto &g : config.groups)
                    {
                        if (routers.contains(g.id))
                        {
                            auto status = routers.at(g.id)->status();
                            status["referenceVolumePercent"] =
                                endpoint_volume(route_sources.at(g.id)) * 100.0;
                            groups.push_back(std::move(status));
                        }
                        else
                            groups.push_back({{"id", g.id},
                                              {"state", unavailable.contains(g.id) ? unavailable.at(g.id)
                                                                                   : "driver-unavailable"},
                                              {"members", Json::array()}});
                    }
                    std::lock_guard lock(control);
                    snapshot = {{"state", "running"},
                                {"groups", groups},
                                {"revision", revision},
                                {"routerCount", routers.size()},
                                {"activeGroupId", active_group_id(config)},
                                {"startWithWindows", login_start_enabled(lite)},
                                {"referenceVolumePercent",
                                 lite_source.empty() ? 0.0 : endpoint_volume(lite_source) * 100.0},
                                {"configDirectory", utf8(local_data().wstring())}};
                    if (once)
                    {
                        stopping = true;
                        std::cout << snapshot.dump(2) << '\n';
                    }
                }
                catch (const std::exception &e)
                {
                    std::lock_guard lock(control);
                    snapshot = {{"state", lite ? "configuration-unavailable" : "service-unavailable"},
                                {"message", e.what()},
                                {"groups", Json::array()}};
                    if (once)
                        throw;
                }
            }
            if (busy)
            {
                std::lock_guard lock(control);
                snapshot["state"] = "calibrating";
                snapshot["groups"] = Json::array();
            }
            WaitForSingleObject(control_wake.get(), 50);
        }
        cancel = true;
        routers.clear();
        if (job_thread.joinable())
            job_thread.join();
        return 0;
    }
    catch (const std::exception &e)
    {
        log_event("engine-failure", e.what());
        std::cerr << e.what() << '\n';
        return 1;
    }
}
