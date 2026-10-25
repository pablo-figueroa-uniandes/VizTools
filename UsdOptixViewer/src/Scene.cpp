#include "Scene.h"

#include <algorithm>
#include <cmath>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/utils.h>

#include "third_party/stb_image.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Local-space triangle soup produced for one prim.
struct LocalMesh
{
    std::vector<GfVec3f> points;
    std::vector<unsigned int> indices;     // 3 point indices per triangle
    std::vector<unsigned int> cornerFv;    // 3 face-vertex indices per triangle
    std::vector<unsigned int> faceOfTri;   // source face of each triangle
    std::vector<GfVec3f> cornerNormals;    // 3 shading normals per triangle, empty = flat
    size_t faceCount = 1;
};

bool meshFromUsdMesh(const UsdGeomMesh& mesh, UsdTimeCode time, LocalMesh& out)
{
    VtArray<GfVec3f> points;
    VtArray<int> counts, faceIndices;
    if (!mesh.GetPointsAttr().Get(&points, time) || !mesh.GetFaceVertexCountsAttr().Get(&counts, time) ||
        !mesh.GetFaceVertexIndicesAttr().Get(&faceIndices, time))
        return false;

    out.points.assign(points.begin(), points.end());
    out.faceCount = counts.size();
    size_t offset = 0;
    for (size_t face = 0; face < counts.size(); ++face)
    {
        const int count = counts[face];
        if (count < 0 || offset + count > faceIndices.size())
            break;
        for (int k = 1; k + 1 < count; ++k)  // fan triangulation
        {
            const size_t c0 = offset, c1 = offset + k, c2 = offset + k + 1;
            const int i0 = faceIndices[c0], i1 = faceIndices[c1], i2 = faceIndices[c2];
            if (i0 < 0 || i1 < 0 || i2 < 0 || size_t(std::max({i0, i1, i2})) >= points.size())
                continue;
            out.indices.insert(out.indices.end(), {unsigned(i0), unsigned(i1), unsigned(i2)});
            out.cornerFv.insert(out.cornerFv.end(), {unsigned(c0), unsigned(c1), unsigned(c2)});
            out.faceOfTri.push_back(unsigned(face));
        }
        offset += count;
    }

    // Make every triangle counter-clockwise when seen from outside, so the
    // renderer can derive the outward normal (and the normal-map frame) from it.
    TfToken orientation;
    if (mesh.GetOrientationAttr().Get(&orientation) && orientation == UsdGeomTokens->leftHanded)
        for (size_t t = 0; t < out.indices.size(); t += 3)
        {
            std::swap(out.indices[t + 1], out.indices[t + 2]);
            std::swap(out.cornerFv[t + 1], out.cornerFv[t + 2]);
        }

    // Shading normals: authored primvars:normals or normals, otherwise smooth
    // vertex normals for subdivision surfaces (as Hydra does) and flat for
    // polygonal meshes.
    VtArray<GfVec3f> normals;
    TfToken interpolation;
    if (const UsdGeomPrimvar pv = UsdGeomPrimvarsAPI(mesh).GetPrimvar(UsdGeomTokens->normals);
        pv && pv.ComputeFlattened(&normals, time))
        interpolation = pv.GetInterpolation();
    else if (mesh.GetNormalsAttr().Get(&normals, time))
        interpolation = mesh.GetNormalsInterpolation();
    TfToken scheme;
    mesh.GetSubdivisionSchemeAttr().Get(&scheme);
    if (normals.empty() && scheme != UsdGeomTokens->none)
    {
        normals.assign(points.size(), GfVec3f(0.0f));
        for (size_t t = 0; t < out.indices.size(); t += 3)
        {
            const unsigned i0 = out.indices[t], i1 = out.indices[t + 1], i2 = out.indices[t + 2];
            const GfVec3f n = GfCross(points[i1] - points[i0], points[i2] - points[i0]);  // area weighted
            normals[i0] += n;
            normals[i1] += n;
            normals[i2] += n;
        }
        interpolation = UsdGeomTokens->vertex;
    }
    if (!normals.empty())
        for (size_t t = 0; t < out.indices.size(); t += 3)
            for (size_t c = 0; c < 3; ++c)
            {
                size_t i = 0;
                if (interpolation == UsdGeomTokens->faceVarying)
                    i = out.cornerFv[t + c];
                else if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying)
                    i = out.indices[t + c];
                else if (interpolation == UsdGeomTokens->uniform)
                    i = out.faceOfTri[t / 3];
                out.cornerNormals.push_back(i < normals.size() ? normals[i] : GfVec3f(0.0f));
            }
    return true;
}

