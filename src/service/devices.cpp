#include <omni/devices.hpp>
#include <initguid.h>
#include <devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <cfgmgr32.h>
namespace omni
{
VirtualDevices::~VirtualDevices()
{
    for (auto &[id, d] : devices_)
        if (d.handle)
            SwDeviceClose(d.handle);
}
namespace
{
struct Creation
{
    Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT result{E_PENDING};
    std::wstring instance;
};
void CALLBACK created(HSWDEVICE, HRESULT result, PVOID context, PCWSTR instance)
{
    auto &c = *static_cast<Creation *>(context);
    c.result = result;
    if (instance)
        c.instance = instance;
    SetEvent(c.done.get());
}
std::vector<std::string> persistent_groups()
{
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG count{};
    if (CM_Get_Device_ID_List_SizeW(&count, L"SWD\\OmniSound", flags) != CR_SUCCESS || count > 1024 * 1024)
        throw std::runtime_error("Cannot enumerate persistent OmniSound devices");
    std::vector<wchar_t> ids(count + 1);
    if (CM_Get_Device_ID_ListW(L"SWD\\OmniSound", ids.data(), count, flags) != CR_SUCCESS)
        throw std::runtime_error("OmniSound device enumeration changed; retry required");
    std::vector<std::string> result;
    for (auto *id = ids.data(); *id; id += wcslen(id) + 1)
    {
        DEVINST node{};
        if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        DEVPROPKEY key{device_property_set, 2};
        DEVPROPTYPE type{};
        wchar_t group[64]{};
        ULONG bytes = sizeof(group);
        if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(group), &bytes, 0) ==
                CR_SUCCESS &&
            type == DEVPROP_TYPE_STRING && valid_id(utf8(group)))
            result.push_back(utf8(group));
    }
    return result;
}
} // namespace
VirtualDevices::Device VirtualDevices::create(const Group &g)
{
    Creation state;
    auto id = wide(g.id);
    auto name = wide("OmniSound · " + g.name);
    GUID container;
    check(CLSIDFromString((L"{" + id + L"}").c_str(), &container), "Read container identity");
    const wchar_t hardware[] = L"OmniSound\\Group\0";
    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = id.c_str();
    info.pszzHardwareIds = hardware;
    info.pszDeviceDescription = name.c_str();
    info.pContainerId = &container;
    info.CapabilityFlags = SWDeviceCapabilitiesDriverRequired | SWDeviceCapabilitiesSilentInstall;
    DEVPROPERTY props[2]{};
    props[0].CompKey.Key = {device_property_set, 2};
    props[0].Type = DEVPROP_TYPE_STRING;
    props[0].BufferSize = static_cast<ULONG>((id.size() + 1) * sizeof(wchar_t));
    props[0].Buffer = const_cast<wchar_t *>(id.c_str());
    props[1].CompKey.Key = DEVPKEY_Device_FriendlyName;
    props[1].Type = DEVPROP_TYPE_STRING;
    props[1].BufferSize = static_cast<ULONG>((name.size() + 1) * sizeof(wchar_t));
    props[1].Buffer = const_cast<wchar_t *>(name.c_str());
    Device d;
    check(SwDeviceCreate(L"OmniSound", L"HTREE\\ROOT\\0", &info, 2, props, created, &state, &d.handle),
          "Create virtual output device");
    // The callback must have completed before its context can be destroyed. Closing cancels enumeration.
    if (WaitForSingleObject(state.done.get(), 10000) != WAIT_OBJECT_0)
    {
        SwDeviceClose(d.handle);
        // SwDeviceClose guarantees no pending callback can touch state after returning.
        throw std::runtime_error("Virtual output enumeration timed out");
    }
    if (FAILED(state.result))
    {
        SwDeviceClose(d.handle);
        check(state.result, "Enumerate virtual output");
    }
    d.instance = state.instance;
    d.name = g.name;
    // Keep software devnodes across service restarts. Explicit deletion changes the lifetime first.
    auto hr = SwDeviceSetLifetime(d.handle, SWDeviceLifetimeParentPresent);
    if (FAILED(hr))
    {
        SwDeviceClose(d.handle);
        check(hr, "Persist virtual output");
    }
    return d;
}
void VirtualDevices::rename(Device &d, const Group &g)
{
    auto name = wide("OmniSound · " + g.name);
    DEVPROPERTY prop{};
    prop.CompKey.Key = DEVPKEY_Device_FriendlyName;
    prop.Type = DEVPROP_TYPE_STRING;
    prop.BufferSize = static_cast<ULONG>((name.size() + 1) * sizeof(wchar_t));
    prop.Buffer = const_cast<wchar_t *>(name.c_str());
    check(SwDevicePropertySet(d.handle, 1, &prop), "Rename virtual device");
    d.ready = false;
    for (const auto &e : endpoints(false, true))
        if (e.group_id == g.id)
        {
            auto endpoint = get_device(e.id);
            ComPtr<IPropertyStore> store;
            check(endpoint->OpenPropertyStore(STGM_READWRITE, &store), "Open virtual endpoint properties");
            PROPVARIANT v;
            PropVariantInit(&v);
            check(InitPropVariantFromString(name.c_str(), &v), "Prepare endpoint name");
            auto hr = store->SetValue(PKEY_Device_FriendlyName, v);
            PropVariantClear(&v);
            check(hr, "Set Windows endpoint name");
            check(store->Commit(), "Commit Windows endpoint name");
            d.ready = e.active;
        }
    d.name = g.name;
}
void VirtualDevices::reconcile(const std::vector<Group> &groups)
{
    if (!recovered_)
    {
        // Recover the crash window between saving a deletion and closing its persistent devnode.
        try
        {
            for (const auto &id : persistent_groups())
                if (std::none_of(groups.begin(), groups.end(), [&](const Group &g) { return g.id == id; }))
                {
                    auto stale = create(Group{id, "Removed", {}});
                    auto hr = SwDeviceSetLifetime(stale.handle, SWDeviceLifetimeHandle);
                    SwDeviceClose(stale.handle);
                    check(hr, "Remove obsolete virtual output");
                }
            recovered_ = true;
        }
        catch (const std::exception &e)
        {
            log_event("device-recovery", e.what());
        }
    }
    for (auto it = devices_.begin(); it != devices_.end();)
    {
        if (std::none_of(groups.begin(), groups.end(), [&](const Group &g) { return g.id == it->first; }))
        {
            if (it->second.handle)
            {
                auto hr = SwDeviceSetLifetime(it->second.handle, SWDeviceLifetimeHandle);
                if (FAILED(hr))
                {
                    it->second.error = "Virtual device removal will be retried";
                    ++it;
                    continue;
                }
                SwDeviceClose(it->second.handle);
            }
            it = devices_.erase(it);
        }
        else
            ++it;
    }
    for (const auto &g : groups)
    {
        auto found = devices_.find(g.id);
        if (found == devices_.end() || !found->second.handle)
        {
            try
            {
                auto d = create(g);
                found = devices_.insert_or_assign(g.id, std::move(d)).first;
            }
            catch (const std::exception &e)
            {
                devices_[g.id].error = e.what();
                continue;
            }
        }
        auto &d = found->second;
        try
        {
            if (d.name != g.name || !d.ready)
                rename(d, g);
            d.error = d.ready ? "" : "Driver is missing, not signed, or not ready";
        }
        catch (const std::exception &e)
        {
            d.error = e.what();
        }
    }
}
Json VirtualDevices::status() const
{
    Json out = Json::array();
    for (const auto &[id, d] : devices_)
        out.push_back({{"id", id}, {"ready", d.ready}, {"error", d.error}});
    return out;
}
} // namespace omni
