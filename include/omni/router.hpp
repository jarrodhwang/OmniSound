#pragma once
#include <omni/audio.hpp>
#include <omni/routing.hpp>
#include <thread>
#include <atomic>
namespace omni
{
class GroupRouter
{
  public:
    GroupRouter(Group, std::string source, bool invalidate_calibration = false, bool lite = false);
    ~GroupRouter();
    Json status() const;
    void update_controls(const Group &);
    void source_gain(Frame gain)
    {
        input_l_ = gain.l;
        input_r_ = gain.r;
    }
    void invalidate_measurement() noexcept
    {
        measurement_invalidated_ = true;
    }
    const Group &group() const
    {
        return group_;
    }
    const std::string &source_fingerprint() const
    {
        return source_fingerprint_;
    }

  private:
    enum class State
    {
        starting,
        buffering,
        playing,
        disconnected,
        error
    };
    struct MemberState
    {
        Member member;
        std::atomic<State> state{State::starting};
        std::atomic<uint64_t> underruns{};
        std::atomic<uint64_t> missing_frames{}, resynchronizations{}, headroom_samples{}, invalid_samples{};
        std::atomic<uint64_t> render_starvations{};
        std::atomic<bool> raw_output{};
        std::atomic<double> correction_ms{};
        std::atomic<double> playback_delay_ms{};
        std::atomic<double> adaptive_buffer_ms{};
        std::atomic<bool> calibration_valid{};
        std::atomic<HRESULT> error{};
        explicit MemberState(Member m) : member(std::move(m)) {}
        std::array<std::atomic<float>, 16> matrix{};
        std::atomic<unsigned> matrix_revision{};
        std::atomic<double> delay_ms{};
        std::atomic<bool> follow_volume{};
    };
    void run();
    Group group_;
    const Group initial_group_;
    std::string source_;
    const std::string source_fingerprint_;
    bool invalidate_;
    bool lite_;
    Handle stop_;
    std::thread thread_;
    std::vector<std::unique_ptr<MemberState>> states_;
    std::atomic<State> state_{State::starting};
    std::atomic<double> latency_ms_{};
    std::atomic<HRESULT> error_{};
    std::atomic<bool> measurement_invalidated_{};
    std::atomic<float> input_l_{1}, input_r_{1};
    std::atomic<uint64_t> source_format_{(uint64_t{3} << 32) | 2};
    std::atomic<double> safety_ms_{};
};
Json calibrate_group(const Group &, const std::string &microphone, std::atomic<bool> &cancel,
                     const std::function<void(const std::string &)> &progress);
} // namespace omni
