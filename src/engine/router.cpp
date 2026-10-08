#include <omni/router.hpp>
#include <algorithm>
#include <cmath>
#include <future>
namespace omni
{
GroupRouter::GroupRouter(Group g, std::string source, bool invalid, bool lite)
    : group_(std::move(g)), initial_group_(group_), source_(std::move(source)),
      source_fingerprint_(describe_endpoint(source_).fingerprint), invalidate_(invalid), lite_(lite),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    for (const auto &m : group_.members)
        states_.push_back(std::make_unique<MemberState>(m));
    update_controls(group_);
    thread_ = std::thread([this] { run(); });
}
void GroupRouter::update_controls(const Group &group)
{
    static_assert(std::atomic<float>::is_always_lock_free);
    for (size_t i = 0; i < states_.size(); ++i)
    {
        auto &state = *states_[i];
        const auto format = source_format_.load();
        auto matrix = routing_matrix(lite_ ? Member{} : group.members[i], static_cast<unsigned>(format),
                                     static_cast<uint32_t>(format >> 32));
        state.matrix_revision.fetch_add(1); // Odd means the bounded audio-thread snapshot must retry.
        for (size_t c = 0; c < 8; ++c)
        {
            state.matrix[c].store(matrix.left[c], std::memory_order_relaxed);
            state.matrix[c + 8].store(matrix.right[c], std::memory_order_relaxed);
        }
        state.matrix_revision.fetch_add(1);
        state.delay_ms = group.members[i].delay_ms;
        state.follow_volume = group.members[i].follow_reference_volume;
        if (group.members[i].delay_ms != group_.members[i].delay_ms ||
            group.members[i].routing != group_.members[i].routing ||
            group.members[i].azimuth_deg != group_.members[i].azimuth_deg ||
            group.members[i].distance_m != group_.members[i].distance_m)
            measurement_invalidated_ = true;
    }
    safety_ms_ = lite_ ? 0.0 : group.safety_ms;
    group_ = group;
}
GroupRouter::~GroupRouter()
{
    SetEvent(stop_.get());
    if (thread_.joinable())
        thread_.join();
}
Json GroupRouter::status() const
{
    static const char *names[] = {"starting", "buffering", "playing", "disconnected", "error"};
    Json j = {{"id", group_.id},
              {"sourceEndpointId", source_},
              {"state", names[static_cast<int>(state_.load())]},
              {"latencyMs", latency_ms_.load()},
              {"errorCode", error_.load()},
              {"inputChannels", static_cast<unsigned>(source_format_.load())},
              {"inputChannelMask", static_cast<uint32_t>(source_format_.load() >> 32)},
              {"processingRate", sample_rate},
              {"outputProcessing", group_.output_processing},
              {"members", Json::array()}};
    for (const auto &s : states_)
    {
        Json m = {{"endpointId", s->member.endpoint_id},
                  {"state", names[static_cast<int>(s->state.load())]},
                  {"underrunBuffers", s->underruns.load()},
                  {"missingSourceFrames", s->missing_frames.load()},
                  {"resynchronizations", s->resynchronizations.load()},
                  {"floatHeadroomSamples", s->headroom_samples.load()},
                  {"invalidSamples", s->invalid_samples.load()},
                  {"renderStarvations", s->render_starvations.load()},
                  {"rawOutput", s->raw_output.load()},
                  {"estimatedCorrectionMs", s->correction_ms.load()},
                  {"bufferDelayMs", s->playback_delay_ms.load()},
                  {"adaptiveBufferMs", s->adaptive_buffer_ms.load()},
                  {"calibrationValid", s->calibration_valid.load()},
                  {"errorCode", s->error.load()}};
        if (s->member.calibration)
            m["lastMeasurement"] = *s->member.calibration;
        j["members"].push_back(std::move(m));
    }
    return j;
}
void GroupRouter::run()
{
    try
    {
        ComScope com;
        CaptureStream source(source_, true, !lite_);
        source_format_ = (static_cast<uint64_t>(source.channel_mask()) << 32) | source.channels();
        Timeline timeline;
        SincSampler sampler;
        ClockFit source_clock;
        struct OpenedOutput
        {
            std::unique_ptr<RenderStream> stream;
            double acoustic{};
            bool bluetooth{};
        };
        struct Output
        {
            std::unique_ptr<RenderStream> stream;
            std::future<OpenedOutput> pending;
            MemberState *state{};
            double acoustic{}, retry{}, phase_offset{};
            double jitter_guard{};
            ContinuousRenderer renderer;
            std::unique_ptr<Timeline> routed;
            RoutingMatrix matrix, target_matrix;
        };
        std::vector<Output> outputs;
        outputs.reserve(states_.size());
        const bool prefer_raw = lite_ || initial_group_.output_processing == "raw";
        double safety = lite_ ? 0.0 : initial_group_.safety_ms / 1000.0;
        double target = safety;
        for (auto &s : states_)
        {
            Output o;
            o.state = s.get();
            o.matrix = o.target_matrix =
                routing_matrix(lite_ ? Member{} : s->member, source.channels(), source.channel_mask());
            if (!lite_)
                o.routed = std::make_unique<Timeline>();
            try
            {
                auto ep = describe_endpoint(s->member.endpoint_id);
                if (ep.capture || !ep.group_id.empty())
                    throw std::runtime_error("Invalid group output");
                if (s->member.calibration && s->member.calibration->fingerprint == ep.fingerprint)
                {
                    o.acoustic = s->member.calibration->acoustic_ms / 1000.0;
                    s->calibration_valid =
                        !invalidate_ && s->member.calibration->verified && s->member.delay_ms == 0;
                }
                o.stream = std::make_unique<RenderStream>(s->member.endpoint_id, prefer_raw);
                s->raw_output = o.stream->raw();
                target = std::max(target, safety + o.stream->latency_seconds() + std::max(o.acoustic, 0.0));
            }
            catch (const HResultError &e)
            {
                s->error = e.code;
                s->state = State::disconnected;
            }
            catch (...)
            {
                s->error = E_FAIL;
                s->state = State::error;
            }
            outputs.push_back(std::move(o));
        }
        target = std::min(target, 2.0);
        latency_ms_ = target * 1000;
        source.start();
        for (auto &o : outputs)
            if (o.stream)
                o.stream->start();
        AudioPriority priority;
        double last_time = qpc_seconds(), last_source_packet = 0;
        double source_begin = 0, source_end = 0, source_packet_frames = 0;
        bool source_idle = true;
        while (WaitForSingleObject(stop_.get(), 0) != WAIT_OBJECT_0)
        {
            if (measurement_invalidated_.exchange(false))
                for (auto &o : outputs)
                    o.state->calibration_valid = false;
            HANDLE handles[max_members + 2];
            DWORD count = 2;
            handles[0] = stop_.get();
            handles[1] = source.event();
            for (auto &o : outputs)
                if (o.stream)
                    handles[count++] = o.stream->event();
            auto wake = WaitForMultipleObjects(count, handles, FALSE, 100);
            if (wake == WAIT_OBJECT_0)
                break;
            double now = qpc_seconds();
            if (now - last_time > 1)
            {
                timeline.clear();
                source_clock.reset();
                for (auto &o : outputs)
                {
                    o.renderer.reset();
                    if (o.routed)
                        o.routed->clear();
                    o.state->calibration_valid = false;
                }
            }
            last_time = now;
            safety = safety_ms_.load() / 1000.0;
            if (!lite_)
            {
                target = safety;
                for (auto &o : outputs)
                {
                    if (o.stream)
                        target = std::max(target,
                                          safety + o.stream->latency_seconds() + std::max(o.acoustic, 0.0));
                    // Two bounded attempts, then retain the previous complete matrix.
                    for (unsigned attempt = 0; attempt < 2; ++attempt)
                    {
                        const auto revision = o.state->matrix_revision.load();
                        if (revision & 1)
                            continue;
                        RoutingMatrix matrix;
                        for (size_t c = 0; c < 8; ++c)
                        {
                            matrix.left[c] = o.state->matrix[c].load(std::memory_order_relaxed);
                            matrix.right[c] = o.state->matrix[c + 8].load(std::memory_order_relaxed);
                        }
                        if (revision == o.state->matrix_revision.load())
                        {
                            o.target_matrix = matrix;
                            break;
                        }
                    }
                }
                target = std::min(target, 2.0);
                latency_ms_ = target * 1000;
            }
            CapturePacket packet;
            while (source.next(packet))
            {
                last_source_packet = now;
                source_idle = false;
                source_packet_frames = packet.frames;
                if (packet.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                {
                    timeline.clear();
                    source_clock.reset();
                    for (auto &o : outputs)
                    {
                        o.renderer.reset();
                        if (o.routed)
                            o.routed->clear();
                    }
                }
                if (!(packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))
                {
                    if (!source_clock.has_position())
                        source_begin = static_cast<double>(packet.position);
                    source_end = static_cast<double>(packet.position) + packet.frames;
                    source_clock.observe(static_cast<double>(packet.position), packet.time);
                    if (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        timeline.silence(static_cast<int64_t>(packet.position), packet.frames);
                    else if (lite_)
                        timeline.put(static_cast<int64_t>(packet.position), {packet.data, packet.frames});
                    if (!lite_)
                        for (auto &o : outputs)
                        {
                            if (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT)
                                o.routed->silence(static_cast<int64_t>(packet.position), packet.frames);
                            else
                                for (UINT32 frame = 0; frame < packet.frames; ++frame)
                                {
                                    for (size_t c = 0; c < 8; ++c)
                                    {
                                        o.matrix.left[c] +=
                                            (o.target_matrix.left[c] - o.matrix.left[c]) / 240;
                                        o.matrix.right[c] +=
                                            (o.target_matrix.right[c] - o.matrix.right[c]) / 240;
                                    }
                                    const auto channels =
                                        canonical_channels(packet.samples + frame * packet.channels,
                                                           packet.channels, packet.channel_mask);
                                    for (const auto value : channels)
                                        if (!std::isfinite(value))
                                            ++o.state->invalid_samples;
                                    const auto value = o.matrix.apply(channels);
                                    o.routed->put(static_cast<int64_t>(packet.position + frame), {&value, 1});
                                }
                        }
                }
                else
                {
                    timeline.clear();
                    source_clock.reset();
                    for (auto &o : outputs)
                    {
                        o.renderer.reset();
                        if (o.routed)
                            o.routed->clear();
                    }
                }
                source.release(packet.frames);
            }
            const double packet_period = source_packet_frames / 48000;
            if (!source_idle && now - last_source_packet > std::clamp(packet_period * 2, .02, .1))
            {
                // Loopback produces no packets when the Windows mix goes idle.
                // Do not keep re-anchoring into its final, already-played packet.
                source_idle = true;
                timeline.clear();
                source_clock.reset();
                for (auto &o : outputs)
                {
                    o.renderer.reset();
                    if (o.routed)
                        o.routed->clear();
                }
            }
            bool playing = false, connected = false;
            for (auto &o : outputs)
            {
                if (!o.stream)
                {
                    try
                    {
                        if (!o.pending.valid())
                        {
                            if (now < o.retry)
                                continue;
                            // Endpoint activation can block. Keep it off the running group's audio thread.
                            o.pending =
                                std::async(std::launch::async, [member = o.state->member, prefer_raw] {
                                    ComScope worker_com;
                                    auto ep = describe_endpoint(member.endpoint_id);
                                    if (!ep.active || ep.capture || !ep.group_id.empty())
                                        throw std::runtime_error("Output remains unavailable");
                                    double acoustic = member.calibration && member.calibration->fingerprint ==
                                                                                ep.fingerprint
                                                          ? member.calibration->acoustic_ms / 1000.0
                                                          : 0;
                                    return OpenedOutput{
                                        std::make_unique<RenderStream>(member.endpoint_id, prefer_raw),
                                        acoustic, ep.bluetooth};
                                });
                        }
                        if (o.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                            continue;
                        auto opened = o.pending.get();
                        o.acoustic = opened.acoustic;
                        o.stream = std::move(opened.stream);
                        o.state->raw_output = o.stream->raw();
                        o.stream->start();
                        o.renderer.reset();
                        o.state->calibration_valid = false;
                        o.state->error = S_OK;
                        target = std::min(2.0, std::max(target, safety + o.stream->latency_seconds() +
                                                                    std::max(o.acoustic, 0.0)));
                        latency_ms_ = target * 1000;
                    }
                    catch (...)
                    {
                        o.stream.reset();
                        o.retry = now + 2;
                    }
                    if (!o.stream)
                        continue;
                }
                connected = true;
                try
                {
                    o.stream->update_clock();
                    auto n = o.stream->available();
                    if (!n)
                        continue;
                    // Packet timestamps already provide a presentation anchor. Begin
                    // at nominal 48 kHz once a complete block is available; the rate
                    // fit refines it later instead of discarding the first 80 ms.
                    bool ready = source_clock.has_position() && o.stream->clock_position_valid();
                    double desired = 0, step = 1;
                    if (ready)
                    {
                        const double delay = playback_delay_seconds(
                            lite_ ? o.stream->latency_seconds() : target,
                            safety + o.stream->latency_seconds(), o.state->delay_ms.load());
                        o.state->playback_delay_ms = delay * 1000;
                        double t = o.stream->next_time() + o.acoustic - delay;
                        desired = (t - source_clock.time_of(0)) / source_clock.seconds_per_frame();
                        step = o.stream->frame_seconds() / source_clock.seconds_per_frame();
                        if (lite_)
                        {
                            // Anchor once to a complete captured block plus one observed
                            // capture-packet reserve. Packet arrival times are not a playback
                            // clock: repeatedly clamping to them creates phase jumps/modulation.
                            const double newest = source_end - n * step - SincSampler::right_guard - 1;
                            if (!o.renderer.ready())
                            {
                                o.jitter_guard =
                                    std::max(o.jitter_guard, std::min(960.0, source_packet_frames +
                                                                                 SincSampler::right_guard));
                                double anchor = std::min(desired, newest - o.state->member.delay_ms * 48 -
                                                                      o.jitter_guard);
                                o.phase_offset = desired - anchor;
                            }
                            desired -= o.phase_offset;
                            if (!o.renderer.ready() && desired < source_begin + SincSampler::left_guard)
                                ready = false;
                            o.state->adaptive_buffer_ms = o.jitter_guard / 48;
                            o.state->playback_delay_ms =
                                (o.stream->next_time() - source_clock.time_of(desired)) * 1000;
                        }
                    }
                    if (!ready || !std::isfinite(desired) || !std::isfinite(step))
                    {
                        auto *out = o.stream->acquire(n);
                        o.renderer.silence({out, n});
                        o.stream->release(n);
                        o.state->state = State::buffering;
                        o.renderer.reset();
                        continue;
                    }
                    double error = o.renderer.ready() ? desired - o.renderer.position() : 0;
                    o.state->correction_ms = error / 48.0;
                    auto *out = o.stream->acquire(n);
                    const Frame input_gain = lite_ || o.state->follow_volume.load()
                                                 ? Frame{input_l_.load(std::memory_order_relaxed),
                                                         input_r_.load(std::memory_order_relaxed)}
                                                 : Frame{1, 1};
                    auto rendered = o.renderer.render(o.routed ? *o.routed : timeline, sampler, {out, n},
                                                      desired, step, input_gain);
                    o.state->resynchronizations += rendered.resynchronized;
                    o.state->headroom_samples += rendered.headroom_samples;
                    o.state->invalid_samples += rendered.invalid_samples;
                    o.state->render_starvations = o.stream->starvations();
                    o.stream->release(n);
                    if (rendered.missing_frames)
                    {
                        if (now - last_source_packet < std::max(.015, packet_period * 1.5))
                        {
                            o.state->missing_frames += rendered.missing_frames;
                            ++o.state->underruns;
                            if (lite_)
                            {
                                // Grow only on observed starvation, bounded to 20 ms.
                                o.jitter_guard = std::min(960.0, o.jitter_guard + 64);
                                o.state->adaptive_buffer_ms = o.jitter_guard / 48;
                                o.renderer.reset();
                            }
                        }
                        o.state->state = State::buffering;
                    }
                    else
                    {
                        o.state->state = State::playing;
                        playing = true;
                    }
                }
                catch (const HResultError &e)
                {
                    o.state->error = e.code;
                    o.state->state = State::disconnected;
                    o.state->calibration_valid = false;
                    o.stream.reset();
                    o.renderer.reset();
                    o.retry = now + 2;
                }
            }
            playing = std::any_of(outputs.begin(), outputs.end(),
                                  [](const Output &o) { return o.state->state == State::playing; });
            state_ = playing ? State::playing : connected ? State::buffering : State::disconnected;
        }
    }
    catch (const HResultError &e)
    {
        error_ = e.code;
        state_ = State::error;
        log_event("group-audio-error", e.what());
    }
    catch (const std::exception &e)
    {
        error_ = E_FAIL;
        state_ = State::error;
        log_event("group-audio-error", e.what());
    }
}
} // namespace omni
