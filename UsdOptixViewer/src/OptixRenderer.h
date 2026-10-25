// Ray traces a FlatGeometry with OptiX into a host-side RGBA image.
#pragma once

#include <string>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>
#include <optix_types.h>

#include "LaunchParams.h"

struct FlatGeometry;

class OptixRenderer
{
public:
    OptixRenderer() = default;
    ~OptixRenderer();
    OptixRenderer(const OptixRenderer&) = delete;
    OptixRenderer& operator=(const OptixRenderer&) = delete;

    // Creates the CUDA/OptiX context and pipeline. Returns false and sets
    // `error` on failure.
    bool init(std::string& error);
    const std::string& deviceName() const { return m_deviceName; }

    void setGeometry(const FlatGeometry& geometry);
    void resize(unsigned int width, unsigned int height);
    void setCamera(const float eye[3], const float target[3], const float up[3], float fovYDeg);
    void setLightDir(const float dir[3]);
    void setShadows(bool on) { m_params.shadows = on ? 1u : 0u; }
    void setMipmaps(bool on) { m_params.mipmaps = on ? 1u : 0u; }
    void setNormalMaps(bool on) { m_params.normalMaps = on ? 1u : 0u; }
    void setReflections(bool on) { m_params.reflections = on ? 1u : 0u; }
    void setSelectedMesh(unsigned int meshId) { m_params.selectedMesh = meshId; }

    // Adds one progressive sample per pixel and copies the average to the
    // host buffers. Call resetAccumulation() after changing anything.
    void render();
    void resetAccumulation() { m_params.frameIndex = 0; }
    unsigned int samples() const { return m_params.frameIndex; }

    unsigned int width() const { return m_params.width; }
    unsigned int height() const { return m_params.height; }
    const std::vector<uchar4>& pixels() const { return m_pixels; }
    unsigned int meshIdAt(unsigned int x, unsigned int y) const;
    float lastRenderMs() const { return m_lastRenderMs; }

private:
    struct DeviceBuffer
    {
        CUdeviceptr ptr = 0;
        size_t size = 0;
        void resize(size_t bytes);
        void upload(const void* data, size_t bytes);
        void free();
    };

    void createPipeline();
    void buildSbt();
    void uploadTextures(const FlatGeometry& geometry);
    void freeTextures();

    std::string m_deviceName;
    CUcontext m_cuContext = nullptr;
    CUstream m_stream = nullptr;
    OptixDeviceContext m_context = nullptr;
    OptixModule m_module = nullptr;
    OptixPipeline m_pipeline = nullptr;
    OptixProgramGroup m_raygenPG = nullptr, m_missPG[RAY_TYPE_COUNT] = {}, m_hitPG = nullptr;
    OptixShaderBindingTable m_sbt = {};
    DeviceBuffer m_raygenRecord, m_missRecords, m_hitRecords;

    DeviceBuffer m_vertices, m_indices, m_triMesh, m_triMaterial, m_triUVs, m_triNormals, m_materials, m_gas;
    DeviceBuffer m_colorBuffer, m_accumBuffer, m_meshIdBuffer, m_paramsBuffer;
    std::vector<cudaMipmappedArray_t> m_textureArrays;
    std::vector<cudaTextureObject_t> m_textureObjects;
    DeviceBuffer m_textureTable;
    LaunchParams m_params = {};

    std::vector<uchar4> m_pixels;
    std::vector<unsigned int> m_meshIds;
    float m_lastRenderMs = 0.0f;
};
