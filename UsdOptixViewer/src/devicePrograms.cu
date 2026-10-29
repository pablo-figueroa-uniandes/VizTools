// OptiX programs: progressive pinhole or orthographic camera, UsdPreviewSurface-style shading
// (Lambert diffuse + GGX specular) with a directional light, hard shadows,
// one glossy reflection bounce and a gradient sky.
#include <optix.h>

#include "LaunchParams.h"

extern "C" __constant__ LaunchParams params;

constexpr float kPi = 3.14159265358979f;
constexpr float kLightIntensity = 0.7f * kPi;  // a surface facing the light reflects 0.7 x its albedo

static __forceinline__ __device__ float3 operator+(float3 a, float3 b) { return make_float3(a.x + b.x, a.y + b.y, a.z + b.z); }
static __forceinline__ __device__ float3 operator-(float3 a, float3 b) { return make_float3(a.x - b.x, a.y - b.y, a.z - b.z); }
static __forceinline__ __device__ float3 operator*(float3 a, float s) { return make_float3(a.x * s, a.y * s, a.z * s); }
static __forceinline__ __device__ float3 operator*(float3 a, float3 b) { return make_float3(a.x * b.x, a.y * b.y, a.z * b.z); }
static __forceinline__ __device__ float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static __forceinline__ __device__ float3 cross(float3 a, float3 b)
{
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static __forceinline__ __device__ float3 normalize(float3 a) { return a * rsqrtf(fmaxf(dot(a, a), 1e-20f)); }
static __forceinline__ __device__ float3 mix(float3 a, float3 b, float t) { return a * (1.0f - t) + b * t; }

// ---- random numbers ---------------------------------------------------------
static __forceinline__ __device__ unsigned int tea(unsigned int v0, unsigned int v1)
{
    unsigned int s0 = 0;
    for (int n = 0; n < 4; ++n)
    {
        s0 += 0x9e3779b9;
        v0 += ((v1 << 4) + 0xa341316c) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4);
        v1 += ((v0 << 4) + 0xad90777d) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761e);
    }
    return v0;
}

static __forceinline__ __device__ float rnd(unsigned int& seed)
{
    seed = 1664525u * seed + 1013904223u;
    return float(seed & 0x00ffffffu) / float(0x01000000u);
}

// ---- payload ------------------------------------------------------------------
// Radiance rays: p0..p2 = rgb (float bits), p3 = mesh id, p4 = depth, p5 = rng seed,
// p6 = hit distance (float bits; left alone on a miss).
// Shadow rays: p0 = 1 when the light is visible.
static __forceinline__ __device__ void setColor(float3 c, unsigned int meshId)
{
    optixSetPayload_0(__float_as_uint(c.x));
    optixSetPayload_1(__float_as_uint(c.y));
    optixSetPayload_2(__float_as_uint(c.z));
    optixSetPayload_3(meshId);
}

static __forceinline__ __device__ bool isShadowRay()
{
    return optixGetRayFlags() & OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT;
}

static __forceinline__ __device__ float3 sky(float3 dir)
{
    return mix(make_float3(0.10f, 0.10f, 0.12f), make_float3(0.32f, 0.38f, 0.48f), 0.5f * (dir.y + 1.0f));
}

// ---- the hit triangle -----------------------------------------------------------
// The triangle under the current hit: its mesh (the instance), its vertex
// indices and its corners moved from the mesh's space to world space.
struct HitTriangle
{
    const MeshData* mesh;
    unsigned int prim;
    uint3 tri;
    float3 a, b, c;
};

static __forceinline__ __device__ HitTriangle hitTriangle()
{
    HitTriangle h;
    h.mesh = &params.meshes[optixGetInstanceId()];
    h.prim = optixGetPrimitiveIndex();
    h.tri = h.mesh->indices[h.prim];
    h.a = optixTransformPointFromObjectToWorldSpace(h.mesh->positions[h.tri.x]);
    h.b = optixTransformPointFromObjectToWorldSpace(h.mesh->positions[h.tri.y]);
    h.c = optixTransformPointFromObjectToWorldSpace(h.mesh->positions[h.tri.z]);
    return h;
}

static __forceinline__ __device__ void cornerUVs(const HitTriangle& h, float2 uv[3])
{
    const float2* uvs = h.mesh->uvs;
    uv[0] = uvs ? uvs[h.tri.x] : make_float2(0.0f, 0.0f);
    uv[1] = uvs ? uvs[h.tri.y] : make_float2(0.0f, 0.0f);
    uv[2] = uvs ? uvs[h.tri.z] : make_float2(0.0f, 0.0f);
}

