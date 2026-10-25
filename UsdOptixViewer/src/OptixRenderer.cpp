#include "OptixRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <cuda_runtime.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include "Scene.h"

extern const char g_devicePrograms[];
extern const size_t g_devicePrograms_size;

namespace {

void check(cudaError_t r, const char* what)
{
    if (r != cudaSuccess)
        throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(r));
}

void check(OptixResult r, const char* what)
{
    if (r != OPTIX_SUCCESS)
        throw std::runtime_error(std::string(what) + ": " + optixGetErrorName(r));
}

template <typename T>
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord
{
    alignas(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};
struct Empty {};

float3 make3(const float v[3]) { return make_float3(v[0], v[1], v[2]); }
float3 scale(float3 a, float s) { return make_float3(a.x * s, a.y * s, a.z * s); }
float3 cross(float3 a, float3 b) { return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float3 normalize(float3 a)
{
    const float len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return len > 0.0f ? scale(a, 1.0f / len) : a;
}

// Next mip level with a 2x2 box filter (edge texels are repeated for odd
// sizes). sRGB color channels are averaged in linear space.
std::vector<unsigned char> downsample(const std::vector<unsigned char>& src, int w, int h, bool srgb)
{
    static const auto toLinear = [] {
        std::array<float, 256> lut{};
        for (int i = 0; i < 256; ++i)
        {
            const float c = i / 255.0f;
            lut[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return lut;
    }();
    const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    std::vector<unsigned char> dst(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
        {
            const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
            const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
            const unsigned char* texels[4] = {&src[(size_t(y0) * w + x0) * 4], &src[(size_t(y0) * w + x1) * 4],
                                              &src[(size_t(y1) * w + x0) * 4], &src[(size_t(y1) * w + x1) * 4]};
            unsigned char* out = &dst[(size_t(y) * nw + x) * 4];
            for (int c = 0; c < 4; ++c)
            {
                if (srgb && c < 3)
                {
                    float sum = 0.0f;
                    for (const unsigned char* t : texels)
                        sum += toLinear[t[c]];
                    const float l = sum * 0.25f;
                    const float e = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
                    out[c] = (unsigned char)std::lround(std::clamp(e, 0.0f, 1.0f) * 255.0f);
                }
                else
                    out[c] = (unsigned char)((texels[0][c] + texels[1][c] + texels[2][c] + texels[3][c] + 2) / 4);
            }
        }
    return dst;
}

}  // namespace

// ---- DeviceBuffer ---------------------------------------------------------
void OptixRenderer::DeviceBuffer::resize(size_t bytes)
{
    if (bytes == size)
        return;
    free();
    if (bytes)
        check(cudaMalloc(reinterpret_cast<void**>(&ptr), bytes), "cudaMalloc");
    size = bytes;
}

void OptixRenderer::DeviceBuffer::upload(const void* data, size_t bytes)
{
    resize(bytes);
    if (bytes)
        check(cudaMemcpy(reinterpret_cast<void*>(ptr), data, bytes, cudaMemcpyHostToDevice), "cudaMemcpy");
}

void OptixRenderer::DeviceBuffer::free()
{
    if (ptr)
        cudaFree(reinterpret_cast<void*>(ptr));
    ptr = 0;
    size = 0;
}

// ---- OptixRenderer --------------------------------------------------------
OptixRenderer::~OptixRenderer()
{
    if (!m_context)
        return;
    cudaDeviceSynchronize();
    freeTextures();
    for (DeviceBuffer* b : {&m_raygenRecord, &m_missRecords, &m_hitRecords, &m_vertices, &m_indices, &m_triMesh,
                            &m_triMaterial, &m_triUVs, &m_triNormals, &m_materials, &m_gas, &m_colorBuffer,
                            &m_accumBuffer, &m_meshIdBuffer,
                            &m_paramsBuffer})
        b->free();
    optixPipelineDestroy(m_pipeline);
    optixProgramGroupDestroy(m_raygenPG);
    for (OptixProgramGroup pg : m_missPG)
        optixProgramGroupDestroy(pg);
    optixProgramGroupDestroy(m_hitPG);
    optixModuleDestroy(m_module);
    optixDeviceContextDestroy(m_context);
    cudaStreamDestroy(m_stream);
}

bool OptixRenderer::init(std::string& error)
{
    try
    {
        int deviceCount = 0;
        check(cudaGetDeviceCount(&deviceCount), "cudaGetDeviceCount");
        if (deviceCount == 0)
            throw std::runtime_error("no CUDA capable device found");
        check(cudaSetDevice(0), "cudaSetDevice");
        check(cudaFree(nullptr), "cudaFree");  // creates the primary context
        cudaDeviceProp props{};
        check(cudaGetDeviceProperties(&props, 0), "cudaGetDeviceProperties");
        m_deviceName = props.name;
        check(cudaStreamCreate(&m_stream), "cudaStreamCreate");
        if (cuCtxGetCurrent(&m_cuContext) != CUDA_SUCCESS)
            throw std::runtime_error("cuCtxGetCurrent failed");

        check(optixInit(), "optixInit");
        OptixDeviceContextOptions options = {};
        check(optixDeviceContextCreate(m_cuContext, &options, &m_context), "optixDeviceContextCreate");

        createPipeline();
        buildSbt();
        m_params.selectedMesh = kNoMesh;
        m_params.shadows = 1u;
        m_params.mipmaps = 1u;
        m_params.normalMaps = 1u;
        m_params.reflections = 1u;
        const float light[3] = {0.4f, 0.8f, 0.45f};
        setLightDir(light);
        m_paramsBuffer.resize(sizeof(LaunchParams));
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    return true;
}

void OptixRenderer::createPipeline()
{
    OptixModuleCompileOptions moduleOptions = {};
    moduleOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;

    OptixPipelineCompileOptions pipelineOptions = {};
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    pipelineOptions.numPayloadValues = 6;
    pipelineOptions.numAttributeValues = 2;
    pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pipelineOptions.pipelineLaunchParamsVariableName = "params";
    pipelineOptions.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

    char log[2048];
    size_t logSize = sizeof(log);
    check(optixModuleCreate(m_context, &moduleOptions, &pipelineOptions, g_devicePrograms, g_devicePrograms_size,
                            log, &logSize, &m_module),
          "optixModuleCreate");

    OptixProgramGroupOptions pgOptions = {};
    OptixProgramGroupDesc desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc.raygen.module = m_module;
    desc.raygen.entryFunctionName = "__raygen__renderFrame";
    logSize = sizeof(log);
    check(optixProgramGroupCreate(m_context, &desc, 1, &pgOptions, log, &logSize, &m_raygenPG), "raygen group");

    const char* missNames[RAY_TYPE_COUNT] = {"__miss__radiance", "__miss__shadow"};
    for (int i = 0; i < RAY_TYPE_COUNT; ++i)
    {
        desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = m_module;
        desc.miss.entryFunctionName = missNames[i];
        logSize = sizeof(log);
        check(optixProgramGroupCreate(m_context, &desc, 1, &pgOptions, log, &logSize, &m_missPG[i]), "miss group");
    }

    desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    desc.hitgroup.moduleCH = m_module;
    desc.hitgroup.entryFunctionNameCH = "__closesthit__radiance";
    desc.hitgroup.moduleAH = m_module;
    desc.hitgroup.entryFunctionNameAH = "__anyhit__cutout";
    logSize = sizeof(log);
    check(optixProgramGroupCreate(m_context, &desc, 1, &pgOptions, log, &logSize, &m_hitPG), "hit group");

    OptixProgramGroup groups[] = {m_raygenPG, m_missPG[0], m_missPG[1], m_hitPG};
    OptixPipelineLinkOptions linkOptions = {};
    linkOptions.maxTraceDepth = 3;  // camera -> reflection -> shadow
    logSize = sizeof(log);
    check(optixPipelineCreate(m_context, &pipelineOptions, &linkOptions, groups, 4, log, &logSize, &m_pipeline),
          "optixPipelineCreate");
    check(optixPipelineSetStackSizeFromCallDepths(m_pipeline, linkOptions.maxTraceDepth, 0, 0, 0, 1),
          "optixPipelineSetStackSizeFromCallDepths");
}

void OptixRenderer::buildSbt()
{
    SbtRecord<Empty> raygen;
    check(optixSbtRecordPackHeader(m_raygenPG, &raygen), "pack raygen");
    m_raygenRecord.upload(&raygen, sizeof(raygen));

    SbtRecord<Empty> miss[RAY_TYPE_COUNT];
    for (int i = 0; i < RAY_TYPE_COUNT; ++i)
        check(optixSbtRecordPackHeader(m_missPG[i], &miss[i]), "pack miss");
    m_missRecords.upload(miss, sizeof(miss));

    // Both ray types use the same record; the shadow ray disables closest-hit.
    SbtRecord<Empty> hit[RAY_TYPE_COUNT];
    for (int i = 0; i < RAY_TYPE_COUNT; ++i)
        check(optixSbtRecordPackHeader(m_hitPG, &hit[i]), "pack hit");
    m_hitRecords.upload(hit, sizeof(hit));

    m_sbt.raygenRecord = m_raygenRecord.ptr;
    m_sbt.missRecordBase = m_missRecords.ptr;
    m_sbt.missRecordStrideInBytes = sizeof(SbtRecord<Empty>);
    m_sbt.missRecordCount = RAY_TYPE_COUNT;
    m_sbt.hitgroupRecordBase = m_hitRecords.ptr;
    m_sbt.hitgroupRecordStrideInBytes = sizeof(SbtRecord<Empty>);
    m_sbt.hitgroupRecordCount = RAY_TYPE_COUNT;
}

void OptixRenderer::setGeometry(const FlatGeometry& g)
{
    m_params.handle = 0;
    m_params.frameIndex = 0;
    m_gas.free();
    m_vertices.upload(g.vertices.data(), g.vertices.size() * sizeof(float));
    m_indices.upload(g.indices.data(), g.indices.size() * sizeof(unsigned int));
    m_triMesh.upload(g.triMesh.data(), g.triMesh.size() * sizeof(unsigned int));
    m_triMaterial.upload(g.triMaterial.data(), g.triMaterial.size() * sizeof(unsigned int));
    m_triUVs.upload(g.triUVs.data(), g.triUVs.size() * sizeof(float));
    m_triNormals.upload(g.triNormals.data(), g.triNormals.size() * sizeof(float));
    std::vector<MaterialData> materials;
    for (const Material& m : g.materials)
    {
        auto f4 = [](const std::array<float, 4>& v) { return make_float4(v[0], v[1], v[2], v[3]); };
        auto f3 = [](const std::array<float, 3>& v) { return make_float3(v[0], v[1], v[2]); };
        auto scalar = [](const ScalarInput& in) {
            return ScalarParam{in.value, in.texture, in.channel, in.scale, in.bias};
        };
        MaterialData d = {};
        d.texScale = f4(m.scale);
        d.texBias = f4(m.bias);
        d.normalScale = f4(m.normalScale);
        d.normalBias = f4(m.normalBias);
        d.specScale = f4(m.specularScale);
        d.specBias = f4(m.specularBias);
        d.color = f3(m.color);
        d.texture = m.texture;
        d.specColor = f3(m.specularColor);
        d.specTexture = m.specularTexture;
        d.roughness = scalar(m.roughness);
        d.metallic = scalar(m.metallic);
        d.opacityTexture = m.opacityTexture;
        d.opacityChannel = m.opacityChannel;
        d.opacityThreshold = m.opacityThreshold;
        d.normalTexture = m.normalTexture;
        d.specularWorkflow = m.specularWorkflow ? 1 : 0;
        d.ior = m.ior;
        materials.push_back(d);
    }
    m_materials.upload(materials.data(), materials.size() * sizeof(MaterialData));
    uploadTextures(g);
    m_params.vertices = reinterpret_cast<const float3*>(m_vertices.ptr);
    m_params.indices = reinterpret_cast<const uint3*>(m_indices.ptr);
    m_params.triMesh = reinterpret_cast<const unsigned int*>(m_triMesh.ptr);
    m_params.triMaterial = reinterpret_cast<const unsigned int*>(m_triMaterial.ptr);
    m_params.triUVs = reinterpret_cast<const float2*>(m_triUVs.ptr);
    m_params.triNormals = reinterpret_cast<const float3*>(m_triNormals.ptr);
    m_params.materials = reinterpret_cast<const MaterialData*>(m_materials.ptr);
    m_params.textures = reinterpret_cast<const cudaTextureObject_t*>(m_textureTable.ptr);
    if (g.empty())
        return;

    // any-hit stays enabled for alpha cutouts; it returns early for opaque materials
    const unsigned int flags[] = {OPTIX_GEOMETRY_FLAG_NONE};
    OptixBuildInput input = {};
    input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    OptixBuildInputTriangleArray& tri = input.triangleArray;
    tri.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    tri.vertexStrideInBytes = sizeof(float3);
    tri.numVertices = unsigned(g.vertices.size() / 3);
    tri.vertexBuffers = &m_vertices.ptr;
    tri.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
    tri.indexStrideInBytes = sizeof(uint3);
    tri.numIndexTriplets = unsigned(g.indices.size() / 3);
    tri.indexBuffer = m_indices.ptr;
    tri.flags = flags;
    tri.numSbtRecords = 1;

    OptixAccelBuildOptions accelOptions = {};
    accelOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    accelOptions.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes sizes = {};
    check(optixAccelComputeMemoryUsage(m_context, &accelOptions, &input, 1, &sizes), "optixAccelComputeMemoryUsage");
    DeviceBuffer temp;
    temp.resize(sizes.tempSizeInBytes);
    m_gas.resize(sizes.outputSizeInBytes);
    check(optixAccelBuild(m_context, m_stream, &accelOptions, &input, 1, temp.ptr, temp.size, m_gas.ptr, m_gas.size,
                          &m_params.handle, nullptr, 0),
          "optixAccelBuild");
    check(cudaStreamSynchronize(m_stream), "accel build");
    temp.free();
}

void OptixRenderer::uploadTextures(const FlatGeometry& g)
{
    freeTextures();
    for (const Texture& t : g.textures)
    {
        // full mip chain, down to 1x1
        const unsigned levels = 1u + unsigned(std::floor(std::log2(double(std::max(t.width, t.height)))));
        const cudaChannelFormatDesc format = cudaCreateChannelDesc<uchar4>();
        cudaMipmappedArray_t mipmaps = nullptr;
        check(cudaMallocMipmappedArray(&mipmaps, &format, make_cudaExtent(size_t(t.width), size_t(t.height), 0),
                                       levels),
              "cudaMallocMipmappedArray");
        m_textureArrays.push_back(mipmaps);
        std::vector<unsigned char> level = t.rgba;
        int w = t.width, h = t.height;
        for (unsigned l = 0; l < levels; ++l)
        {
            cudaArray_t array = nullptr;
            check(cudaGetMipmappedArrayLevel(&array, mipmaps, l), "cudaGetMipmappedArrayLevel");
            check(cudaMemcpy2DToArray(array, 0, 0, level.data(), size_t(w) * 4, size_t(w) * 4, size_t(h),
                                      cudaMemcpyHostToDevice),
                  "texture upload");
            if (l + 1 < levels)
            {
                level = downsample(level, w, h, t.srgb);
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
            }
        }

        cudaResourceDesc resource = {};
        resource.resType = cudaResourceTypeMipmappedArray;
        resource.res.mipmap.mipmap = mipmaps;
        cudaTextureDesc desc = {};
        auto mode = [](TextureWrap w) {
            switch (w)
            {
            case TextureWrap::Mirror: return cudaAddressModeMirror;
            case TextureWrap::Clamp: return cudaAddressModeClamp;
            case TextureWrap::Black: return cudaAddressModeBorder;
            default: return cudaAddressModeWrap;
            }
        };
        desc.addressMode[0] = mode(t.wrapS);
        desc.addressMode[1] = mode(t.wrapT);
        desc.filterMode = cudaFilterModeLinear;
        desc.mipmapFilterMode = cudaFilterModeLinear;
        desc.maxMipmapLevelClamp = float(levels - 1);
        desc.maxAnisotropy = 8;
        desc.readMode = cudaReadModeNormalizedFloat;
        desc.normalizedCoords = 1;
        desc.sRGB = t.srgb ? 1 : 0;  // the hardware converts sRGB texels to linear
        cudaTextureObject_t object = 0;
        check(cudaCreateTextureObject(&object, &resource, &desc, nullptr), "cudaCreateTextureObject");
        m_textureObjects.push_back(object);
    }
    m_textureTable.upload(m_textureObjects.data(), m_textureObjects.size() * sizeof(cudaTextureObject_t));
}

void OptixRenderer::freeTextures()
{
    for (cudaTextureObject_t object : m_textureObjects)
        cudaDestroyTextureObject(object);
    for (cudaMipmappedArray_t mipmaps : m_textureArrays)
        cudaFreeMipmappedArray(mipmaps);
    m_textureObjects.clear();
    m_textureArrays.clear();
    m_textureTable.free();
}

void OptixRenderer::resize(unsigned int width, unsigned int height)
{
    width = width ? width : 1;
    height = height ? height : 1;
    if (width == m_params.width && height == m_params.height)
        return;
    m_params.width = width;
    m_params.height = height;
    m_colorBuffer.resize(size_t(width) * height * sizeof(uchar4));
    m_accumBuffer.resize(size_t(width) * height * sizeof(float4));
    m_params.accumBuffer = reinterpret_cast<float4*>(m_accumBuffer.ptr);
    m_params.frameIndex = 0;
    m_meshIdBuffer.resize(size_t(width) * height * sizeof(unsigned int));
    m_params.colorBuffer = reinterpret_cast<uchar4*>(m_colorBuffer.ptr);
    m_params.meshIdBuffer = reinterpret_cast<unsigned int*>(m_meshIdBuffer.ptr);
    m_pixels.resize(size_t(width) * height);
    m_meshIds.resize(size_t(width) * height);
}

void OptixRenderer::setCamera(const float eye[3], const float target[3], const float up[3], float fovYDeg)
{
    const float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float halfH = std::tan(fovYDeg * 0.5f * float(M_PI) / 180.0f);
    const float aspect = m_params.height ? float(m_params.width) / float(m_params.height) : 1.0f;
    setCameraView(eye, forward, up, halfH * aspect, halfH, false, 0.0f, 1e16f);
}

void OptixRenderer::setCameraView(const float eye[3], const float forward[3], const float up[3], float halfWidth,
                                  float halfHeight, bool orthographic, float clipNear, float clipFar)
{
    const float3 w = normalize(make3(forward));
    const float3 u = normalize(cross(w, make3(up)));
    const float3 v = cross(u, w);
    m_params.camera = {make3(eye), scale(u, halfWidth), scale(v, halfHeight), w, orthographic ? 1u : 0u,
                       clipNear, clipFar};
}

void OptixRenderer::setLightDir(const float dir[3])
{
    m_params.lightDir = normalize(make3(dir));
}

void OptixRenderer::render()
{
    if (!m_params.width || !m_params.height)
        return;
    const auto start = std::chrono::steady_clock::now();
    if (!m_params.handle)
    {
        // Nothing to trace: clear to the background color.
        std::fill(m_pixels.begin(), m_pixels.end(), make_uchar4(40, 42, 48, 255));
        std::fill(m_meshIds.begin(), m_meshIds.end(), kNoMesh);
        return;
    }
    check(cudaMemcpyAsync(reinterpret_cast<void*>(m_paramsBuffer.ptr), &m_params, sizeof(m_params),
                          cudaMemcpyHostToDevice, m_stream),
          "upload params");
    check(optixLaunch(m_pipeline, m_stream, m_paramsBuffer.ptr, sizeof(LaunchParams), &m_sbt, m_params.width,
                      m_params.height, 1),
          "optixLaunch");
    check(cudaMemcpyAsync(m_pixels.data(), reinterpret_cast<void*>(m_colorBuffer.ptr), m_colorBuffer.size,
                          cudaMemcpyDeviceToHost, m_stream),
          "download color");
    if (m_params.frameIndex == 0)
        check(cudaMemcpyAsync(m_meshIds.data(), reinterpret_cast<void*>(m_meshIdBuffer.ptr), m_meshIdBuffer.size,
                              cudaMemcpyDeviceToHost, m_stream),
              "download ids");
    check(cudaStreamSynchronize(m_stream), "render");
    ++m_params.frameIndex;
    m_lastRenderMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

unsigned int OptixRenderer::meshIdAt(unsigned int x, unsigned int y) const
{
    if (x >= m_params.width || y >= m_params.height)
        return kNoMesh;
    return m_meshIds[size_t(y) * m_params.width + x];
}
