#include "watchdog_service.hpp"

#include <ipmid/api.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/State/Watchdog/server.hpp>

#include <exception>
#include <stdexcept>
#include <string>

using phosphor::logging::elog;
using phosphor::logging::entry;
using phosphor::logging::level;
using phosphor::logging::log;
using sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;
using sdbusplus::xyz::openbmc_project::State::server::convertForMessage;
using sdbusplus::xyz::openbmc_project::State::server::Watchdog;

static constexpr char wd_path[] = "/xyz/openbmc_project/watchdog/host0";
static constexpr char wd_intf[] = "xyz.openbmc_project.State.Watchdog";
static constexpr char prop_intf[] = "org.freedesktop.DBus.Properties";

ipmi::ServiceCache WatchdogService::wd_service(wd_intf, wd_path);

WatchdogService::WatchdogService() : bus(ipmid_get_sd_bus_connection()) {}

void WatchdogService::resetTimeRemaining(bool enableWatchdog)
{
    bool wasValid = wd_service.isValid(bus);
    auto request = wd_service.newMethodCall(bus, wd_intf, "ResetTimeRemaining");
    request.append(enableWatchdog);
    try
    {
        auto response = bus.call(request);
    }
    catch (const std::exception& e)
    {
        wd_service.invalidate();
        if (wasValid)
        {
            // Retry the request once in case the cached service was stale
            return resetTimeRemaining(enableWatchdog);
        }
        log<level::ERR>(
            "WatchdogService: Method error resetting time remaining",
            entry("ENABLE_WATCHDOG=%d", !!enableWatchdog),
            entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }
}

WatchdogService::Properties WatchdogService::getProperties()
{
    bool wasValid = wd_service.isValid(bus);
    auto request = wd_service.newMethodCall(bus, prop_intf, "GetAll");
    request.append(wd_intf);

    std::map<std::string, std::variant<bool, uint64_t, std::string>> properties;
    try
    {
        auto response = bus.call(request);
        response.read(properties);
    }
    catch (const std::exception& e)
    {
        wd_service.invalidate();
        if (wasValid)
        {
            // Retry the request once in case the cached service was stale
            return getProperties();
        }
        log<level::ERR>("WatchdogService: Method error getting properties",
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }

    try
    {
        Properties wd_prop;
        wd_prop.initialized = std::get<bool>(properties.at("Initialized"));
        wd_prop.enabled = std::get<bool>(properties.at("Enabled"));
        wd_prop.expireAction = Watchdog::convertActionFromString(
            std::get<std::string>(properties.at("ExpireAction")));
        wd_prop.timerUse = Watchdog::convertTimerUseFromString(
            std::get<std::string>(properties.at("CurrentTimerUse")));
        wd_prop.expiredTimerUse = Watchdog::convertTimerUseFromString(
            std::get<std::string>(properties.at("ExpiredTimerUse")));

        wd_prop.interval = std::get<uint64_t>(properties.at("Interval"));
        wd_prop.timeRemaining =
            std::get<uint64_t>(properties.at("TimeRemaining"));
        return wd_prop;
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("WatchdogService: Decode error in get properties",
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }

    // Needed instead of elog<InternalFailure>() since the compiler can't
    // deduce the that elog<>() always throws
    throw std::runtime_error(
        "WatchdogService: Should not reach end of getProperties");
}

template <typename T>
T WatchdogService::getProperty(const std::string& key)
{
    bool wasValid = wd_service.isValid(bus);
    auto request = wd_service.newMethodCall(bus, prop_intf, "Get");
    request.append(wd_intf, key);
    try
    {
        auto response = bus.call(request);
        std::variant<T> value;
        response.read(value);
        return std::get<T>(value);
    }
    catch (const std::exception& e)
    {
        wd_service.invalidate();
        if (wasValid)
        {
            // Retry the request once in case the cached service was stale
            return getProperty<T>(key);
        }
        log<level::ERR>("WatchdogService: Method error getting property",
                        entry("PROPERTY=%s", key.c_str()),
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }

    // Needed instead of elog<InternalFailure>() since the compiler can't
    // deduce the that elog<>() always throws
    throw std::runtime_error(
        "WatchdogService: Should not reach end of getProperty");
}

template <typename T>
void WatchdogService::setProperty(const std::string& key, const T& val)
{
    bool wasValid = wd_service.isValid(bus);
    auto request = wd_service.newMethodCall(bus, prop_intf, "Set");
    request.append(wd_intf, key, std::variant<T>(val));
    try
    {
        auto response = bus.call(request);
    }
    catch (const std::exception& e)
    {
        wd_service.invalidate();
        if (wasValid)
        {
            // Retry the request once in case the cached service was stale
            setProperty(key, val);
            return;
        }
        log<level::ERR>("WatchdogService: Method error setting property",
                        entry("PROPERTY=%s", key.c_str()),
                        entry("ERROR=%s", e.what()));
        elog<InternalFailure>();
    }
}

bool WatchdogService::getInitialized()
{
    return getProperty<bool>("Initialized");
}

void WatchdogService::setInitialized(bool initialized)
{
    setProperty("Initialized", initialized);
}

void WatchdogService::setEnabled(bool enabled)
{
    setProperty("Enabled", enabled);
}

void WatchdogService::setExpireAction(Action expireAction)
{
    setProperty("ExpireAction", convertForMessage(expireAction));
}

void WatchdogService::setTimerUse(TimerUse timerUse)
{
    setProperty("CurrentTimerUse", convertForMessage(timerUse));
}

void WatchdogService::setExpiredTimerUse(TimerUse timerUse)
{
    setProperty("ExpiredTimerUse", convertForMessage(timerUse));
}

void WatchdogService::setInterval(uint64_t interval)
{
    setProperty("Interval", interval);
}
