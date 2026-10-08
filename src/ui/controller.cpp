#include "pch.h"
#include "controller.hpp"
#include <omni/ipc.hpp>

namespace omni::ui
{
IpcController::IpcController(bool lite)
    : lite_(lite), worker_([this] {
          ComScope worker_com;
          for (;;)
          {
              std::function<void()> task;
              {
                  std::unique_lock lock(mutex_);
                  ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                  if (stopping_)
                  {
                      device_watcher_.reset();
                      return;
                  }
                  task = std::move(tasks_.front());
                  tasks_.pop_front();
              }
              try
              {
                  task();
              }
              catch (...)
              {
                  // A broken completion must not permanently stop IPC servicing.
              }
          }
      })
{
}

IpcController::~IpcController()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        tasks_.clear();
    }
    ready_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

void IpcController::enqueue(std::function<void()> task)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            throw std::runtime_error("Audio controller is shutting down");
        tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
}

Snapshot IpcController::read_snapshot()
{
    Snapshot result;
    try
    {
        ComScope com;
        if (!device_watcher_)
            device_watcher_ = std::make_unique<DeviceWatcher>();
        const auto now = std::chrono::steady_clock::now();
        const auto revision = device_watcher_->revision();
        if (device_refresh_ == std::chrono::steady_clock::time_point{} || revision != device_revision_ ||
            now >= device_refresh_)
        {
            cached_outputs_ = endpoints(false, true);
            cached_microphones_ = endpoints(true);
            device_revision_ = revision;
            device_refresh_ = now + std::chrono::seconds(30);
        }
        result.outputs = cached_outputs_;
        result.microphones = cached_microphones_;
        if (lite_)
            try
            {
                result.default_render_id = default_render_endpoint();
            }
            catch (const std::exception &e)
            {
                if (!result.error.empty())
                    result.error += "; ";
                result.error += e.what();
            }
    }
    catch (const std::exception &e)
    {
        result.error = e.what();
    }

    try
    {
        if (lite_)
        {
            try
            {
                result.config = request(engine_pipe(true), {{"op", "load"}}, 1500).at("config").get<Config>();
            }
            catch (...)
            {
                start_engine_process();
                result.config = request(engine_pipe(true), {{"op", "load"}}, 3000).at("config").get<Config>();
            }
        }
        else
            result.config = request(service_pipe, {{"op", "load"}}, 2000).at("config").get<Config>();
        result.service_available = true;
    }
    catch (const std::exception &e)
    {
        if (!result.error.empty())
            result.error += "; ";
        result.error += e.what();
    }

    try
    {
        result.engine_status = request(engine_pipe(lite_), {{"op", "status"}}, 1500);
    }
    catch (const std::exception &e)
    {
        auto message = std::string(e.what());
        if (result.service_available && start_engine_process())
        {
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                Sleep(250);
                try
                {
                    result.engine_status = request(engine_pipe(lite_), {{"op", "status"}}, 750);
                    return result;
                }
                catch (const std::exception &retry_error)
                {
                    message = retry_error.what();
                }
            }
        }
        result.engine_status = {{"engine", {{"state", "disconnected"}}},
                                {"job", {{"state", "unavailable"}, {"message", std::move(message)}}}};
    }
    return result;
}

