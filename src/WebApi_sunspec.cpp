// SPDX-License-Identifier: GPL-2.0-or-later
#include "WebApi_sunspec.h"
#include "Configuration.h"
#include "SunSpecServer.h"
#include "WebApi.h"
#include "WebApi_errors.h"
#include "helper.h"
#include <AsyncJson.h>

void WebApiSunSpecClass::init(AsyncWebServer& server, Scheduler& /* scheduler */)
{
    using std::placeholders::_1;

    server.on("/api/sunspec/config", HTTP_GET,  std::bind(&WebApiSunSpecClass::onSunSpecAdminGet,  this, _1));
    server.on("/api/sunspec/config", HTTP_POST, std::bind(&WebApiSunSpecClass::onSunSpecAdminPost, this, _1));
}

void WebApiSunSpecClass::onSunSpecAdminGet(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();
    const CONFIG_T& config = Configuration.get();

    root["sunspec_enabled"]            = config.SunSpec.Enabled;
    root["sunspec_device_name"]        = config.SunSpec.DeviceName;
    root["sunspec_power_limit_enabled"] = config.SunSpec.PowerLimitEnabled;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiSunSpecClass::onSunSpecAdminPost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    auto& retMsg = response->getRoot();

    if (!(root["sunspec_enabled"].is<bool>()
            && root["sunspec_device_name"].is<String>()
            && root["sunspec_power_limit_enabled"].is<bool>())) {
        retMsg["message"] = "Values are missing!";
        retMsg["code"] = WebApiError::GenericValueMissing;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    if (root["sunspec_enabled"].as<bool>()) {
        const String deviceName = root["sunspec_device_name"].as<String>();
        if (deviceName.length() == 0 || deviceName.length() > SUNSPEC_MAX_DEVICE_NAME_STRLEN) {
            retMsg["message"] = "Device name must be between 1 and 31 characters!";
            retMsg["code"] = WebApiError::SunSpecDeviceNameLength;
            retMsg["param"]["max"] = SUNSPEC_MAX_DEVICE_NAME_STRLEN;
            WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
            return;
        }
    }

    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();

        config.SunSpec.Enabled            = root["sunspec_enabled"].as<bool>();
        config.SunSpec.PowerLimitEnabled  = root["sunspec_power_limit_enabled"].as<bool>();
        strlcpy(config.SunSpec.DeviceName,
            root["sunspec_device_name"].as<String>().c_str(),
            sizeof(config.SunSpec.DeviceName));
    }

    WebApi.writeConfig(retMsg);
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}
