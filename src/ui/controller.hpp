#pragma once
#include <omni/audio.hpp>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace omni::ui
{
struct Snapshot
{
    Config config;
    std::vector<Endpoint> outputs;
    std::vector<Endpoint> microphones;
    std::string default_render_id;
    Json engine_status;
    bool service_available{};
    bool save_committed{};
    bool activation_committed{};
    std::string error;
};

// UI-independent facade. Implementations own IPC and device notifications;
// callers must dispatch completions to their UI thread.
class Controller
{
  public:
    using Completion = std::function<void(Snapshot)>;
    virtual ~Controller() = default;
    virtual void refresh(Completion completion) = 0;
    virtual void save(Config config, Completion completion) = 0;
    virtual void activate(std::string group_id, Completion completion) = 0;
    virtual void startup(bool enabled, Completion completion) = 0;
    virtual void shutdown(Completion completion) = 0;
    virtual void test(std::string endpoint_id, Completion completion) = 0;
    virtual void calibrate(std::string group_id, std::string microphone_id, Completion completion) = 0;
    virtual void cancel(std::string group_id, Completion completion) = 0;
};

// Keeps service and engine IPC off the UI thread. Completions run on its worker;
// framework-specific controllers must marshal them to their dispatcher.
class IpcController final : public Controller
{
  public:
    explicit IpcController(bool lite = false);
    ~IpcController() override;
    IpcController(const IpcController &) = delete;
    IpcController &operator=(const IpcController &) = delete;

    void refresh(Completion completion) override;
    void save(Config config, Completion completion) override;
    void activate(std::string group_id, Completion completion) override;
    void startup(bool enabled, Completion completion) override;
    void shutdown(Completion completion) override;
    void test(std::string endpoint_id, Completion completion) override;
    void calibrate(std::string group_id, std::string microphone_id, Completion completion) override;
    void cancel(std::string group_id, Completion completion) override;

  private:
    void enqueue(std::function<void()> task);
    Snapshot read_snapshot();
    std::unique_ptr<DeviceWatcher> device_watcher_;
    std::vector<Endpoint> cached_outputs_, cached_microphones_;
    std::chrono::steady_clock::time_point device_refresh_{};
    uint64_t device_revision_{};
    bool start_engine_process();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_{};
    std::chrono::steady_clock::time_point last_engine_launch_{};
    bool lite_{};
    std::thread worker_;
};
} // namespace omni::ui
