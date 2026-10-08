#include <omni/audio.hpp>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <devicetopology.h>
#include <sstream>
#include <numbers>
#include <cmath>
namespace omni
{
static ComPtr<IMMDeviceEnumerator> enumerator()
{
    ComPtr<IMMDeviceEnumerator> e;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)),
          "Enumerate audio devices");
    return e;
}
ComPtr<IMMDevice> get_device(const std::string &id)
{
    ComPtr<IMMDevice> d;
    check(enumerator()->GetDevice(wide(id).c_str(), &d), "Open audio endpoint");
    return d;
}
float endpoint_volume(const std::string &id)
{
    ComPtr<IAudioEndpointVolume> volume;
    check(get_device(id)->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume),
          "Open Windows device volume");
    float scalar{};
    check(volume->GetMasterVolumeLevelScalar(&scalar), "Read Windows device volume");
    return scalar;
}
void set_endpoint_volume(const std::string &id, float scalar)
{
    ComPtr<IAudioEndpointVolume> volume;
    check(get_device(id)->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume),
          "Open Windows device volume");
    float current{};
    check(volume->GetMasterVolumeLevelScalar(&current), "Read Windows device volume");
    if (std::abs(current - scalar) > 0.001f)
        check(volume->SetMasterVolumeLevelScalar(std::clamp(scalar, 0.f, 1.f), nullptr),
              "Set Windows device volume");
}
Frame loopback_unity_gain(const std::string &id, bool master_only)
{
    ComPtr<IAudioEndpointVolume> volume;
    check(get_device(id)->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume),
          "Open reference volume");
    BOOL mute{};
    check(volume->GetMute(&mute), "Read reference mute");
    if (mute)
        return {};
    DWORD support{};
    check(volume->QueryHardwareSupport(&support), "Read reference volume implementation");
    if (support & ENDPOINT_HARDWARE_SUPPORT_VOLUME)
        return {1, 1};
    if (master_only)
    {
        // Pro can route center/side/rear channels into either stereo lane. Undo
        // only the shared master so channel balance remains in the captured PCM.
        float level{};
        check(volume->GetMasterVolumeLevel(&level), "Read group master attenuation");
        const auto gain = static_cast<float>(std::min(512.0, std::pow(10.0, -level / 20.0)));
        return {gain, gain};
    }
    UINT channels{};
    check(volume->GetChannelCount(&channels), "Read reference channels");
    float left{}, right{};
    check(volume->GetChannelVolumeLevel(0, &left), "Read reference left attenuation");
    check(volume->GetChannelVolumeLevel(channels > 1 ? 1 : 0, &right), "Read reference right attenuation");
    return {static_cast<float>(std::min(512.0, std::pow(10.0, -left / 20.0))),
            static_cast<float>(std::min(512.0, std::pow(10.0, -right / 20.0)))};
}
VolumeWatcher::VolumeWatcher(const std::string &id, std::function<void()> changed)
    : changed_(std::move(changed))
{
    check(get_device(id)->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume_),
          "Watch reference volume");
    check(volume_->RegisterControlChangeNotify(this), "Register reference volume changes");
}
VolumeWatcher::~VolumeWatcher()
{
    volume_->UnregisterControlChangeNotify(this);
}
HRESULT VolumeWatcher::QueryInterface(REFIID id, void **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(IAudioEndpointVolumeCallback))
    {
        *out = static_cast<IAudioEndpointVolumeCallback *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
static std::wstring string_property(IPropertyStore *store, REFPROPERTYKEY key)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}
static Endpoint describe(IMMDevice *d)
{
    Endpoint e;
    LPWSTR id{};
    check(d->GetId(&id), "Read audio identity");
    e.id = utf8(id);
    CoTaskMemFree(id);
    DWORD state{};
    check(d->GetState(&state), "Read audio state");
    e.active = state == DEVICE_STATE_ACTIVE;
    ComPtr<IMMEndpoint> ep;
    check(d->QueryInterface(IID_PPV_ARGS(&ep)), "Read endpoint type");
    EDataFlow flow;
    check(ep->GetDataFlow(&flow), "Read audio direction");
    e.capture = flow == eCapture;
    ComPtr<IPropertyStore> props;
    check(d->OpenPropertyStore(STGM_READ, &props), "Read audio properties");
    e.name = utf8(string_property(props.Get(), PKEY_Device_FriendlyName));
    auto instance = string_property(props.Get(), PKEY_Device_InstanceId);
    if (instance.empty())
    {
        ComPtr<IDeviceTopology> topology;
        ComPtr<IConnector> connector;
        LPWSTR adapter_id{};
        if (SUCCEEDED(d->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topology)) &&
            SUCCEEDED(topology->GetConnector(0, &connector)) &&
            SUCCEEDED(connector->GetDeviceIdConnectedTo(&adapter_id)))
        {
            ComPtr<IMMDevice> adapter;
            ComPtr<IPropertyStore> adapter_props;
            if (SUCCEEDED(enumerator()->GetDevice(adapter_id, &adapter)) &&
                SUCCEEDED(adapter->OpenPropertyStore(STGM_READ, &adapter_props)))
                instance = string_property(adapter_props.Get(), PKEY_Device_InstanceId);
            CoTaskMemFree(adapter_id);
        }
    }
    if (instance.empty())
        instance = L"SWD\\MMDEVAPI\\" + wide(e.id);
    e.instance = utf8(instance);
    DEVINST node;
    if (CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
    {
        for (int i = 0; i < 12; ++i)
        {
            wchar_t devId[MAX_DEVICE_ID_LEN]{};
            if (CM_Get_Device_IDW(node, devId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS)
            {
                auto s = std::wstring(devId);
                if (s.find(L"BTH") != s.npos)
                    e.bluetooth = true;
            }
            DEVPROPKEY key{device_property_set, 2};
            DEVPROPTYPE type{};
            wchar_t group[64]{};
            ULONG n = sizeof(group);
            if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(group), &n, 0) ==
                    CR_SUCCESS &&
                type == DEVPROP_TYPE_STRING)
            {
                auto value = utf8(group);
                if (valid_id(value))
                    e.group_id = value;
            }
            DEVINST parent;
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
                break;
            node = parent;
        }
    }
    ComPtr<IAudioClient> client;
    if (e.active && SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
    {
        WAVEFORMATEX *fmt{};
        if (SUCCEEDED(client->GetMixFormat(&fmt)))
        {
            e.mix_rate = fmt->nSamplesPerSec;
            e.mix_bits = fmt->wBitsPerSample;
            e.mix_channels = fmt->nChannels;
            std::ostringstream s;
            s << e.id << '|' << e.instance << '|' << fmt->wFormatTag << '|' << fmt->nSamplesPerSec << '|'
              << fmt->nChannels << '|' << fmt->wBitsPerSample;
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
            {
                auto *ex = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(fmt);
                s << '|' << ex->dwChannelMask << '|' << ex->SubFormat.Data1;
            }
            e.fingerprint = s.str();
            CoTaskMemFree(fmt);
        }
    }
    return e;
}
Endpoint describe_endpoint(const std::string &id)
{
    return describe(get_device(id).Get());
}
std::string default_render_endpoint()
{
    ComPtr<IMMDevice> device;
    check(enumerator()->GetDefaultAudioEndpoint(eRender, eMultimedia, &device),
          "Read Windows default output");
    LPWSTR id{};
    check(device->GetId(&id), "Read Windows default output identity");
    std::string result;
    try
    {
        result = utf8(id);
    }
    catch (...)
    {
        CoTaskMemFree(id);
        throw;
    }
    CoTaskMemFree(id);
    return result;
}
std::vector<Endpoint> endpoints(bool capture, bool inactive)
{
    ComPtr<IMMDeviceCollection> list;
    check(enumerator()->EnumAudioEndpoints(capture ? eCapture : eRender,
                                           inactive ? DEVICE_STATEMASK_ALL : DEVICE_STATE_ACTIVE, &list),
          "List audio endpoints");
    UINT n;
    check(list->GetCount(&n), "Count endpoints");
    std::vector<Endpoint> out;
    for (UINT i = 0; i < n; ++i)
    {
        ComPtr<IMMDevice> d;
        if (SUCCEEDED(list->Item(i, &d)))
            try
            {
                out.push_back(describe(d.Get()));
            }
            catch (const std::exception &e)
            {
                log_event("endpoint-enumeration", e.what());
            }
    }
    return out;
}
void to_json(Json &j, const Endpoint &e)
{
    j = {{"id", e.id},
         {"name", e.name},
         {"instance", e.instance},
         {"groupId", e.group_id},
         {"fingerprint", e.fingerprint},
         {"bluetooth", e.bluetooth},
         {"capture", e.capture},
         {"active", e.active},
         {"mixRate", e.mix_rate},
         {"mixBits", e.mix_bits},
         {"mixChannels", e.mix_channels}};
}
WAVEFORMATEXTENSIBLE stereo_format()
{
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = 48000;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = 8;
    f.Format.nAvgBytesPerSec = 384000;
    f.Format.cbSize = 22;
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}
DeviceWatcher::DeviceWatcher() : enumerator_(enumerator())
{
    check(enumerator_->RegisterEndpointNotificationCallback(this), "Watch audio devices");
}
DeviceWatcher::~DeviceWatcher()
{
    enumerator_->UnregisterEndpointNotificationCallback(this);
}
HRESULT DeviceWatcher::QueryInterface(REFIID id, void **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient))
    {
        *out = static_cast<IMMNotificationClient *>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
RenderStream::RenderStream(const std::string &id, bool prefer_raw)
    : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    auto f = stereo_format();
    HRESULT initialized = E_FAIL;
    for (unsigned attempt = 0; attempt < (prefer_raw ? 2u : 1u); ++attempt)
    {
        const bool raw = prefer_raw && attempt == 0;
        auto activate = [&] {
            client_.Reset();
            check(get_device(id)->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_),
                  "Activate output");
            if (!raw)
                return S_OK;
            ComPtr<IAudioClient2> properties_client;
            auto hr = client_.As(&properties_client);
            if (FAILED(hr))
                return hr;
            AudioClientProperties properties{};
            properties.cbSize = sizeof(properties);
            properties.eCategory = AudioCategory_Media;
            properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
            return properties_client->SetClientProperties(&properties);
        };
        initialized = activate();
        if (FAILED(initialized))
            continue;
        ComPtr<IAudioClient3> low_latency;
        UINT32 normal{}, fundamental{}, minimum{}, maximum{};
        if (SUCCEEDED(client_.As(&low_latency)) && SUCCEEDED(low_latency->GetSharedModeEnginePeriod(
                                                       &f.Format, &normal, &fundamental, &minimum, &maximum)))
            initialized = low_latency->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum,
                                                                   &f.Format, nullptr);
        else
            initialized = E_FAIL;
        if (FAILED(initialized))
        {
            // Failed period/RAW negotiation is retried with a fresh client;
            // unsupported RAW outputs still use the normal compatible stream.
            low_latency.Reset();
            initialized = activate();
            if (SUCCEEDED(initialized))
                initialized = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                  0, 0, &f.Format, nullptr);
        }
        if (SUCCEEDED(initialized))
        {
            raw_ = raw;
            break;
        }
    }
    check(initialized, "Initialize shared output");
    check(client_->SetEventHandle(event_.get()), "Set output event");
    check(client_->GetBufferSize(&buffer_frames_), "Read output buffer size");
    check(client_->GetService(IID_PPV_ARGS(&render_)), "Open output buffer");
    check(client_->GetService(IID_PPV_ARGS(&clock_)), "Read output clock");
    check(clock_->GetFrequency(&frequency_), "Read output frequency");
    REFERENCE_TIME latency{};
    check(client_->GetStreamLatency(&latency), "Read output latency");
    latency_ = std::max(static_cast<double>(latency) / 1e7, static_cast<double>(buffer_frames_) / 48000);
}
RenderStream::~RenderStream()
{
    if (started_)
        client_->Stop();
}
UINT32 RenderStream::available()
{
    UINT32 padding;
    check(client_->GetCurrentPadding(&padding), "Read output padding");
    if (padding > buffer_frames_)
        throw std::runtime_error("Invalid output padding");
    if (started_ && !padding && !starvation_pending_ &&
        qpc_seconds() - last_write_time_ > static_cast<double>(buffer_frames_) / 48000 * 1.25)
    {
        // Some drivers pause their position counter when an empty stream stops
        // contributing frames. Queue/write-time evidence still detects that gap.
        starvation_pending_ = true;
        ++starvations_;
        fit_.reset();
        update_clock();
    }
    return buffer_frames_ - padding;
}
Frame *RenderStream::acquire(UINT32 n)
{
    BYTE *bytes{};
    check(render_->GetBuffer(n, &bytes), "Acquire output buffer");
    return reinterpret_cast<Frame *>(bytes);
}
void RenderStream::release(UINT32 n, bool silence)
{
    check(render_->ReleaseBuffer(n, silence ? AUDCLNT_BUFFERFLAGS_SILENT : 0), "Release output buffer");
    submitted_ += n;
    last_write_time_ = qpc_seconds();
    starvation_pending_ = false;
}
void RenderStream::start()
{
    auto n = available();
    acquire(n);
    release(n, true);
    check(client_->Start(), "Start output");
    started_ = true;
}
bool RenderStream::update_clock()
{
    UINT64 pos{}, qpc{};
    auto hr = clock_->GetPosition(&pos, &qpc);
    check(hr, "Read output position");
    if (hr == S_FALSE || !pos || !frequency_)
        return false;
    double played = static_cast<double>(pos) * 48000 / frequency_;
    // A late scheduler wake can drain the Windows render queue completely. The
    // position can keep advancing while submitted_ does not. Rebase it then;
    // available() also detects drivers whose position stalls with the queue.
    if (played > static_cast<double>(submitted_))
    {
        submitted_ = static_cast<uint64_t>(std::ceil(played));
        if (!starvation_pending_)
            ++starvations_;
        starvation_pending_ = true;
    }
    double time = static_cast<double>(qpc) / 1e7;
    bool fitted = fit_.observe(played, time);
    if (!fitted && !fit_.has_position())
        fit_.observe(played, time); // First valid anchor after a timestamp/queue reset.
    return fitted;
}
double RenderStream::next_time() const
{
    return fit_.time_of(static_cast<double>(submitted_));
}
CaptureStream::CaptureStream(const std::string &id, bool loopback, bool preserve_channels)
    : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    check(get_device(id)->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_),
          "Activate capture");
    auto f = stereo_format();
    if (preserve_channels)
    {
        WAVEFORMATEX *mix{};
        check(client_->GetMixFormat(&mix), "Read Pro input format");
        channels_ = mix->nChannels;
        mask_ = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22
                    ? reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->dwChannelMask
                : channels_ == 2 ? 3u
                                 : 0u;
        CoTaskMemFree(mix);
        if (!((channels_ == 2 && mask_ == 3) || (channels_ == 6 && (mask_ == 0x3f || mask_ == 0x60f)) ||
              (channels_ == 8 && mask_ == 0x63f)))
            throw std::runtime_error("Pro input must be stereo, 5.1 or 7.1 PCM");
        f.Format.nChannels = static_cast<WORD>(channels_);
        f.Format.nBlockAlign = static_cast<WORD>(channels_ * sizeof(float));
        f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
        f.dwChannelMask = mask_;
    }
    check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                  (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0),
                              0, 0, &f.Format, nullptr),
          "Initialize capture");
    check(client_->SetEventHandle(event_.get()), "Set capture event");
    check(client_->GetService(IID_PPV_ARGS(&capture_)), "Open capture buffer");
}
CaptureStream::~CaptureStream()
{
    if (started_)
        client_->Stop();
}
void CaptureStream::start()
{
    check(client_->Start(), "Start capture");
    started_ = true;
}
bool CaptureStream::next(CapturePacket &p)
{
    UINT32 n;
    check(capture_->GetNextPacketSize(&n), "Read capture packet size");
    if (!n)
        return false;
    BYTE *data{};
    UINT64 qpc{};
    check(capture_->GetBuffer(&data, &p.frames, &p.flags, &p.position, &qpc), "Read capture packet");
    p.data = channels_ == 2 ? reinterpret_cast<const Frame *>(data) : nullptr;
    p.samples = reinterpret_cast<const float *>(data);
    p.channels = channels_;
    p.channel_mask = mask_;
    p.time = static_cast<double>(qpc) / 1e7;
    return true;
}
void CaptureStream::release(UINT32 n)
{
    check(capture_->ReleaseBuffer(n), "Release capture packet");
}
AudioPriority::AudioPriority()
{
    DWORD task{};
    handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    if (handle_)
        AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
}
AudioPriority::~AudioPriority()
{
    if (handle_)
        AvRevertMmThreadCharacteristics(handle_);
}
void play_test(const std::string &id, std::atomic<bool> &cancel)
{
    auto e = describe_endpoint(id);
    if (!e.group_id.empty())
        throw std::runtime_error("Choose a physical output for identification");
    RenderStream r(id);
    r.start();
    AudioPriority priority;
    uint64_t phase = 0;
    auto end = qpc_seconds() + 1.2;
    while (qpc_seconds() < end && !cancel)
    {
        if (WaitForSingleObject(r.event(), 100) != WAIT_OBJECT_0)
            continue;
        auto n = r.available();
        if (!n)
            continue;
        auto *out = r.acquire(n);
        for (UINT32 i = 0; i < n; ++i, ++phase)
        {
            double t = static_cast<double>(phase) / 48000;
            double env = std::min({1.0, t / 0.02, std::max(0.0, (1.0 - t) / 0.02)});
            float v = static_cast<float>(0.08 * env * std::sin(2 * std::numbers::pi * 660 * t));
            out[i] = {v, v};
        }
        r.release(n);
    }
}
} // namespace omni
