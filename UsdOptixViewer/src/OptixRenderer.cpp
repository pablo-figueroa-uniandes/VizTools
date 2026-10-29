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

extern const unsigned char g_devicePrograms[];
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

void OptixRenderer::MeshBuffers::free()
{
    for (DeviceBuffer* b : {&positions, &normals, &uvs, &indices, &triMaterial, &gas})
        b->free();
    handle = 0;
}

// ---- OptixRenderer --------------------------------------------------------
OptixRenderer::~OptixRenderer()
{
    release();
}

void OptixRenderer::release()
{
    if (!m_context)
        return;
    cudaDeviceSynchronize();
    freeScene();
    for (DeviceBuffer* b : {&m_raygenRecord, &m_missRecords, &m_hitRecords, &m_colorBuffer, &m_accumBuffer,
                            &m_meshIdBuffer, &m_hitBuffer, &m_paramsBuffer, &m_viewAccum, &m_viewMeshIds, &m_viewParams})
        b->free();
    optixPipelineDestroy(m_pipeline);
    optixProgramGroupDestroy(m_raygenPG);
    for (OptixProgramGroup pg : m_missPG)
        optixProgramGroupDestroy(pg);
    optixProgramGroupDestroy(m_hitPG);
    optixModuleDestroy(m_module);
    optixDeviceContextDestroy(m_context);
    cudaStreamDestroy(m_stream);
    m_context = nullptr;
    m_module = nullptr;
    m_pipeline = nullptr;
    m_raygenPG = m_hitPG = nullptr;
    m_missPG[RAY_TYPE_RADIANCE] = m_missPG[RAY_TYPE_SHADOW] = nullptr;
    m_sbt = {};
    m_stream = nullptr;
    m_cuContext = nullptr;
    m_params = {};
    m_pixels.clear();
    m_meshIds.clear();
}