void finishPrimitive(LocalMesh& out)
{
    out.cornerFv = out.indices;
    out.faceOfTri.assign(out.indices.size() / 3, 0u);
}

void meshFromCube(double size, LocalMesh& out)
{
    const float h = float(size) * 0.5f;
    for (int i = 0; i < 8; ++i)
        out.points.emplace_back(i & 1 ? h : -h, i & 2 ? h : -h, i & 4 ? h : -h);
    out.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                   2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    finishPrimitive(out);
}

void meshFromSphere(double radius, LocalMesh& out)
{
    const int slices = 48, stacks = 24;
    for (int j = 0; j <= stacks; ++j)
    {
        const double phi = M_PI * j / stacks;
        for (int i = 0; i <= slices; ++i)
        {
            const double theta = 2.0 * M_PI * i / slices;
            out.points.emplace_back(float(radius * std::sin(phi) * std::cos(theta)), float(radius * std::cos(phi)),
                                    float(radius * std::sin(phi) * std::sin(theta)));
        }
    }
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i)
        {
            const unsigned a = j * (slices + 1) + i, b = a + slices + 1;
            out.indices.insert(out.indices.end(), {a, a + 1, b, a + 1, b + 1, b});  // counter-clockwise from outside
        }
    finishPrimitive(out);
    for (unsigned idx : out.indices)
        out.cornerNormals.push_back(out.points[idx]);  // normalized later
}

// Value of a shader input, following connections to material interface
// inputs. Returns false when the input is unset or driven by another shader.
template <typename T>
bool inputValue(const UsdShadeShader& shader, const char* name, T* value)
{
    const UsdShadeInput input = shader.GetInput(TfToken(name));
    if (!input)
        return false;
    for (const UsdAttribute& attr : input.GetValueProducingAttributes())
        if (UsdShadeUtils::GetType(attr.GetName()) == UsdShadeAttributeType::Input && attr.Get(value))
            return true;
    return false;
}

// Shader output that drives the given input (through node graphs), if any.
UsdAttribute sourceOutput(const UsdShadeInput& input)
{
    if (input)
        for (const UsdAttribute& attr : input.GetValueProducingAttributes(/*shaderOutputsOnly=*/true))
            return attr;
    return UsdAttribute();
}

UsdShadeShader sourceShader(const UsdShadeInput& input)
{
    const UsdAttribute output = sourceOutput(input);
    return output ? UsdShadeShader(output.GetPrim()) : UsdShadeShader();
}

TfToken shaderId(const UsdShadeShader& shader)
{
    TfToken id;
    shader.GetShaderId(&id);
    return id;
}

TextureWrap wrapMode(const UsdShadeShader& shader, const char* name)
{
    TfToken mode;
    inputValue(shader, name, &mode);
    if (mode == "mirror")
        return TextureWrap::Mirror;
    if (mode == "clamp")
        return TextureWrap::Clamp;
    if (mode == "black")
        return TextureWrap::Black;
    return TextureWrap::Repeat;  // "repeat" and "useMetadata"
}

}  // namespace

bool Scene::load(const std::string& path, std::string& error)
{
    clear();
    UsdStageRefPtr stage = UsdStage::Open(path);
    if (!stage)
    {
        error = "Could not open USD stage: " + path;
        return false;
    }
    m_stage = stage;
    m_filePath = path;
    m_zUp = UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z;
    m_time = stage->HasAuthoredTimeCodeRange() ? UsdTimeCode(stage->GetStartTimeCode()) : UsdTimeCode::Default();
    m_root = createEntities(stage->GetPseudoRoot(), entt::null);
    extractGeometry();
    return true;
}

void Scene::clear()
{
    m_registry.clear();
    m_root = entt::null;
    m_geometry = FlatGeometry{};
    m_warnings.clear();
    m_materialCache.clear();
    m_textureCache.clear();
    m_stage = nullptr;
    m_filePath.clear();
}

