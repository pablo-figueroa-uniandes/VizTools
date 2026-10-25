#include "CloudXrService.h"

#include <cstdlib>
#include <filesystem>
#include <type_traits>

#include <csignal>

#include <dlfcn.h>

CloudXrService::~CloudXrService()
{
    stop();
}

bool CloudXrService::start(const std::string& sdkDir, const std::string& deviceProfile, std::string& error)
{
    const std::filesystem::path dir(sdkDir);
    const std::filesystem::path manifest = dir / "openxr_cloudxr.json";
    if (!std::filesystem::exists(manifest))
    {
        error = "No CloudXR SDK in " + sdkDir + " (openxr_cloudxr.json not found)";
        return false;
    }
    // the library finds its own dependencies next to it (RUNPATH $ORIGIN)
    m_library = dlopen((dir / "libcloudxr.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_library)
    {
        error = std::string("Could not load libcloudxr.so: ") + dlerror();
        return false;
    }
    auto resolve = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(m_library, name));
        if (!fn)
            error = std::string("libcloudxr.so lacks ") + name;
        return fn != nullptr;
    };
    if (!resolve(m_getRuntimeVersion, "nv_cxr_get_runtime_version") || !resolve(m_create, "nv_cxr_service_create") ||
        !resolve(m_setStringProperty, "nv_cxr_service_set_string_property") ||
        !resolve(m_start, "nv_cxr_service_start") || !resolve(m_stop, "nv_cxr_service_stop") ||
        !resolve(m_join, "nv_cxr_service_join") || !resolve(m_destroy, "nv_cxr_service_destroy") ||
        !resolve(m_pollEvent, "nv_cxr_service_poll_event"))
    {
        stop();
        return false;
    }
    uint32_t major = 0, minor = 0, patch = 0;
    m_getRuntimeVersion(&major, &minor, &patch);
    m_version = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);

    nv_cxr_result_t result = m_create(&m_service);
    if (result == NV_CXR_SUCCESS && !deviceProfile.empty())
    {
        const std::string name = "device-profile";
        result = m_setStringProperty(m_service, name.data(), name.size(), deviceProfile.data(), deviceProfile.size());
    }
    if (result == NV_CXR_SUCCESS)
    {
        // The service installs SIGINT/SIGTERM handlers that stop it while the
        // app still uses it. Give the signals back to SDL, whose quit event
        // ends the session first and then stops the service.
        struct sigaction interrupt{}, terminate{};
        sigaction(SIGINT, nullptr, &interrupt);
        sigaction(SIGTERM, nullptr, &terminate);
        result = m_start(m_service);
        sigaction(SIGINT, &interrupt, nullptr);
        sigaction(SIGTERM, &terminate, nullptr);
    }
    if (result != NV_CXR_SUCCESS)
    {
        error = "The CloudXR service did not start (error " + std::to_string(int(result)) +
                "). Is another CloudXR runtime running? The service's messages are on the console.";
        if (m_service)
        {
            m_destroy(m_service);
            m_service = nullptr;
        }
        stop();
        return false;
    }
    // the OpenXR loader reads this when the instance is created
    setenv("XR_RUNTIME_JSON", manifest.c_str(), 1);
    return true;
}

void CloudXrService::stop()
{
    if (m_service)
    {
        m_stop(m_service);
        m_join(m_service);
        m_destroy(m_service);
        m_service = nullptr;
    }
    if (m_library)
    {
        dlclose(m_library);
        m_library = nullptr;
    }
    m_clientConnected = false;
}

bool CloudXrService::clientConnected()
{
    if (!m_service)
        return false;
    // poll_event reports "no event" as a success with type NV_CXR_EVENT_NONE
    nv_cxr_event_t event{};
    while (m_pollEvent(m_service, &event) == NV_CXR_SUCCESS && event.type != NV_CXR_EVENT_NONE)
    {
        if (event.type == NV_CXR_EVENT_CLOUDXR_CLIENT_CONNECTED)
            m_clientConnected = true;
        else if (event.type == NV_CXR_EVENT_CLOUDXR_CLIENT_DISCONNECTED)
            m_clientConnected = false;
        event = {};
    }
    return m_clientConnected;
}
