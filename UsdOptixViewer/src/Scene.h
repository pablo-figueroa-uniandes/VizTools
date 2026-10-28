// A USD stage and what the renderer needs from it. The stage itself is the
// scene graph (hierarchy, names, types); the scene adds one indexed
// triangle mesh per renderable prim, its materials and textures, and the
// stage's cameras.
#pragma once

#include <array>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/material.h>

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

// One renderable prim (Mesh, Cube or Sphere) as an indexed triangle mesh in
// the prim's local space, placed in the world by `toWorld`. Vertices are the
// distinct corners of the source faces (position, shading normal and st),
// welded and reordered with meshoptimizer.
struct RenderMesh
{
    pxr::SdfPath path;
    std::array<float, 12> toWorld;          // row-major 3x4, acting on column vectors
    std::vector<float>        positions;    // xyz per vertex
    std::vector<float>        normals;      // xyz per vertex; empty for flat shading
    std::vector<float>        uvs;          // st per vertex; empty without texture coordinates
    std::vector<unsigned int> indices;      // 3 per triangle, counter-clockwise from outside
    std::vector<unsigned int> triMaterial;  // material of each triangle
    unsigned int material;         // material bound to the prim itself
    unsigned int subsetMaterials;  // number of GeomSubsets with their own material
    size_t corners;                // triangle corners before welding
    float boundsMin[3], boundsMax[3];  // world space

    size_t triangleCount() const { return indices.size() / 3; }
    size_t vertexCount() const { return positions.size() / 3; }
};

// A UsdGeomCamera at the stage's start time, in world space. The window is
// the image plane at distance 1 (perspective) or the view size in scene
// units (orthographic); the viewer fits it to the viewport's aspect ratio.
struct SceneCamera
{
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
    size_t primCount() const { return m_primCount; }
    // The mesh id of a mesh is its index in meshes().
    const std::vector<RenderMesh>& meshes() const { return m_meshes; }
    const std::vector<Material>& materials() const { return m_materials; }
    const std::vector<Texture>& textures() const { return m_textures; }
    const std::vector<SceneCamera>& cameras() const { return m_cameras; }
    const std::vector<std::string>& warnings() const { return m_warnings; }
    bool zUp() const { return m_zUp; }
    const float* boundsMin() const { return m_boundsMin; }
    const float* boundsMax() const { return m_boundsMax; }
    size_t triangleCount() const;

    // -1 when the prim has no mesh or is not a camera
    int meshIndex(const pxr::SdfPath& path) const;
    int cameraIndex(const pxr::SdfPath& path) const;

private:
    void extractGeometry();
    void extractCameras();
    unsigned int materialFor(const pxr::UsdShadeMaterial& material);
    unsigned int displayColorMaterial(const pxr::UsdPrim& prim);
    // srgbByDefault: color space when sourceColorSpace is neither "raw" nor "sRGB"
    int loadTexture(const pxr::UsdShadeShader& shader, bool srgbByDefault = true);
    ScalarInput scalarInput(const pxr::UsdShadeShader& surface, const char* name, float fallback);
    void warn(const std::string& message);

    std::string         m_filePath;
    pxr::UsdStageRefPtr m_stage;
    pxr::UsdTimeCode    m_time;
    size_t              m_primCount = 0;
    std::vector<RenderMesh>  m_meshes;
    std::vector<Material>    m_materials;
    std::vector<Texture>     m_textures;
    std::vector<SceneCamera> m_cameras;
    std::vector<std::string> m_warnings;
    std::unordered_map<pxr::SdfPath, unsigned int, pxr::SdfPath::Hash> m_meshByPath;
    std::map<pxr::SdfPath, unsigned int> m_materialCache;
    std::map<std::tuple<std::string, bool, int, int>, int> m_textureCache;
    float               m_boundsMin[3] = {0, 0, 0};
    float               m_boundsMax[3] = {0, 0, 0};
    bool                m_zUp = false;
};