void Scene::warn(const std::string& message)
{
    if (std::find(m_warnings.begin(), m_warnings.end(), message) == m_warnings.end())
        m_warnings.push_back(message);
}

UsdPrim Scene::prim(entt::entity e) const
{
    if (!m_stage || !m_registry.valid(e))
        return {};
    return m_stage->GetPrimAtPath(m_registry.get<PrimInfo>(e).path);
}

entt::entity Scene::createEntities(const UsdPrim& prim, entt::entity parent)
{
    const entt::entity e = m_registry.create();
    m_registry.emplace<PrimInfo>(e, prim.GetPath(), prim.IsPseudoRoot() ? std::string("/") : prim.GetName().GetString(),
                                 prim.GetTypeName().GetString());
    Hierarchy hierarchy{parent, {}};
    for (const UsdPrim& child : prim.GetFilteredChildren(UsdTraverseInstanceProxies()))
        hierarchy.children.push_back(createEntities(child, e));
    m_registry.emplace<Hierarchy>(e, std::move(hierarchy));
    return e;
}

// ---- materials and textures -------------------------------------------------
unsigned int Scene::displayColorMaterial(const UsdPrim& prim)
{
    Material m;
    m.name = "displayColor";
    VtArray<GfVec3f> colors;
    if (UsdGeomGprim(prim).GetDisplayColorPrimvar().ComputeFlattened(&colors, m_time) && !colors.empty())
        m.color = {colors[0][0], colors[0][1], colors[0][2]};
    m_geometry.materials.push_back(m);
    return unsigned(m_geometry.materials.size() - 1);
}