// ---- textures -------------------------------------------------------------------
// Samples a texture at the current hit, flipping USD's bottom-left st origin
// to the image's top-left row order. For camera rays the mip level comes from
// ray differentials: the rays through the neighbouring pixels are intersected
// with the triangle's plane, and the offsets are converted to st gradients.
// Shadow and reflection rays sample the finest level.
static __forceinline__ __device__ float4 sampleTexture(int texture, const HitTriangle& h)
{
    const float2 bary = optixGetTriangleBarycentrics();
    float2 uv[3];
    cornerUVs(h, uv);
    const float w0 = 1.0f - bary.x - bary.y;
    const float s = w0 * uv[0].x + bary.x * uv[1].x + bary.y * uv[2].x;
    const float t = w0 * uv[0].y + bary.x * uv[1].y + bary.y * uv[2].y;

    float2 dx = make_float2(0.0f, 0.0f), dy = make_float2(0.0f, 0.0f);
    if (params.mipmaps && !isShadowRay() && optixGetPayload_4() == 0)
    {
        const float3 e1 = h.b - h.a, e2 = h.c - h.a;
        const float3 n = cross(e1, e2);
        const float3 o = optixGetWorldRayOrigin();
        const float3 p = o + optixGetWorldRayDirection() * optixGetRayTmax();

        // neighbouring rays: shifted origins (orthographic) or directions (perspective)
        const Camera& cam = params.camera;
        const float3 du = cam.u * (2.0f / params.width), dv = cam.v * (-2.0f / params.height);
        float3 oX = o, oY = o, dirX, dirY;
        if (cam.orthographic)
        {
            oX = o + du;
            oY = o + dv;
            dirX = dirY = cam.w;
        }
        else
        {
            const uint3 idx = optixGetLaunchIndex();
            const float nx = 2.0f * (idx.x + 0.5f) / params.width - 1.0f;
            const float ny = 1.0f - 2.0f * (idx.y + 0.5f) / params.height;
            dirX = cam.w + cam.u * nx + cam.v * ny + du;
            dirY = cam.w + cam.u * nx + cam.v * ny + dv;
        }

        const float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2);
        const float denom = d00 * d11 - d01 * d01;
        auto gradient = [&](float3 origin, float3 dir) {
            const float dn = dot(dir, n);
            if (fabsf(dn) < 1e-20f || fabsf(denom) < 1e-30f)
                return make_float2(0.0f, 0.0f);
            const float3 offset = origin + dir * (dot(p - origin, n) / dn) - p;
            const float d20 = dot(offset, e1), d21 = dot(offset, e2);
            const float b1 = (d11 * d20 - d01 * d21) / denom, b2 = (d00 * d21 - d01 * d20) / denom;
            return make_float2(b1 * (uv[1].x - uv[0].x) + b2 * (uv[2].x - uv[0].x),
                               -(b1 * (uv[1].y - uv[0].y) + b2 * (uv[2].y - uv[0].y)));  // t is flipped
        };
        dx = gradient(oX, dirX);
        dy = gradient(oY, dirY);
    }
    return tex2DGrad<float4>(params.textures[texture], s, 1.0f - t, dx, dy);
}

static __forceinline__ __device__ float channel(float4 v, int c)
{
    return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w;
}

static __forceinline__ __device__ float evalScalar(const ScalarParam& p, const HitTriangle& h)
{
    if (p.texture < 0)
        return p.value;
    return channel(sampleTexture(p.texture, h), p.channel) * p.scale + p.bias;
}

// Shading normal from a tangent-space normal map. The tangent frame comes from
// the triangle's positions and st (tangent along +s, bitangent along +t, the
// image's "up"), orthonormalized around the base normal `n`.
static __forceinline__ __device__ float3 normalMapped(const MaterialData& mat, const HitTriangle& h, float3 n)
{
    const float3 e1 = h.b - h.a, e2 = h.c - h.a;
    float2 uv[3];
    cornerUVs(h, uv);
    const float du1 = uv[1].x - uv[0].x, dv1 = uv[1].y - uv[0].y;
    const float du2 = uv[2].x - uv[0].x, dv2 = uv[2].y - uv[0].y;
    const float det = du1 * dv2 - du2 * dv1;
    if (fabsf(det) < 1e-12f)
        return n;  // degenerate st: no usable frame
    const float3 tangent = (e1 * dv2 - e2 * dv1) * (1.0f / det);
    const float3 bitangent = (e2 * du1 - e1 * du2) * (1.0f / det);
    const float3 t = normalize(tangent - n * dot(n, tangent));
    float3 b = cross(n, t);
    if (dot(b, bitangent) < 0.0f)
        b = b * -1.0f;  // mirrored st

    const float4 texel = sampleTexture(mat.normalTexture, h);
    const float3 local = make_float3(texel.x * mat.normalScale.x + mat.normalBias.x,
                                     texel.y * mat.normalScale.y + mat.normalBias.y,
                                     texel.z * mat.normalScale.z + mat.normalBias.z);
    return normalize(t * local.x + b * local.y + n * local.z);
}

