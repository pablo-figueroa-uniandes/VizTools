// Runs the NVIDIA CloudXR runtime inside this process, from the libraries of
// a CloudXR SDK, and points the OpenXR loader at it. The service must be
// started before the OpenXR instance is created.
#pragma once

#include <string>

#include "third_party/cxrServiceAPI.h"

class CloudXrService
{
public:
    CloudXrService() = default;
    ~CloudXrService();
    CloudXrService(const CloudXrService&) = delete;
    CloudXrService& operator=(const CloudXrService&) = delete;

    // sdkDir holds libcloudxr.so and openxr_cloudxr.json. deviceProfile is a
    // CloudXR device profile ("apple-vision-pro", "auto-webrtc", ...), or empty
    // for the runtime's default.
    bool start(const std::string& sdkDir, const std::string& deviceProfile, std::string& error);
    void stop();
    bool running() const { return m_service != nullptr; }
    const std::string& version() const { return m_version; }

    // Drains the service's events; true while a CloudXR client is connected.
    bool clientConnected();

private:
    void* m_library = nullptr;
    nv_cxr_service* m_service = nullptr;
    std::string m_version;
    bool m_clientConnected = false;

    PFN_nv_cxr_get_runtime_version m_getRuntimeVersion = nullptr;
    PFN_nv_cxr_service_create m_create = nullptr;
    PFN_nv_cxr_service_set_string_property m_setStringProperty = nullptr;
    PFN_nv_cxr_service_start m_start = nullptr;
    PFN_nv_cxr_service_stop m_stop = nullptr;
    PFN_nv_cxr_service_join m_join = nullptr;
    PFN_nv_cxr_service_destroy m_destroy = nullptr;
    PFN_nv_cxr_service_poll_event m_pollEvent = nullptr;
};