unsigned int Scene::materialFor(const UsdShadeMaterial& material)
{
    const SdfPath path = material.GetPath();
    if (auto it = m_materialCache.find(path); it != m_materialCache.end())
        return it->second;

    Material m;
    m.name = path.GetString();
    m.color = {0.18f, 0.18f, 0.18f};  // UsdPreviewSurface default
    if (const UsdShadeShader surface = material.ComputeSurfaceSource())
    {
        GfVec3f c;
        if (inputValue(surface, "diffuseColor", &c))
            m.color = {c[0], c[1], c[2]};
        else if (const UsdShadeInput in = surface.GetInput(TfToken("diffuseColor")); in && in.GetAttr().Get(&c))
            m.color = {c[0], c[1], c[2]};  // authored default beside a connection

        const UsdShadeShader texture = sourceShader(surface.GetInput(TfToken("diffuseColor")));
        if (texture && shaderId(texture) == "UsdUVTexture")
        {
            GfVec4f v;
            if (inputValue(texture, "scale", &v))
                m.scale = {v[0], v[1], v[2], v[3]};
            if (inputValue(texture, "bias", &v))
                m.bias = {v[0], v[1], v[2], v[3]};
            if (inputValue(texture, "fallback", &v))
                m.color = {v[0], v[1], v[2]};
            m.texture = loadTexture(texture);

            // texture coordinates come from a UsdPrimvarReader_float2
            const UsdShadeShader reader = sourceShader(texture.GetInput(TfToken("st")));
            VtValue varname;
            if (reader && shaderId(reader) == "UsdTransform2d")
                warn("UsdTransform2d texture transforms are ignored (" + m.name + ")");
            else if (reader && inputValue(reader, "varname", &varname))
            {
                if (varname.IsHolding<TfToken>())
                    m.uvSet = varname.UncheckedGet<TfToken>().GetString();
                else if (varname.IsHolding<std::string>())
                    m.uvSet = varname.UncheckedGet<std::string>();
            }
        }
        else if (texture)
            warn("Unsupported shader driving diffuseColor: " + shaderId(texture).GetString() + " (" + m.name + ")");

        // alpha cutout from a texture channel (translucency is not supported)
        float threshold = 0.0f;
        inputValue(surface, "opacityThreshold", &threshold);
        const UsdAttribute opacity = sourceOutput(surface.GetInput(TfToken("opacity")));
        if (opacity && shaderId(UsdShadeShader(opacity.GetPrim())) == "UsdUVTexture")
        {
            if (threshold > 0.0f)
            {
                const std::string channel = UsdShadeUtils::GetBaseNameAndType(opacity.GetName()).first.GetString();
                m.opacityTexture = loadTexture(UsdShadeShader(opacity.GetPrim()));
                m.opacityChannel = channel == "r" ? 0 : channel == "g" ? 1 : channel == "b" ? 2 : 3;
                m.opacityThreshold = threshold;
            }
            else
                warn("Translucent materials are rendered opaque (" + m.name + ")");
        }

        // specular: metallic or specular workflow
        m.roughness = scalarInput(surface, "roughness", 0.5f);
        m.metallic = scalarInput(surface, "metallic", 0.0f);
        inputValue(surface, "ior", &m.ior);
        int useSpecularWorkflow = 0;
        inputValue(surface, "useSpecularWorkflow", &useSpecularWorkflow);
        m.specularWorkflow = useSpecularWorkflow != 0;
        if (m.specularWorkflow)
        {
            GfVec3f c;
            if (inputValue(surface, "specularColor", &c))
                m.specularColor = {c[0], c[1], c[2]};
            const UsdShadeShader specular = sourceShader(surface.GetInput(TfToken("specularColor")));
            if (specular && shaderId(specular) == "UsdUVTexture")
            {
                m.specularTexture = loadTexture(specular);
                GfVec4f v;
                if (inputValue(specular, "scale", &v))
                    m.specularScale = {v[0], v[1], v[2], v[3]};
                if (inputValue(specular, "bias", &v))
                    m.specularBias = {v[0], v[1], v[2], v[3]};
            }
        }

        // tangent-space normal map
        const UsdShadeShader normal = sourceShader(surface.GetInput(TfToken("normal")));
        if (normal && shaderId(normal) == "UsdUVTexture")
        {
            m.normalTexture = loadTexture(normal, /*srgbByDefault=*/false);
            GfVec4f scale, bias;
            const bool hasScale = inputValue(normal, "scale", &scale), hasBias = inputValue(normal, "bias", &bias);
            if (hasScale || hasBias)
            {
                // as authored; UsdUVTexture defaults are scale 1, bias 0
                m.normalScale = hasScale ? std::array<float, 4>{scale[0], scale[1], scale[2], scale[3]}
                                         : std::array<float, 4>{1, 1, 1, 1};
                m.normalBias = hasBias ? std::array<float, 4>{bias[0], bias[1], bias[2], bias[3]}
                                       : std::array<float, 4>{0, 0, 0, 0};
            }
            else
                warn("Normal map without scale/bias, assuming the usual 2x-1 remap (" + m.name + ")");
            // 'uvSet' is shared with the diffuse texture when there is one
            if (m.texture < 0)
                if (const UsdShadeShader reader = sourceShader(normal.GetInput(TfToken("st"))))
                {
                    VtValue varname;
                    if (inputValue(reader, "varname", &varname))
                    {
                        if (varname.IsHolding<TfToken>())
                            m.uvSet = varname.UncheckedGet<TfToken>().GetString();
                        else if (varname.IsHolding<std::string>())
                            m.uvSet = varname.UncheckedGet<std::string>();
                    }
                }
        }
        else if (normal)
            warn("Unsupported shader driving normal: " + shaderId(normal).GetString() + " (" + m.name + ")");
    }
    m_geometry.materials.push_back(m);
    const unsigned index = unsigned(m_geometry.materials.size() - 1);
    m_materialCache[path] = index;
    return index;
}

ScalarInput Scene::scalarInput(const UsdShadeShader& surface, const char* name, float fallback)
{
    ScalarInput in;
    in.value = fallback;
    inputValue(surface, name, &in.value);
    const UsdAttribute output = sourceOutput(surface.GetInput(TfToken(name)));
    if (!output)
        return in;
    const UsdShadeShader texture(output.GetPrim());
    if (shaderId(texture) != "UsdUVTexture")
    {
        warn("Unsupported shader driving " + std::string(name) + ": " + shaderId(texture).GetString());
        return in;
    }
    const std::string channel = UsdShadeUtils::GetBaseNameAndType(output.GetName()).first.GetString();
    in.channel = channel == "g" ? 1 : channel == "b" ? 2 : channel == "a" ? 3 : 0;
    in.texture = loadTexture(texture, /*srgbByDefault=*/false);
    GfVec4f v;
    if (inputValue(texture, "scale", &v))
        in.scale = v[in.channel];
    if (inputValue(texture, "bias", &v))
        in.bias = v[in.channel];
    if (in.texture < 0 && inputValue(texture, "fallback", &v))
        in.value = v[in.channel];
    return in;
}