// ---- GGX microfacet model -----------------------------------------------------
static __forceinline__ __device__ float ggxD(float nDotH, float a2)
{
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

static __forceinline__ __device__ float smithG1(float nDotX, float a2)
{
    return 2.0f * nDotX / (nDotX + sqrtf(a2 + (1.0f - a2) * nDotX * nDotX));
}

static __forceinline__ __device__ float3 fresnel(float3 f0, float cosTheta)
{
    const float m = 1.0f - fminf(fmaxf(cosTheta, 0.0f), 1.0f);
    const float m5 = m * m * m * m * m;
    return f0 + (make_float3(1.0f, 1.0f, 1.0f) - f0) * m5;
}

// ---- programs -------------------------------------------------------------------
extern "C" __global__ void __raygen__renderFrame()
{
    const uint3 idx = optixGetLaunchIndex();
    const unsigned int pixel = idx.y * params.width + idx.x;
    unsigned int seed = tea(pixel, params.frameIndex);

    // frame 0 goes through the pixel center (also used for picking), later frames jitter
    const float jx = params.frameIndex == 0 ? 0.5f : rnd(seed);
    const float jy = params.frameIndex == 0 ? 0.5f : rnd(seed);
    const float2 ndc = make_float2(2.0f * (idx.x + jx) / params.width - 1.0f,
                                   1.0f - 2.0f * (idx.y + jy) / params.height);
    const Camera& cam = params.camera;
    float3 origin = cam.eye, dir;
    if (cam.orthographic)
    {
        origin = cam.eye + cam.u * ndc.x + cam.v * ndc.y;
        dir = cam.w;
    }
    else
        dir = normalize(cam.w + cam.u * ndc.x + cam.v * ndc.y);
    const float depthPerT = dot(dir, cam.w);  // the clipping range is a depth along w, not a ray distance

    unsigned int r, g, b, meshId, depth = 0, hitT = 0;
    optixTrace(params.handle, origin, dir, cam.clipNear / depthPerT, cam.clipFar / depthPerT, 0.0f,
               OptixVisibilityMask(255), OPTIX_RAY_FLAG_NONE, RAY_TYPE_RADIANCE, RAY_TYPE_COUNT, RAY_TYPE_RADIANCE,
               r, g, b, meshId, depth, seed, hitT);

    const float4 sample = make_float4(__uint_as_float(r), __uint_as_float(g), __uint_as_float(b), 1.0f);
    float4 sum = sample;
    if (params.frameIndex > 0)
    {
        const float4 prev = params.accumBuffer[pixel];
        sum = make_float4(prev.x + sample.x, prev.y + sample.y, prev.z + sample.z, prev.w + 1.0f);
    }
    params.accumBuffer[pixel] = sum;
    const float inv = 1.0f / sum.w;
    // approximate sRGB encoding
    params.colorBuffer[pixel] = make_uchar4((unsigned char)(255.0f * __saturatef(sqrtf(sum.x * inv))),
                                            (unsigned char)(255.0f * __saturatef(sqrtf(sum.y * inv))),
                                            (unsigned char)(255.0f * __saturatef(sqrtf(sum.z * inv))), 255);
    if (params.frameIndex == 0)
    {
        params.meshIdBuffer[pixel] = meshId;
        // the orbit camera pivots on these points
        if (params.hitBuffer)
        {
            const float3 p = origin + dir * __uint_as_float(hitT);
            params.hitBuffer[pixel] = make_float4(p.x, p.y, p.z, meshId == kNoMesh ? 0.0f : 1.0f);
        }
    }
}

extern "C" __global__ void __miss__radiance()
{
    setColor(sky(optixGetWorldRayDirection()), kNoMesh);
}

extern "C" __global__ void __miss__shadow()
{
    optixSetPayload_0(1u);  // light is visible
}

// Alpha cutouts (UsdPreviewSurface opacity + opacityThreshold), for camera,
// reflection and shadow rays.
extern "C" __global__ void __anyhit__cutout()
{
    const MeshData& mesh = params.meshes[optixGetInstanceId()];
    const MaterialData& mat = params.materials[mesh.triMaterial[optixGetPrimitiveIndex()]];
    if (mat.opacityTexture < 0)
        return;
    const float4 texel = sampleTexture(mat.opacityTexture, hitTriangle());
    if (channel(texel, mat.opacityChannel) < mat.opacityThreshold)
        optixIgnoreIntersection();
}

extern "C" __global__ void __closesthit__radiance()
{
    const unsigned int depth = optixGetPayload_4();
    unsigned int seed = optixGetPayload_5();
    const HitTriangle hit = hitTriangle();
    const float3 rayDir = optixGetWorldRayDirection();
    const unsigned int meshId = optixGetInstanceId();
    const MaterialData& mat = params.materials[hit.mesh->triMaterial[hit.prim]];

    // geometric normal: outward for counter-clockwise triangles (see Scene.cpp)
    float3 n = normalize(cross(hit.b - hit.a, hit.c - hit.a));
    // interpolated shading normal (smooth/authored normals); they win if the winding disagrees
    float3 ns = n;
    if (const float3* cn = hit.mesh->normals)
    {
        const float2 bary = optixGetTriangleBarycentrics();
        const float3 local = cn[hit.tri.x] * (1.0f - bary.x - bary.y) + cn[hit.tri.y] * bary.x + cn[hit.tri.z] * bary.y;
        if (dot(local, local) > 0.0f)
        {
            ns = normalize(optixTransformNormalFromObjectToWorldSpace(local));
            if (dot(ns, n) < 0.0f)
                n = n * -1.0f;
        }
    }
    if (params.normalMaps && mat.normalTexture >= 0)
        ns = normalMapped(mat, hit, ns);
    if (dot(n, rayDir) > 0.0f)
    {
        n = n * -1.0f;  // two-sided
        ns = ns * -1.0f;
    }
    // keep the shading normal facing the viewer
    const float3 v = rayDir * -1.0f;
    const float facing = dot(ns, v);
    if (facing < 1e-3f)
        ns = normalize(ns + v * (1e-3f - facing));
    const float nDotV = fmaxf(dot(ns, v), 1e-4f);

    // material
    float3 albedo = mat.color;
    if (mat.texture >= 0)
    {
        const float4 texel = sampleTexture(mat.texture, hit);
        albedo = make_float3(texel.x * mat.texScale.x + mat.texBias.x, texel.y * mat.texScale.y + mat.texBias.y,
                             texel.z * mat.texScale.z + mat.texBias.z);
    }
    if (meshId == params.selectedMesh)
        albedo = mix(albedo, make_float3(1.0f, 0.55f, 0.1f), 0.6f);
    const float roughness = __saturatef(evalScalar(mat.roughness, hit));
    float3 f0, diffuse;
    if (mat.specularWorkflow)
    {
        f0 = mat.specColor;
        if (mat.specTexture >= 0)
        {
            const float4 texel = sampleTexture(mat.specTexture, hit);
            f0 = make_float3(texel.x * mat.specScale.x + mat.specBias.x, texel.y * mat.specScale.y + mat.specBias.y,
                             texel.z * mat.specScale.z + mat.specBias.z);
        }
        diffuse = albedo;
    }
    else
    {
        const float metallic = __saturatef(evalScalar(mat.metallic, hit));
        const float r0 = (mat.ior - 1.0f) / (mat.ior + 1.0f);
        f0 = mix(make_float3(r0 * r0, r0 * r0, r0 * r0), albedo, metallic);
        diffuse = albedo * (1.0f - metallic);
    }
    const float3 fv = fresnel(f0, nDotV);
    const float3 kd = make_float3(1.0f - fv.x, 1.0f - fv.y, 1.0f - fv.z);

    // ambient + headlight fill on the diffuse part (no indirect diffuse)
    float3 color = diffuse * kd * (0.15f + 0.25f * nDotV);

    // directional light: Lambert + GGX
    const float3 hitPos = optixGetWorldRayOrigin() + rayDir * optixGetRayTmax();
    const float3 offset = n * (1e-3f * (1.0f + fabsf(optixGetRayTmax()) * 1e-3f));
    const float3 l = params.lightDir;
    const float nDotL = dot(ns, l);
    if (nDotL > 0.0f && dot(n, l) > 0.0f)
    {
        unsigned int visible = 1u;
        if (params.shadows)
        {
            visible = 0u;
            optixTrace(params.handle, hitPos + offset, l, 0.0f, 1e16f, 0.0f, OptixVisibilityMask(255),
                       OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
                       RAY_TYPE_RADIANCE, RAY_TYPE_COUNT, RAY_TYPE_SHADOW, visible);
        }
        if (visible)
        {
            const float3 h = normalize(v + l);
            const float aLight = fmaxf(roughness * roughness, 0.01f);  // a delta light needs a minimum width
            const float a2 = aLight * aLight;
            const float3 f = fresnel(f0, dot(v, h));
            const float spec = ggxD(fmaxf(dot(ns, h), 0.0f), a2) * smithG1(nDotV, a2) * smithG1(nDotL, a2) /
                               (4.0f * nDotV);  // BRDF * nDotL
            color = color + (diffuse * kd * (nDotL / kPi) + f * spec) * kLightIntensity;
        }
    }

    // glossy reflection: one direction per sample, accumulated over frames.
    // Visible-normal sampling of GGX (Heitz 2018): the weight is F * G1(r) <= 1,
    // which keeps rough and grazing reflections free of fireflies.
    {
        const float alpha = fmaxf(roughness * roughness, 1e-3f), a2 = alpha * alpha;
        const float3 up = fabsf(ns.z) < 0.999f ? make_float3(0.0f, 0.0f, 1.0f) : make_float3(1.0f, 0.0f, 0.0f);
        const float3 tx = normalize(cross(up, ns)), ty = cross(ns, tx);
        const float3 ve = make_float3(dot(v, tx), dot(v, ty), dot(v, ns));  // view in tangent space
        const float3 vh = normalize(make_float3(alpha * ve.x, alpha * ve.y, ve.z));
        const float lenSq = vh.x * vh.x + vh.y * vh.y;
        const float3 t1 = lenSq > 0.0f ? make_float3(-vh.y, vh.x, 0.0f) * rsqrtf(lenSq) : make_float3(1.0f, 0.0f, 0.0f);
        const float3 t2 = cross(vh, t1);
        const float rad = sqrtf(rnd(seed)), phi = 2.0f * kPi * rnd(seed);
        const float p1 = rad * cosf(phi), s = 0.5f * (1.0f + vh.z);
        const float p2 = (1.0f - s) * sqrtf(fmaxf(0.0f, 1.0f - p1 * p1)) + s * rad * sinf(phi);
        const float3 nh = t1 * p1 + t2 * p2 + vh * sqrtf(fmaxf(0.0f, 1.0f - p1 * p1 - p2 * p2));
        const float3 hl = normalize(make_float3(alpha * nh.x, alpha * nh.y, fmaxf(nh.z, 0.0f)));
        const float3 h = tx * hl.x + ty * hl.y + ns * hl.z;
        const float vDotH = dot(v, h);
        const float3 r = h * (2.0f * vDotH) - v;
        const float nDotR = dot(ns, r);
        if (vDotH > 0.0f && nDotR > 0.0f && dot(n, r) > 0.0f)
        {
            const float3 weight = fresnel(f0, vDotH) * smithG1(nDotR, a2);
            float3 incoming;
            if (params.reflections && depth == 0)
            {
                unsigned int rr, rg, rb, rMesh, rDepth = depth + 1, rT;
                optixTrace(params.handle, hitPos + offset, r, 0.0f, 1e16f, 0.0f, OptixVisibilityMask(255),
                           OPTIX_RAY_FLAG_NONE, RAY_TYPE_RADIANCE, RAY_TYPE_COUNT, RAY_TYPE_RADIANCE,
                           rr, rg, rb, rMesh, rDepth, seed, rT);
                incoming = make_float3(__uint_as_float(rr), __uint_as_float(rg), __uint_as_float(rb));
                // Firefly clamp: a rough reflection that lands on another surface's sharp
                // highlight of the (delta) light returns huge, rare values. Anything above
                // 2 already displays as white, so clamping there only removes the spikes.
                const float peak = fmaxf(incoming.x, fmaxf(incoming.y, incoming.z));
                if (peak > 2.0f)
                    incoming = incoming * (2.0f / peak);
            }
            else
                incoming = sky(r);
            color = color + weight * incoming;
        }
    }

    optixSetPayload_5(seed);
    optixSetPayload_6(__float_as_uint(optixGetRayTmax()));
    setColor(color, meshId);
}
