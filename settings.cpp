#include "settings.hpp"

#include <ipmid/utils.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/message/types.hpp>
#include <xyz/openbmc_project/Common/error.hpp>

namespace settings
{

using namespace phosphor::logging;
using namespace sdbusplus::xyz::openbmc_project::Common::Error;

constexpr auto mapperService = "xyz.openbmc_project.ObjectMapper";
constexpr auto mapperPath = "/xyz/openbmc_project/object_mapper";
constexpr auto mapperIntf = "xyz.openbmc_project.ObjectMapper";

Objects::Objects(sdbusplus::bus_t& bus, const std::vector<Interface>& filter) :
    bus(bus)
{
    auto depth = 0;

    auto mapperCall = bus.new_method_call(mapperService, mapperPath, mapperIntf,
                                          "GetSubTree");
    mapperCall.append(root);
    mapperCall.append(depth);
    mapperCall.append(filter);

    using Interfaces = std::vector<Interface>;
    using MapperResponse = std::map<Path, std::map<Service, Interfaces>>;
    MapperResponse result;
    try
    {
        auto response = bus.call(mapperCall);
        response.read(result);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Error in mapper GetSubTree",
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }

    for (auto& iter : result)
    {
        const auto& path = iter.first;
        for (auto& interface : iter.second.begin()->second)
        {
            auto found = map.find(interface);
            if (map.end() != found)
            {
                auto& paths = found->second;
                paths.push_back(path);
            }
            else
            {
                map.emplace(std::move(interface), std::vector<Path>({path}));
            }
        }
    }
}

Service Objects::service(const Path& path, const Interface& interface) const
{
    using Interfaces = std::vector<Interface>;
    auto mapperCall = bus.new_method_call(mapperService, mapperPath, mapperIntf,
                                          "GetObject");
    mapperCall.append(path);
    mapperCall.append(Interfaces({interface}));

    std::map<Service, Interfaces> result;
    try
    {
        auto response = bus.call(mapperCall);
        response.read(result);
        return result.begin()->first;
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Invalid response from mapper",
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }
}

namespace boot
{

std::tuple<Path, OneTimeEnabled> setting(const Objects& objects,
                                         const Interface& iface)
{
    constexpr auto bootObjCount = 2;
    constexpr auto oneTime = "one_time";
    constexpr auto enabledIntf = "xyz.openbmc_project.Object.Enable";

    const std::vector<Path>& paths = objects.map.at(iface);
    auto count = paths.size();
    if (count != bootObjCount)
    {
        log<level::ERR>("Exactly two objects expected",
                        entry("INTERFACE=%s", iface.c_str()),
                        entry("COUNT=%d", count));
        elog<InternalFailure>();
    }
    size_t index = 0;
    if (std::string::npos == paths[0].rfind(oneTime))
    {
        index = 1;
    }
    const Path& oneTimeSetting = paths[index];
    const Path& regularSetting = paths[!index];

    auto method = objects.bus.new_method_call(
        objects.service(oneTimeSetting, iface).c_str(), oneTimeSetting.c_str(),
        ipmi::PROP_INTF, "Get");
    method.append(enabledIntf, "Enabled");

    std::variant<bool> enabled;
    try
    {
        auto reply = objects.bus.call(method);
        reply.read(enabled);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("Error in getting Enabled property",
                        entry("OBJECT=%s", oneTimeSetting.c_str()),
                        entry("INTERFACE=%s", iface.c_str()),
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }

    auto oneTimeEnabled = std::get<bool>(enabled);
    const Path& setting = oneTimeEnabled ? oneTimeSetting : regularSetting;
    return std::make_tuple(setting, oneTimeEnabled);
}

} // namespace boot

} // namespace settings