int Scene::loadTexture(const UsdShadeShader& shader, bool srgbByDefault)
{
    SdfAssetPath file;
    if (!inputValue(shader, "file", &file) || file.GetAssetPath().empty())
    {
        warn("Texture without a file: " + shader.GetPath().GetString());
        return -1;
    }
    if (file.GetAssetPath().find("<UDIM>") != std::string::npos)
    {
        warn("UDIM textures are not supported: " + file.GetAssetPath());
        return -1;
    }
    const std::string resolved = file.GetResolvedPath();
    if (resolved.empty())
    {
        warn("Texture not found: " + file.GetAssetPath());
        return -1;
    }

    TfToken colorSpace;
    inputValue(shader, "sourceColorSpace", &colorSpace);
    const bool srgb = colorSpace == "sRGB" || (colorSpace != "raw" && srgbByDefault);
    const TextureWrap wrapS = wrapMode(shader, "wrapS"), wrapT = wrapMode(shader, "wrapT");
    const auto key = std::make_tuple(resolved, srgb, int(wrapS), int(wrapT));
    if (auto it = m_textureCache.find(key); it != m_textureCache.end())
        return it->second;

    // ArResolver also reads files packaged inside .usdz archives
    int index = -1;
    std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(ArResolvedPath(resolved));
    std::shared_ptr<const char> buffer = asset ? asset->GetBuffer() : nullptr;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels =
        buffer ? stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(buffer.get()), int(asset->GetSize()), &w, &h,
                                       &channels, 4)
               : nullptr;
    if (pixels)
    {
        Texture t;
        t.path = resolved;
        t.width = w;
        t.height = h;
        t.rgba.assign(pixels, pixels + size_t(w) * h * 4);
        t.srgb = srgb;
        t.wrapS = wrapS;
        t.wrapT = wrapT;
        stbi_image_free(pixels);
        m_geometry.textures.push_back(std::move(t));
        index = int(m_geometry.textures.size() - 1);
    }
    else
        warn("Could not load texture " + resolved + (buffer ? std::string(": ") + stbi_failure_reason() : ""));
    m_textureCache[key] = index;
    return index;
}