bool OptixRenderer::init(std::string& error, const unsigned char* deviceUuid)
{
    try
    {
        int deviceCount = 0;
        check(cudaGetDeviceCount(&deviceCount), "cudaGetDeviceCount");
        if (deviceCount == 0)
            throw std::runtime_error("no CUDA capable device found");
        int device = 0;
        cudaDeviceProp props{};
        if (deviceUuid)
        {
            // the XR runtime chose a GPU; CUDA numbers GPUs differently from Vulkan
            device = -1;
            for (int d = 0; d < deviceCount && device < 0; ++d)
                if (cudaGetDeviceProperties(&props, d) == cudaSuccess &&
                    std::memcmp(props.uuid.bytes, deviceUuid, sizeof(props.uuid.bytes)) == 0)
                    device = d;
            if (device < 0)
                throw std::runtime_error("no CUDA device matches the GPU of the XR runtime");
        }
        check(cudaSetDevice(device), "cudaSetDevice");
        check(cudaFree(nullptr), "cudaFree");  // creates the primary context
        check(cudaGetDeviceProperties(&props, device), "cudaGetDeviceProperties");
        m_deviceName = props.name;
        std::memcpy(m_deviceUuid.data(), props.uuid.bytes, m_deviceUuid.size());
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
    pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
    pipelineOptions.numPayloadValues = 7;
    pipelineOptions.numAttributeValues = 2;
    pipelineOptions.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
    pipelineOptions.pipelineLaunchParamsVariableName = "params";
    pipelineOptions.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

    char log[2048];
    size_t logSize = sizeof(log);
    check(optixModuleCreate(m_context, &moduleOptions, &pipelineOptions,
                            reinterpret_cast<const char*>(g_devicePrograms), g_devicePrograms_size, log, &logSize,
                            &m_module),
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
    // traversable graph depth 2: IAS -> GAS
    check(optixPipelineSetStackSizeFromCallDepths(m_pipeline, linkOptions.maxTraceDepth, 0, 0, 0, 2),
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

void OptixRenderer::setScene(const Scene& scene)
{
    m_params.handle = 0;
    m_params.frameIndex = 0;
    freeScene();
    std::vector<MaterialData> materials;
    for (const Material& m : scene.materials())
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
    uploadTextures(scene);
    m_params.materials = reinterpret_cast<const MaterialData*>(m_materials.ptr);
    m_params.textures = reinterpret_cast<const cudaTextureObject_t*>(m_textureTable.ptr);

    // one GAS per mesh, in the mesh's own space
    std::vector<MeshData> records;
    std::vector<OptixInstance> instances;
    for (const RenderMesh& mesh : scene.meshes())
    {
        MeshBuffers& b = m_meshes.emplace_back();
        b.positions.upload(mesh.positions.data(), mesh.positions.size() * sizeof(float));
        b.normals.upload(mesh.normals.data(), mesh.normals.size() * sizeof(float));
        b.uvs.upload(mesh.uvs.data(), mesh.uvs.size() * sizeof(float));
        b.indices.upload(mesh.indices.data(), mesh.indices.size() * sizeof(unsigned int));
        b.triMaterial.upload(mesh.triMaterial.data(), mesh.triMaterial.size() * sizeof(unsigned int));
        records.push_back({reinterpret_cast<const float3*>(b.positions.ptr),
                           reinterpret_cast<const float3*>(b.normals.ptr), reinterpret_cast<const float2*>(b.uvs.ptr),
                           reinterpret_cast<const uint3*>(b.indices.ptr),
                           reinterpret_cast<const unsigned int*>(b.triMaterial.ptr)});

        // any-hit stays enabled for alpha cutouts; it returns early for opaque materials
        const unsigned int flags[] = {OPTIX_GEOMETRY_FLAG_NONE};
        OptixBuildInput input = {};
        input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
        OptixBuildInputTriangleArray& tri = input.triangleArray;
        tri.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
        tri.vertexStrideInBytes = sizeof(float3);
        tri.numVertices = unsigned(mesh.vertexCount());
        tri.vertexBuffers = &b.positions.ptr;
        tri.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
        tri.indexStrideInBytes = sizeof(uint3);
        tri.numIndexTriplets = unsigned(mesh.triangleCount());
        tri.indexBuffer = b.indices.ptr;
        tri.flags = flags;
        tri.numSbtRecords = 1;
        b.handle = buildAccel(input, b.gas);

        OptixInstance instance = {};
        std::copy(mesh.toWorld.begin(), mesh.toWorld.end(), instance.transform);
        instance.instanceId = unsigned(instances.size());  // the mesh id
        instance.visibilityMask = 255;
        instance.sbtOffset = 0;
        instance.flags = OPTIX_INSTANCE_FLAG_NONE;
        instance.traversableHandle = b.handle;
        instances.push_back(instance);
    }
    m_meshRecords.upload(records.data(), records.size() * sizeof(MeshData));
    m_params.meshes = reinterpret_cast<const MeshData*>(m_meshRecords.ptr);
    if (instances.empty())
        return;

    // the IAS places every mesh in the world
    m_instances.upload(instances.data(), instances.size() * sizeof(OptixInstance));
    OptixBuildInput input = {};
    input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    input.instanceArray.instances = m_instances.ptr;
    input.instanceArray.numInstances = unsigned(instances.size());
    m_params.handle = buildAccel(input, m_ias);
}

OptixTraversableHandle OptixRenderer::buildAccel(const OptixBuildInput& input, DeviceBuffer& output)
{
    OptixAccelBuildOptions accelOptions = {};
    accelOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    accelOptions.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes sizes = {};
    check(optixAccelComputeMemoryUsage(m_context, &accelOptions, &input, 1, &sizes), "optixAccelComputeMemoryUsage");
    DeviceBuffer temp;
    temp.resize(sizes.tempSizeInBytes);
    output.resize(sizes.outputSizeInBytes);
    OptixTraversableHandle handle = 0;
    check(optixAccelBuild(m_context, m_stream, &accelOptions, &input, 1, temp.ptr, temp.size, output.ptr, output.size,
                          &handle, nullptr, 0),
          "optixAccelBuild");
    check(cudaStreamSynchronize(m_stream), "accel build");
    temp.free();
    return handle;
}

void OptixRenderer::freeScene()
{
    for (MeshBuffers& b : m_meshes)
        b.free();
    m_meshes.clear();
    for (DeviceBuffer* b : {&m_meshRecords, &m_instances, &m_ias, &m_materials})
        b->free();
    freeTextures();
}

void OptixRenderer::uploadTextures(const Scene& scene)
{
    freeTextures();
    for (const Texture& t : scene.textures())
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
    m_hitBuffer.resize(size_t(width) * height * sizeof(float4));
    m_params.colorBuffer = reinterpret_cast<uchar4*>(m_colorBuffer.ptr);
    m_params.meshIdBuffer = reinterpret_cast<unsigned int*>(m_meshIdBuffer.ptr);
    m_params.hitBuffer = reinterpret_cast<float4*>(m_hitBuffer.ptr);
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

void OptixRenderer::renderView(const Camera& camera, unsigned int width, unsigned int height, uchar4* target,
                               unsigned int samples)
{
    const size_t pixels = size_t(width) * height;
    if (!m_params.handle)
    {
        // nothing to trace: the background color (40, 42, 48) as one 32-bit word
        if (cuMemsetD32Async(CUdeviceptr(target), 0xff302a28u, pixels, m_stream) != CUDA_SUCCESS)
            throw std::runtime_error("cuMemsetD32Async failed");
        check(cudaStreamSynchronize(m_stream), "renderView");
        return;
    }
    m_viewAccum.resize(pixels * sizeof(float4));
    m_viewMeshIds.resize(pixels * sizeof(unsigned int));
    m_viewParams.resize(sizeof(LaunchParams));
    LaunchParams params = m_params;
    params.camera = camera;
    params.width = width;
    params.height = height;
    params.colorBuffer = target;
    params.accumBuffer = reinterpret_cast<float4*>(m_viewAccum.ptr);
    params.meshIdBuffer = reinterpret_cast<unsigned int*>(m_viewMeshIds.ptr);
    params.hitBuffer = nullptr;
    for (unsigned int s = 0; s < std::max(samples, 1u); ++s)
    {
        params.frameIndex = s;
        check(cudaMemcpyAsync(reinterpret_cast<void*>(m_viewParams.ptr), &params, sizeof(params),
                              cudaMemcpyHostToDevice, m_stream),
              "upload view params");
        check(optixLaunch(m_pipeline, m_stream, m_viewParams.ptr, sizeof(LaunchParams), &m_sbt, width, height, 1),
              "optixLaunch");
    }
    check(cudaStreamSynchronize(m_stream), "renderView");
}

unsigned int OptixRenderer::meshIdAt(unsigned int x, unsigned int y) const
{
    if (x >= m_params.width || y >= m_params.height)
        return kNoMesh;
    return m_meshIds[size_t(y) * m_params.width + x];
}

// Reads one pixel of the hit buffer back on demand: it is only needed when
// the orbit pivot moves, so it is not downloaded with every frame.
bool OptixRenderer::hitPointAt(unsigned int x, unsigned int y, float point[3]) const
{
    if (x >= m_params.width || y >= m_params.height || !m_params.handle || m_params.frameIndex == 0)
        return false;
    float4 hit;
    if (cudaMemcpyAsync(&hit, reinterpret_cast<const float4*>(m_hitBuffer.ptr) + size_t(y) * m_params.width + x,
                        sizeof(hit), cudaMemcpyDeviceToHost, m_stream) != cudaSuccess ||
        cudaStreamSynchronize(m_stream) != cudaSuccess)
        return false;
    point[0] = hit.x, point[1] = hit.y, point[2] = hit.z;
    return hit.w != 0.0f;
}
