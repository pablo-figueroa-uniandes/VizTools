// Data shared between the host (OptixRenderer.cpp) and the OptiX programs
// (devicePrograms.cu).
#pragma once

#include <optix_types.h>
#include <texture_types.h>
#include <vector_types.h>

constexpr unsigned int kNoMesh = 0xffffffffu;

enum RayType { RAY_TYPE_RADIANCE = 0, RAY_TYPE_SHADOW = 1, RAY_TYPE_COUNT };

// Perspective: rays leave `eye` through w + u * x + v * y, for x, y in [-1, 1]
// (u, v span the image plane at distance 1). Orthographic: rays leave
// eye + u * x + v * y along w (u, v are half the view size in scene units).
// Camera rays only see hits between clipNear and clipFar along w.
struct Camera
{
    float3 eye;
    float3 u, v, w;  // right, up and forward (w normalized)
    unsigned int orthographic;
    float clipNear, clipFar;
};

// A float input: `value`, or texture[channel] * scale + bias when texture >= 0.
struct ScalarParam
{
    float value;
    int   texture;
    int   channel;
    float scale, bias;
};

// UsdPreviewSurface subset.
//   diffuse  = texture(st) * texScale + texBias, or `color` when untextured
//   cutout   = hits where opacityTexture(st)[opacityChannel] < opacityThreshold are ignored
//   normal   = normalTexture(st) * normalScale + normalBias (tangent space)
//   specular = GGX; F0 from metallic + ior, or specColor in the specular workflow
struct MaterialData
{
    float4 texScale;
    float4 texBias;
    float4 normalScale;
    float4 normalBias;
    float4 specScale;
    float4 specBias;
    float3 color;
    int    texture;  // index into LaunchParams::textures, -1 for none
    float3 specColor;
    int    specTexture;
    ScalarParam roughness;
    ScalarParam metallic;
    int    opacityTexture;
    int    opacityChannel;
    float  opacityThreshold;
    int    normalTexture;
    int    specularWorkflow;
    float  ior;
};

struct LaunchParams
{
    // output: colorBuffer = accumBuffer / (frameIndex + 1); meshIdBuffer is written on frame 0
    uchar4*       colorBuffer;
    float4*       accumBuffer;
    unsigned int* meshIdBuffer;  // mesh under each pixel (kNoMesh for background)
    unsigned int  width, height;
    unsigned int  frameIndex;    // progressive sample index, reset to 0 after any change

    Camera camera;
    float3 lightDir;  // direction towards the light, normalized
    unsigned int shadows;
    unsigned int mipmaps;      // 0: always sample the finest texture level
    unsigned int normalMaps;   // 0: ignore normal maps
    unsigned int reflections;  // 0: reflect the sky only, without tracing rays
    unsigned int selectedMesh;

    // scene: one GAS with all triangles in world space
    OptixTraversableHandle handle;
    const float3*       vertices;
    const uint3*        indices;
    const unsigned int* triMesh;      // mesh id of each triangle
    const unsigned int* triMaterial;  // material of each triangle
    const float2*       triUVs;       // st of the 3 corners of each triangle
    const float3*       triNormals;   // shading normal of the 3 corners (zero: flat)
    const MaterialData* materials;
    const cudaTextureObject_t* textures;
};
