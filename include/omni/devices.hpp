#pragma once
#include <omni/audio.hpp>
#include <swdevice.h>
#include <map>
namespace omni
{
class VirtualDevices
{
  public:
    VirtualDevices() = default;
    ~VirtualDevices();
    void reconcile(const std::vector<Group> &);
    Json status() const;

  private:
    struct Device
    {
        HSWDEVICE handle{};
        std::wstring instance;
        std::string name, error;
        bool ready{};
    };
    std::map<std::string, Device> devices_;
    bool recovered_{};
    Device create(const Group &);
    void rename(Device &, const Group &);
};
} // namespace omni
