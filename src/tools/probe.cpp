#include <omni/audio.hpp>
#include <omni/ipc.hpp>
#include <iostream>
#include <fstream>
#include <wintrust.h>
#include <softpub.h>
#include "quality.hpp"
using namespace omni;
int wmain(int argc, wchar_t **argv)
{
    try
    {
        ComScope com;
        std::wstring op = argc > 1 ? argv[1] : L"--enumerate";
        if (op == L"--quality-check" || op == L"--quality-check-raw")
        {
            std::cout << check_audio_quality(op == L"--quality-check-raw", argc > 2 ? std::stod(argv[2]) : 6)
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (op == L"--enumerate")
        {
            std::cout << Json({{"render", endpoints(false, true)}, {"capture", endpoints(true)}}).dump(2)
                      << '\n';
            return 0;
        }
        if (op == L"--service")
        {
            std::cout << request(service_pipe, {{"op", "load"}}).dump(2) << '\n';
            return 0;
        }
        if (op == L"--lite-config")
        {
            std::cout << request(engine_pipe(true), {{"op", "load"}}).dump(2) << '\n';
            return 0;
        }
        if (op == L"--activate" && argc == 3)
        {
            std::cout << request(engine_pipe(true), {{"op", "activate"}, {"groupId", utf8(argv[2])}}).dump(2)
                      << '\n';
            return 0;
        }
        if (op == L"--startup" && argc == 3)
        {
            std::cout << request(engine_pipe(true),
                                 {{"op", "startup"}, {"enabled", std::wstring_view(argv[2]) == L"on"}})
                             .dump(2)
                      << '\n';
            return 0;
        }
        if (op == L"--volume-status")
        {
            Json levels = Json::array();
            for (const auto &ep : endpoints())
                levels.push_back(
                    {{"endpointId", ep.id}, {"name", ep.name}, {"percent", endpoint_volume(ep.id) * 100}});
            std::cout << levels.dump(2) << '\n';
            return 0;
        }
        if (op == L"--mirror-smoke")
        {
            const auto source = default_render_endpoint();
            auto config = request(engine_pipe(true), {{"op", "load"}}).at("config").get<Config>();
            std::vector<std::pair<std::string, std::unique_ptr<CaptureStream>>> captures;
            for (const auto &group : config.groups)
                if (group.id == active_group_id(config))
                    for (const auto &member : lite_mirror_targets(group, source).members)
                        if (describe_endpoint(member.endpoint_id).active)
                        {
                            auto capture = std::make_unique<CaptureStream>(member.endpoint_id, true);
                            capture->start();
                            captures.emplace_back(member.endpoint_id, std::move(capture));
                        }
            std::vector<double> peaks(captures.size());
            CaptureStream reference(source, true);
            reference.start();
            double reference_peak = 0;
            std::atomic<bool> cancel{};
            std::exception_ptr tone_error;
            std::jthread tone([&] {
                try
                {
                    ComScope com;
                    play_test(source, cancel);
                }
                catch (...)
                {
                    tone_error = std::current_exception();
                }
            });
            const auto end = qpc_seconds() + 2;
            while (qpc_seconds() < end)
            {
                CapturePacket reference_packet;
                while (reference.next(reference_packet))
                {
                    if (!(reference_packet.flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        for (UINT32 frame = 0; frame < reference_packet.frames; ++frame)
                            reference_peak = std::max(
                                reference_peak,
                                static_cast<double>(std::max(std::abs(reference_packet.data[frame].l),
                                                             std::abs(reference_packet.data[frame].r))));
                    reference.release(reference_packet.frames);
                }
                for (size_t i = 0; i < captures.size(); ++i)
                {
                    CapturePacket packet;
                    while (captures[i].second->next(packet))
                    {
                        if (!(packet.flags & AUDCLNT_BUFFERFLAGS_SILENT))
                            for (UINT32 frame = 0; frame < packet.frames; ++frame)
                                peaks[i] = std::max(
                                    peaks[i], static_cast<double>(std::max(std::abs(packet.data[frame].l),
                                                                           std::abs(packet.data[frame].r))));
                        captures[i].second->release(packet.frames);
                    }
                }
                Sleep(5);
            }
            tone.join();
            if (tone_error)
                std::rethrow_exception(tone_error);
            Json result = Json::array();
            bool passed = !captures.empty();
            for (size_t i = 0; i < captures.size(); ++i)
            {
                passed &= peaks[i] > 0.001;
                result.push_back({{"endpointId", captures[i].first},
                                  {"peak", peaks[i]},
                                  {"ratioToReference", reference_peak > 0 ? peaks[i] / reference_peak : 0}});
            }
            std::cout << Json({{"passed", passed},
                               {"outputs", result},
                               {"referencePeak", reference_peak},
                               {"acousticMeasurement", false}})
                             .dump(2)
                      << '\n';
            return passed ? 0 : 2;
        }
        if (op == L"--lite-import" && argc == 3)
        {
            auto config = read_json(argv[2]).get<Config>();
            config.revision = request(engine_pipe(true), {{"op", "load"}}).at("config").at("revision");
            std::cout << request(engine_pipe(true), {{"op", "save"}, {"config", config}}).dump(2) << '\n';
            return 0;
        }
        if (op == L"--status" || op == L"--stop-engine" || op == L"--lite-status" || op == L"--stop-lite")
        {
            const bool lite = op == L"--lite-status" || op == L"--stop-lite";
            const bool status = op == L"--status" || op == L"--lite-status";
            std::cout << request(engine_pipe(lite), {{"op", status ? "status" : "shutdown"}}).dump(2) << '\n';
            return 0;
        }
        if (op == L"--render-recovery-smoke" && argc == 3)
        {
            const auto ep = describe_endpoint(utf8(argv[2]));
            if (ep.capture || !ep.group_id.empty())
                throw std::runtime_error("Choose a physical output");
            RenderStream output(ep.id, true);
            output.start();
            AudioPriority priority;
            auto fill = [&] {
                output.update_clock();
                auto n = output.available();
                if (n)
                {
                    output.acquire(n);
                    output.release(n, true);
                }
            };
            double end = qpc_seconds() + .4;
            while (qpc_seconds() < end)
            {
                WaitForSingleObject(output.event(), 100);
                fill();
            }
            const auto before = output.starvations();
            Sleep(120); // Deliberately drain only this silent diagnostic stream.
            output.update_clock();
            output.available();
            double predicted_error = output.next_time() - qpc_seconds();
            bool passed = output.clock_position_valid() && output.starvations() > before &&
                          std::abs(predicted_error) < .03;
            end = qpc_seconds() + .3;
            while (qpc_seconds() < end)
            {
                WaitForSingleObject(output.event(), 100);
                fill();
            }
            std::cout << Json({{"endpointId", ep.id},
                               {"name", ep.name},
                               {"passed", passed},
                               {"silentRender", true},
                               {"pauseMs", 120},
                               {"starvations", output.starvations()},
                               {"rebasedPredictionErrorMs", predicted_error * 1000}})
                             .dump(2)
                      << '\n';
            return passed ? 0 : 2;
        }
        if (op == L"--render-smoke" && argc == 3)
        {
            auto ep = describe_endpoint(utf8(argv[2]));
            if (ep.capture || !ep.group_id.empty())
                throw std::runtime_error("Choose a physical output");
            RenderStream output(ep.id);
            output.start();
            AudioPriority priority;
            unsigned readings = 0;
            double end = qpc_seconds() + 1;
            while (qpc_seconds() < end)
            {
                WaitForSingleObject(output.event(), 100);
                if (output.update_clock())
                    ++readings;
                auto n = output.available();
                if (n)
                {
                    output.acquire(n);
                    output.release(n, true);
                }
            }
            std::cout << Json({{"endpoint", ep.name},
                               {"silentRender", true},
                               {"clockReadings", readings},
                               {"clockReady", output.clock_ready()},
                               {"secondsPerFrame", output.frame_seconds()},
                               {"reportedLatencyMs", output.latency_seconds() * 1000}})
                             .dump(2)
                      << '\n';
            return output.clock_ready() ? 0 : 2;
        }
        if (op == L"--loopback-smoke" && argc == 3)
        {
            auto ep = describe_endpoint(utf8(argv[2]));
            if (ep.capture || !ep.group_id.empty())
                throw std::runtime_error("Choose a physical output");
            CaptureStream capture(ep.id, true);
            capture.start();
            ClockFit clock;
            std::array<unsigned, 2> packets{};
            unsigned discontinuities = 0, timestamp_errors = 0;
            for (unsigned phase = 0; phase < 2; ++phase)
            {
                if (phase)
                    Sleep(250);
                RenderStream render(ep.id);
                render.start();
                const auto end = qpc_seconds() + 1;
                HANDLE events[]{capture.event(), render.event()};
                while (qpc_seconds() < end)
                {
                    WaitForMultipleObjects(2, events, FALSE, 100);
                    auto n = render.available();
                    if (n)
                    {
                        render.acquire(n);
                        render.release(n, true);
                    }
                    CapturePacket p;
                    while (capture.next(p))
                    {
                        ++packets[phase];
                        if (p.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                        {
                            ++discontinuities;
                            clock.reset();
                        }
                        if (p.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                            ++timestamp_errors;
                        else
                            clock.observe(static_cast<double>(p.position), p.time);
                        capture.release(p.frames);
                    }
                }
            }
            std::cout << Json({{"endpoint", ep.name},
                               {"silentRender", true},
                               {"firstPlaybackPackets", packets[0]},
                               {"resumedPlaybackPackets", packets[1]},
                               {"clockReady", clock.ready()},
                               {"discontinuities", discontinuities},
                               {"timestampErrors", timestamp_errors},
                               {"audioStored", false}})
                             .dump(2)
                      << '\n';
            return packets[0] && packets[1] && clock.ready() && !timestamp_errors ? 0 : 2;
        }
        if (op == L"--check-signature" && argc == 3)
        {
            WINTRUST_FILE_INFO file{sizeof(file)};
            file.pcwszFilePath = argv[2];
            WINTRUST_DATA data{sizeof(data)};
            data.dwUIChoice = WTD_UI_NONE;
            data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
            data.dwUnionChoice = WTD_CHOICE_FILE;
            data.pFile = &file;
            data.dwStateAction = WTD_STATEACTION_VERIFY;
            GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            auto result = WinVerifyTrust(nullptr, &policy, &data);
            data.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(nullptr, &policy, &data);
            std::cout << Json({{"trusted", result == ERROR_SUCCESS}, {"status", result}}).dump() << '\n';
            return result == ERROR_SUCCESS ? 0 : 2;
        }
        std::cerr << "Usage: OmniSoundProbe [--enumerate | --service | --status | --stop-engine | "
                     "--lite-status | --stop-lite | "
                     "--render-smoke endpoint-id | --loopback-smoke endpoint-id | --check-signature file]\n";
        return 2;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