// ---- geometry ---------------------------------------------------------------
void Scene::extractGeometry()
{
    UsdGeomXformCache xformCache(m_time);
    FlatGeometry& g = m_geometry;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};

    for (auto [e, info] : m_registry.view<PrimInfo>().each())
    {
        const UsdPrim p = m_stage->GetPrimAtPath(info.path);
        if (!p || !p.IsA<UsdGeomGprim>())
            continue;
        const UsdGeomImageable imageable(p);
        const TfToken purpose = imageable.ComputePurpose();
        if (imageable.ComputeVisibility(m_time) == UsdGeomTokens->invisible ||
            (purpose != UsdGeomTokens->default_ && purpose != UsdGeomTokens->render))
            continue;

        LocalMesh local;
        if (p.IsA<UsdGeomMesh>())
            meshFromUsdMesh(UsdGeomMesh(p), m_time, local);
        else if (p.IsA<UsdGeomCube>())
        {
            double size = 2.0;
            UsdGeomCube(p).GetSizeAttr().Get(&size, m_time);
            meshFromCube(size, local);
        }
        else if (p.IsA<UsdGeomSphere>())
        {
            double radius = 1.0;
            UsdGeomSphere(p).GetRadiusAttr().Get(&radius, m_time);
            meshFromSphere(radius, local);
        }
        if (local.indices.empty())
            continue;

        // materials: the prim's own binding, overridden per face by GeomSubsets
        UsdShadeMaterialBindingAPI binding(p);
        const UsdShadeMaterial primMaterial = binding.ComputeBoundMaterial();
        const unsigned baseMaterial = primMaterial ? materialFor(primMaterial) : displayColorMaterial(p);
        std::vector<unsigned> faceMaterial(local.faceCount, baseMaterial);
        unsigned subsetMaterials = 0;
        for (const UsdGeomSubset& subset : binding.GetMaterialBindSubsets())
        {
            const UsdShadeMaterial m = UsdShadeMaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial();
            VtIntArray faces;
            if (!m || !subset.GetIndicesAttr().Get(&faces, m_time))
                continue;
            const unsigned index = materialFor(m);
            for (int f : faces)
                if (f >= 0 && size_t(f) < faceMaterial.size())
                    faceMaterial[f] = index;
            ++subsetMaterials;
        }

        // texture coordinates, read from the primvar the material asks for
        std::string uvSet = g.materials[baseMaterial].uvSet;
        for (unsigned m : faceMaterial)
            if (g.materials[m].usesTextures())
            {
                uvSet = g.materials[m].uvSet;
                break;
            }
        VtArray<GfVec2f> uvs;
        TfToken interpolation;
        if (const UsdGeomPrimvar pv = UsdGeomPrimvarsAPI(p).FindPrimvarWithInheritance(TfToken(uvSet));
            pv && pv.ComputeFlattened(&uvs, m_time))
            interpolation = pv.GetInterpolation();
        auto cornerUV = [&](unsigned tri, int corner) -> GfVec2f {
            size_t i = 0;
            if (interpolation == UsdGeomTokens->faceVarying)
                i = local.cornerFv[3 * tri + corner];
            else if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying)
                i = local.indices[3 * tri + corner];
            else if (interpolation == UsdGeomTokens->uniform)
                i = local.faceOfTri[tri];
            return i < uvs.size() ? uvs[i] : GfVec2f(0.0f);
        };

        const GfMatrix4d toWorld = xformCache.GetLocalToWorldTransform(p);
        const GfMatrix4d normalToWorld = toWorld.GetInverse().GetTranspose();
        const unsigned baseVertex = unsigned(g.vertices.size() / 3);
        for (const GfVec3f& pt : local.points)
        {
            const GfVec3d w = toWorld.Transform(GfVec3d(pt));
            for (int k = 0; k < 3; ++k)
            {
                g.vertices.push_back(float(w[k]));
                lo[k] = std::min(lo[k], float(w[k]));
                hi[k] = std::max(hi[k], float(w[k]));
            }
        }

        const unsigned meshId = unsigned(g.meshEntity.size());
        const unsigned firstTri = unsigned(g.indices.size() / 3);
        const unsigned triCount = unsigned(local.indices.size() / 3);
        for (unsigned idx : local.indices)
            g.indices.push_back(baseVertex + idx);
        g.triMesh.insert(g.triMesh.end(), triCount, meshId);
        for (unsigned t = 0; t < triCount; ++t)
        {
            g.triMaterial.push_back(faceMaterial[local.faceOfTri[t]]);
            for (int c = 0; c < 3; ++c)
            {
                const GfVec2f uv = cornerUV(t, c);
                g.triUVs.insert(g.triUVs.end(), {uv[0], uv[1]});
                GfVec3d n(0.0);
                if (!local.cornerNormals.empty())
                {
                    n = normalToWorld.TransformDir(GfVec3d(local.cornerNormals[3 * t + c]));
                    if (n.GetLength() > 1e-12)
                        n.Normalize();
                }
                g.triNormals.insert(g.triNormals.end(), {float(n[0]), float(n[1]), float(n[2])});
            }
        }
        g.meshEntity.push_back(e);
        m_registry.emplace<MeshGeometry>(e, meshId, firstTri, triCount, baseMaterial, subsetMaterials,
                                         !uvs.empty());

        bool textured = false;
        for (unsigned m : faceMaterial)
            textured = textured || g.materials[m].usesTextures();
        if (textured && uvs.empty())
            warn("Textured prim without '" + uvSet + "' texture coordinates: " + info.path.GetString());
    }

    if (!g.empty())
        for (int k = 0; k < 3; ++k)
        {
            g.boundsMin[k] = lo[k];
            g.boundsMax[k] = hi[k];
        }
}