bool IpcController::start_engine_process()
{
    const auto now = std::chrono::steady_clock::now();
    if (last_engine_launch_ != std::chrono::steady_clock::time_point{} &&
        now - last_engine_launch_ < std::chrono::seconds(10))
        return false;
    last_engine_launch_ = now;

    std::wstring image(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!length || length >= image.size())
        return false;
    image.resize(length);
    const auto engine = std::filesystem::path(image).parent_path() / L"OmniSoundEngine.exe";
    const auto attributes = GetFileAttributesW(engine.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    std::wstring command = L"\"" + engine.wstring() + L"\"" + (lite_ ? L" --lite" : L"");
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        engine.parent_path().c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void IpcController::refresh(Completion completion)
{
    enqueue([this, completion = std::move(completion)]() mutable { completion(read_snapshot()); });
}

void IpcController::save(Config config, Completion completion)
{
    enqueue([this, config = std::move(config), completion = std::move(completion)]() mutable {
        try
        {
            if (lite_)
            {
                config = request(engine_pipe(true), {{"op", "save"}, {"config", config}}, 3000)
                             .at("config")
                             .get<Config>();
            }
            else
            {
                (void)request(service_pipe, {{"op", "save"}, {"config", config}}, 3000);
                try
                {
                    (void)request(engine_pipe(false), {{"op", "reload"}}, 1500);
                }
                catch (...)
                { /* Persisted configuration is still picked up on reconnect. */
                }
            }
            auto result = read_snapshot();
            result.save_committed = true;
            completion(std::move(result));
        }
        catch (const std::exception &e)
        {
            auto result = read_snapshot();
            result.error = e.what();
            completion(std::move(result));
        }
    });
}

void IpcController::activate(std::string group_id, Completion completion)
{
    enqueue([this, group_id = std::move(group_id), completion = std::move(completion)]() mutable {
        std::string error;
        try
        {
            (void)request(engine_pipe(lite_), {{"op", "activate"}, {"groupId", group_id}}, 3000);
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        auto result = read_snapshot();
        result.activation_committed = error.empty();
        result.error = std::move(error);
        completion(std::move(result));
    });
}
void IpcController::startup(bool enabled, Completion completion)
{
    enqueue([this, enabled, completion = std::move(completion)]() mutable {
        std::string error;
        try
        {
            (void)request(engine_pipe(lite_), {{"op", "startup"}, {"enabled", enabled}}, 3000);
        }
        catch (const std::exception &e)
        {
            error = e.what();
        }
        auto result = read_snapshot();
        result.error = std::move(error);
        completion(std::move(result));
    });
}
void IpcController::shutdown(Completion completion)
{
    enqueue([this, completion = std::move(completion)]() mutable {
        Snapshot result;
        try
        {
            (void)request(engine_pipe(lite_), {{"op", "shutdown"}}, 2000);
        }
        catch (const std::exception &e)
        {
            result.error = e.what();
        }
        completion(std::move(result));
    });
}
void IpcController::test(std::string endpoint_id, Completion completion)
{
    enqueue([this, endpoint_id = std::move(endpoint_id), completion = std::move(completion)]() mutable {
        Snapshot result;
        try
        {
            (void)request(engine_pipe(lite_), {{"op", "test"}, {"endpointId", endpoint_id}}, 2000);
            result = read_snapshot();
        }
        catch (const std::exception &e)
        {
            result = read_snapshot();
            result.error = e.what();
        }
        completion(std::move(result));
    });
}

void IpcController::calibrate(std::string group_id, std::string microphone_id, Completion completion)
{
    enqueue([this, group_id = std::move(group_id), microphone_id = std::move(microphone_id),
             completion = std::move(completion)]() mutable {
        Snapshot result;
        try
        {
            (void)request(engine_pipe(lite_),
                          {{"op", "calibrate"}, {"groupId", group_id}, {"microphoneId", microphone_id}},
                          2000);
            result = read_snapshot();
        }
        catch (const std::exception &e)
        {
            result = read_snapshot();
            result.error = e.what();
        }
        completion(std::move(result));
    });
}

void IpcController::cancel(std::string, Completion completion)
{
    enqueue([this, completion = std::move(completion)]() mutable {
        Snapshot result;
        try
        {
            (void)request(engine_pipe(lite_), {{"op", "cancel"}}, 1500);
            result = read_snapshot();
        }
        catch (const std::exception &e)
        {
            result = read_snapshot();
            result.error = e.what();
        }
        completion(std::move(result));
    });
}
} // namespace omni::ui
