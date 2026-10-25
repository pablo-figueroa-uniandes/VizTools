// A USD stage mirrored into an EnTT registry: one entity per prim, with
// components describing its place in the hierarchy and its geometry.
#pragma once

#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <entt/entt.hpp>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>

// ---- components -----------------------------------------------------------
struct PrimInfo
{
    pxr::SdfPath path;
    std::string  name;
    std::string  typeName;
};

struct Hierarchy
{
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

// Present on prims that contributed triangles to the rendered geometry.
struct MeshGeometry
{
    unsigned int meshId;
    unsigned int firstTriangle;
    unsigned int triangleCount;
    unsigned int material;        // material bound to the prim itself
    unsigned int subsetMaterials;  // number of GeomSubsets with their own material
    bool hasUVs;
};

// Present on UsdGeomCamera prims: index into Scene::cameras().
struct CameraView
{
    unsigned int index;
};

// ---- render data ----------------------------------------------------------
enum class TextureWrap { Repeat, Mirror, Clamp, Black };

struct Texture
{
    std::string path;  // resolved path (may point inside a .usdz)
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
    bool srgb = true;
    TextureWrap wrapS = TextureWrap::Repeat, wrapT = TextureWrap::Repeat;
};

// A float material input: a constant, or one channel of a texture
// (texture[channel] * scale + bias).
struct ScalarInput
{
    float value = 0.0f;
    int texture = -1;
    int channel = 0;
    float scale = 1.0f, bias = 0.0f;
};

// Simplified UsdPreviewSurface: diffuse color, optionally from a texture
// (texture * scale + bias), tangent-space normal maps, alpha cutouts
// (opacity + opacityThreshold) and GGX specular in the metallic or the
// specular workflow.
struct Material
{
    std::string name;  // material prim path, or "displayColor"
    std::array<float, 3> color = {0.7f, 0.7f, 0.7f};
    int texture = -1;
    std::array<float, 4> scale = {1, 1, 1, 1};
    std::array<float, 4> bias = {0, 0, 0, 0};
    std::string uvSet = "st";  // primvar read by the texture
    // alpha cutout: hits where opacityTexture[opacityChannel] < opacityThreshold are ignored
    int opacityTexture = -1;
    int opacityChannel = 3;
    float opacityThreshold = 0.0f;
    // tangent-space normal = normalTexture * normalScale + normalBias
    int normalTexture = -1;
    std::array<float, 4> normalScale = {2, 2, 2, 1};
    std::array<float, 4> normalBias = {-1, -1, -1, 0};

    ScalarInput roughness{0.5f};
    ScalarInput metallic{0.0f};
    float ior = 1.5f;
    // useSpecularWorkflow: specularColor replaces metallic as the reflectance at normal incidence
    bool specularWorkflow = false;
    std::array<float, 3> specularColor = {0, 0, 0};
    int specularTexture = -1;
    std::array<float, 4> specularScale = {1, 1, 1, 1};
    std::array<float, 4> specularBias = {0, 0, 0, 0};

    bool usesTextures() const
    {
        return texture >= 0 || opacityTexture >= 0 || normalTexture >= 0 || roughness.texture >= 0 ||
               metallic.texture >= 0 || specularTexture >= 0;
    }
};

// Geometry flattened to world space, ready to upload to the renderer.
struct FlatGeometry
{
    std::vector<float>        vertices;     // xyz
    std::vector<unsigned int> indices;      // 3 per triangle
    std::vector<unsigned int> triMesh;      // mesh id per triangle
    std::vector<unsigned int> triMaterial;  // material per triangle
    std::vector<float>        triUVs;       // 6 per triangle (st of each corner)
    std::vector<float>        triNormals;   // 9 per triangle (shading normal of each corner, 0 = flat)
    std::vector<Material>     materials;
    std::vector<Texture>      textures;
    std::vector<entt::entity> meshEntity;   // entity of each mesh id
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};

    bool empty() const { return indices.empty(); }
};

// A UsdGeomCamera at the stage's start time, in world space. The window is
// the image plane at distance 1 (perspective) or the view size in scene
// units (orthographic); the viewer fits it to the viewport's aspect ratio.
struct SceneCamera
{
    entt::entity entity = entt::null;
    std::string path;
    std::array<float, 3> eye = {0, 0, 0}, forward = {0, 0, -1}, up = {0, 1, 0};
    float halfWidth = 1.0f, halfHeight = 1.0f;
    bool orthographic = false;
    float clipNear = 0.0f, clipFar = 1e16f;
    float fovYDeg = 0.0f;  // vertical field of view of a perspective camera
};

class Scene
{
public:
    bool load(const std::string& path, std::string& error);
    void clear();

    bool loaded() const { return static_cast<bool>(m_stage); }
    const std::string& filePath() const { return m_filePath; }
    const pxr::UsdStageRefPtr& stage() const { return m_stage; }
    entt::registry& registry() { return m_registry; }
    entt::entity root() const { return m_root; }
    const FlatGeometry& geometry() const { return m_geometry; }
    const std::vector<SceneCamera>& cameras() const { return m_cameras; }
    const std::vector<std::string>& warnings() const { return m_warnings; }
    bool zUp() const { return m_zUp; }

    pxr::UsdPrim prim(entt::entity e) const;

private:
    entt::entity createEntities(const pxr::UsdPrim& prim, entt::entity parent);
    void extractGeometry();
    void extractCameras(entt::entity e);
    unsigned int materialFor(const pxr::UsdShadeMaterial& material);
    unsigned int displayColorMaterial(const pxr::UsdPrim& prim);
    // srgbByDefault: color space when sourceColorSpace is neither "raw" nor "sRGB"
    int loadTexture(const pxr::UsdShadeShader& shader, bool srgbByDefault = true);
    ScalarInput scalarInput(const pxr::UsdShadeShader& surface, const char* name, float fallback);
    void warn(const std::string& message);

    std::string         m_filePath;
    pxr::UsdStageRefPtr m_stage;
    pxr::UsdTimeCode    m_time;
    entt::registry      m_registry;
    entt::entity        m_root = entt::null;
    FlatGeometry        m_geometry;
    std::vector<SceneCamera> m_cameras;
    std::vector<std::string> m_warnings;
    std::map<pxr::SdfPath, unsigned int> m_materialCache;
    std::map<std::tuple<std::string, bool, int, int>, int> m_textureCache;
    bool                m_zUp = false;
};
