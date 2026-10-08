#pragma once
#include <omni/platform.hpp>
#include <omni/dsp.hpp>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <memory>
namespace omni
{
inline constexpr GUID device_property_set = {
    0x2055f0da, 0x4752, 0x4b84, {0xac, 0xea, 0x93, 0xdf, 0x83, 0x63, 0x1a, 0xe2}};
struct Endpoint
{
    std::string id, name, instance, group_id, fingerprint;
    bool bluetooth{}, capture{}, active{};
    unsigned mix_rate{}, mix_bits{}, mix_channels{};
};
void to_json(Json &, const Endpoint &);
std::vector<Endpoint> endpoints(bool capture = false, bool include_inactive = false);
Endpoint describe_endpoint(const std::string &);
std::string default_render_endpoint();
ComPtr<IMMDevice> get_device(const std::string &);
float endpoint_volume(const std::string &);
void set_endpoint_volume(const std::string &, float);
Frame loopback_unity_gain(const std::string &, bool master_only = false);
class VolumeWatcher final : public IAudioEndpointVolumeCallback
{
  public:
    VolumeWatcher(const std::string &, std::function<void()>);
    ~VolumeWatcher();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        return --refs_;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override
    {
        try
        {
            changed_();
        }
        catch (...)
        {
            return E_FAIL;
        }
        return S_OK;
    }

  private:
    ComPtr<IAudioEndpointVolume> volume_;
    std::function<void()> changed_;
    std::atomic<ULONG> refs_{1};
};
WAVEFORMATEXTENSIBLE stereo_format();
class DeviceWatcher final : public IMMNotificationClient
{
  public:
    DeviceWatcher();
    ~DeviceWatcher();
    uint64_t revision() const
    {
        return revision_.load();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override;
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        return --refs_;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        ++revision_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
    {
        ++revision_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        ++revision_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override
    {
        ++revision_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, PROPERTYKEY) override
    {
        ++revision_;
        return S_OK;
    }

  private:
    std::atomic<ULONG> refs_{1};
    std::atomic<uint64_t> revision_{1};
    ComPtr<IMMDeviceEnumerator> enumerator_;
};
class RenderStream
{
  public:
    explicit RenderStream(const std::string &endpoint, bool prefer_raw = false);
    ~RenderStream();
    HANDLE event() const
    {
        return event_.get();
    }
    UINT32 available();
    Frame *acquire(UINT32 frames);
    void release(UINT32 frames, bool silence = false);
    void start();
    bool update_clock();
    double next_time() const;
    double frame_seconds() const
    {
        return fit_.seconds_per_frame();
    }
    bool clock_ready() const
    {
        return fit_.ready();
    }
    bool clock_position_valid() const
    {
        return fit_.has_position();
    }
    double latency_seconds() const
    {
        return latency_;
    }
    uint64_t submitted() const
    {
        return submitted_;
    }
    uint64_t starvations() const
    {
        return starvations_;
    }
    bool raw() const
    {
        return raw_;
    }

  private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    ComPtr<IAudioClock> clock_;
    Handle event_;
    UINT32 buffer_frames_{};
    UINT64 frequency_{};
    uint64_t submitted_{};
    uint64_t starvations_{};
    ClockFit fit_;
    double latency_{};
    bool started_{};
    bool raw_{};
    bool starvation_pending_{};
    double last_write_time_{};
};
struct CapturePacket
{
    const Frame *data{};
    UINT32 frames{};
    DWORD flags{};
    double time{};
    UINT64 position{};
    const float *samples{};
    unsigned channels{2};
    uint32_t channel_mask{3};
};
class CaptureStream
{
  public:
    CaptureStream(const std::string &, bool loopback, bool preserve_channels = false);
    unsigned channels() const
    {
        return channels_;
    }
    uint32_t channel_mask() const
    {
        return mask_;
    }
    ~CaptureStream();
    HANDLE event() const
    {
        return event_.get();
    }
    void start();
    bool next(CapturePacket &);
    void release(UINT32);

  private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    Handle event_;
    bool started_{};
    unsigned channels_{2};
    uint32_t mask_{3};
};
class AudioPriority
{
  public:
    AudioPriority();
    ~AudioPriority();

  private:
    HANDLE handle_{};
};
void play_test(const std::string &, std::atomic<bool> &cancel);
} // namespace omni
