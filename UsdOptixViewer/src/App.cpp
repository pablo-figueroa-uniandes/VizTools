#include "App.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <optix.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr unsigned int kMaxSamples = 256;  // progressive samples per pixel

constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

std::string valueString(const VtValue& value, size_t maxLength = 160)
{
    std::string s = value.IsEmpty() ? std::string("<no value>") : TfStringify(value);
    if (s.size() > maxLength)
        s = s.substr(0, maxLength) + " ...";
    return s;
}

bool containsCaseInsensitive(const std::string& text, const std::string& pattern)
{
    return std::search(text.begin(), text.end(), pattern.begin(), pattern.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}

}  // namespace

App::~App()
{
    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_texture)
        SDL_DestroyTexture(m_texture);
    if (m_mirrorTexture)
        SDL_DestroyTexture(m_mirrorTexture);
    if (m_sdlRenderer)
        SDL_DestroyRenderer(m_sdlRenderer);
    if (m_window)
        SDL_DestroyWindow(m_window);
    SDL_Quit();
}

bool App::init(int argc, char** argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    m_window = SDL_CreateWindow("USD OptiX Viewer", int(1600 * scale), int(960 * scale),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    m_sdlRenderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_sdlRenderer)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(m_sdlRenderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowRounding = 0.0f;
    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_sdlRenderer);
    ImGui_ImplSDLRenderer3_Init(m_sdlRenderer);

    // UsdOptixViewer [file.usd] [--select /Prim/Path] [--camera /Camera/Path] [--no-mipmaps] [--no-normal-maps]
    //                [--no-reflections] [--xr] [--cloudxr] [--cloudxr-sdk DIR] [--cloudxr-profile NAME]
    //                [--xr-samples N] [--xr-resolution F] [--xr-mirror]
    std::string file, selectPath, cameraPath, cloudXrSdk, cloudXrProfile;
    bool xr = false, cloudXr = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--select" && i + 1 < argc)
            selectPath = argv[++i];
        else if (arg == "--camera" && i + 1 < argc)
            cameraPath = argv[++i];
        else if (arg == "--xr")
            xr = true;
        else if (arg == "--cloudxr")
            cloudXr = true;
        else if (arg == "--cloudxr-sdk" && i + 1 < argc)
            cloudXrSdk = argv[++i];
        else if (arg == "--cloudxr-profile" && i + 1 < argc)
            cloudXrProfile = argv[++i];
        else if (arg == "--xr-samples" && i + 1 < argc)
            m_xrSamples = std::clamp(std::atoi(argv[++i]), 1, 64);
        else if (arg == "--xr-resolution" && i + 1 < argc)
            m_xrResolution = std::clamp(float(std::atof(argv[++i])), 0.1f, 2.0f);
        else if (arg == "--xr-mirror")
            m_xrMirror = true;
        else if (arg == "--no-mipmaps")
            m_mipmaps = false;
        else if (arg == "--no-normal-maps")
            m_normalMaps = false;
        else if (arg == "--no-reflections")
            m_reflections = false;
        else
            file = arg;
    }

    // The XR runtime must exist before the renderer starts, because it may
    // choose another GPU (see updateXr).
    if (xr || cloudXr)
        startXr(cloudXr, cloudXrSdk, cloudXrProfile);
    std::string error;
    m_rendererOk = m_renderer.init(error);
    if (!m_rendererOk)
        m_error = "OptiX initialization failed: " + error;

    if (!file.empty())
        loadFile(file);
    if (!selectPath.empty() && m_scene.loaded() && SdfPath::IsValidPathString(selectPath))
    {
        const SdfPath path(selectPath);
        for (auto [e, info] : m_scene.registry().view<PrimInfo>().each())
            if (info.path == path)
            {
                select(e, true);
                frameSelection();
            }
    }
    if (!cameraPath.empty())
    {
        const std::vector<SceneCamera>& cameras = m_scene.cameras();
        const auto it = std::find_if(cameras.begin(), cameras.end(),
                                     [&](const SceneCamera& c) { return c.path == cameraPath; });
        if (it != cameras.end())
            lookThrough(int(it - cameras.begin()));
        else
            m_error = "No camera at " + cameraPath;
    }
    return true;
}

void App::requestOpen(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingOpen = path;
}

void App::loadFile(const std::string& path)
{
    std::string error;
    m_camera = -1;  // camera indices refer to the previous stage
    m_selected = entt::null;
    m_reveal.clear();
    m_appliedFilter.clear();
    m_filter[0] = '\0';
    if (!m_scene.load(path, error))
    {
        m_error = error;
        return;
    }
    SDL_strlcpy(m_pathInput, path.c_str(), sizeof(m_pathInput));
    SDL_SetWindowTitle(m_window, ("USD OptiX Viewer - " + path).c_str());
    if (m_rendererOk)
    {
        m_renderer.setGeometry(m_scene.geometry());
        m_renderer.setSelectedMesh(kNoMesh);
    }
    const FlatGeometry& g = m_scene.geometry();
    frameBounds(g.boundsMin, g.boundsMax);
}

void App::run()
{
    while (m_running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            processEvent(event);

        std::string pending;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pending.swap(m_pendingOpen);
        }
        if (!pending.empty())
            loadFile(pending);

        updateXr();  // keeps streaming while the window is minimized
        if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawUi();
        ImGui::Render();

        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(m_sdlRenderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(m_sdlRenderer, 0.08f, 0.08f, 0.09f, 1.0f);
        SDL_RenderClear(m_sdlRenderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_sdlRenderer);
        SDL_RenderPresent(m_sdlRenderer);
    }
}

void App::processEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type)
    {
    case SDL_EVENT_QUIT:
        m_running = false;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == SDL_GetWindowID(m_window))
            m_running = false;
        break;
    case SDL_EVENT_DROP_FILE:
        if (event.drop.data)
            requestOpen(event.drop.data);
        break;
    default:
        break;
    }
}

// ---- layout ---------------------------------------------------------------
void App::drawUi()
{
    drawMenuBar();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float statusH = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;
    const float top = vp->WorkPos.y, bottom = vp->Pos.y + vp->Size.y - statusH;
    const float leftW = std::max(260.0f, vp->Size.x * 0.22f), rightW = std::max(320.0f, vp->Size.x * 0.28f);

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, top));
    ImGui::SetNextWindowSize(ImVec2(leftW, bottom - top));
    drawHierarchy();

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - rightW, top));
    ImGui::SetNextWindowSize(ImVec2(rightW, bottom - top));
    drawInspector();

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + leftW, top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x - leftW - rightW, bottom - top));
    drawViewport();

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, bottom));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, statusH));
    drawStatusBar();

    drawPopups();

    if (!ImGui::GetIO().WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_F))
            frameSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            select(entt::null, false);
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_O))
            openFileDialog();
    }
}

void App::drawMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Open...", "Ctrl+O"))
            openFileDialog();
        if (ImGui::BeginMenu("Open path"))
        {
            ImGui::SetNextItemWidth(420.0f);
            const bool enter = ImGui::InputText("##path", m_pathInput, sizeof(m_pathInput),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Load") || enter) && m_pathInput[0])
                requestOpen(m_pathInput);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reload", nullptr, false, m_scene.loaded()))
            requestOpen(m_scene.filePath());
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
            m_running = false;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Frame all"))
        {
            const FlatGeometry& g = m_scene.geometry();
            frameBounds(g.boundsMin, g.boundsMax);
        }
        if (ImGui::MenuItem("Frame selection", "F", false, m_selected != entt::null))
            frameSelection();
        if (ImGui::MenuItem("Shadows", nullptr, &m_shadows))
            m_needsRender = true;
        if (ImGui::MenuItem("Texture mipmaps", nullptr, &m_mipmaps))
            m_needsRender = true;
        if (ImGui::MenuItem("Normal maps", nullptr, &m_normalMaps))
            m_needsRender = true;
        if (ImGui::MenuItem("Reflections", nullptr, &m_reflections))
            m_needsRender = true;
        if (ImGui::SliderFloat3("Light direction", m_light, -1.0f, 1.0f))
            m_needsRender = true;
        ImGui::BeginDisabled(m_camera >= 0);  // a USD camera has its own field of view
        if (ImGui::SliderFloat("Field of view", &m_fovY, 15.0f, 100.0f))
            m_needsRender = true;
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Camera"))
    {
        if (ImGui::MenuItem("Orbit camera", nullptr, m_camera < 0))
            orbitFromUsdCamera();
        const std::vector<SceneCamera>& cameras = m_scene.cameras();
        if (cameras.empty())
            ImGui::TextDisabled("This stage has no cameras.");
        else
            ImGui::SeparatorText("USD cameras");
        for (size_t i = 0; i < cameras.size(); ++i)
            if (ImGui::MenuItem(cameras[i].path.c_str(), nullptr, m_camera == int(i)))
                lookThrough(int(i));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("XR"))
    {
        if (m_xr.state() == XrOutput::State::Off)
            ImGui::TextDisabled("XR output is off. Start with --xr or --cloudxr.");
        else
        {
            ImGui::Text("Runtime: %s", m_xr.runtimeName().c_str());
            if (m_cloudXr.running())
                ImGui::Text("CloudXR %s in this process, client %s", m_cloudXr.version().c_str(),
                            m_xrClient ? "connected" : "not connected");
            ImGui::Text("State: %s", xrStatus().c_str());
            if (m_xr.eyeCount())
                ImGui::Text("%s on %s, %u eyes of %u x %u", m_xr.systemName().c_str(), m_xr.gpuName().c_str(),
                            m_xr.eyeCount(), m_xr.eyeWidth(), m_xr.eyeHeight());
        }
        ImGui::SeparatorText("Headset view");
        ImGui::SliderFloat("World scale", &m_xrWorldScale, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("Samples per frame", &m_xrSamples, 1, 8);
        ImGui::MenuItem("Mirror left eye in viewport", nullptr, &m_xrMirror);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About"))
            m_showAbout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::openFileDialog()
{
    static const SDL_DialogFileFilter filters[] = {{"USD files", "usd;usda;usdc;usdz"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* files, int) {
            if (files && files[0])
                static_cast<App*>(userdata)->requestOpen(files[0]);
            else if (!files)
                SDL_Log("File dialog error: %s", SDL_GetError());
        },
        this, m_window, filters, 2, nullptr, false);
}

// ---- hierarchy ------------------------------------------------------------
void App::updateFilter()
{
    if (m_appliedFilter == m_filter)
        return;
    m_appliedFilter = m_filter;
    m_visibleByFilter.clear();
    if (m_appliedFilter.empty())
        return;
    entt::registry& reg = m_scene.registry();
    for (auto [e, info] : reg.view<PrimInfo>().each())
    {
        if (!containsCaseInsensitive(info.name, m_appliedFilter))
            continue;
        for (entt::entity a = e; a != entt::null && m_visibleByFilter.insert(a).second;)
            a = reg.get<Hierarchy>(a).parent;
    }
}

void App::drawHierarchy()
{
    ImGui::Begin("Hierarchy", nullptr, kPanelFlags);
    if (!m_scene.loaded())
    {
        ImGui::TextWrapped("No stage loaded.\n\nUse File > Open, drop a .usd/.usda/.usdc/.usdz file on the "
                           "window, or pass a file on the command line.");
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter by name", m_filter, sizeof(m_filter));
    updateFilter();
    ImGui::Separator();
    ImGui::BeginChild("tree");
    for (entt::entity child : m_scene.registry().get<Hierarchy>(m_scene.root()).children)
        drawHierarchyNode(child);
    m_revealSelection = false;
    ImGui::EndChild();
    ImGui::End();
}

void App::drawHierarchyNode(entt::entity e)
{
    if (!m_appliedFilter.empty() && !m_visibleByFilter.contains(e))
        return;
    entt::registry& reg = m_scene.registry();
    const PrimInfo& info = reg.get<PrimInfo>(e);
    const Hierarchy& h = reg.get<Hierarchy>(e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (h.children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (e == m_selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (m_revealSelection && m_reveal.contains(e))
        ImGui::SetNextItemOpen(true);
    if (!m_appliedFilter.empty())
        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);

    const bool hasMesh = reg.all_of<MeshGeometry>(e);
    if (hasMesh)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entt::to_integral(e))), flags,
                                        "%s", info.name.c_str());
    if (hasMesh)
        ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        select(e, false);
    if (m_revealSelection && e == m_selected)
        ImGui::SetScrollHereY();
    if (!info.typeName.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", info.typeName.c_str());
    }

    if (open && !h.children.empty())
    {
        for (entt::entity child : h.children)
            drawHierarchyNode(child);
        ImGui::TreePop();
    }
}

void App::select(entt::entity e, bool reveal)
{
    m_selected = e;
    entt::registry& reg = m_scene.registry();
    const MeshGeometry* mesh = reg.valid(e) ? reg.try_get<MeshGeometry>(e) : nullptr;
    m_renderer.setSelectedMesh(mesh ? mesh->meshId : kNoMesh);
    m_needsRender = true;
    m_reveal.clear();
    if (reveal && reg.valid(e))
    {
        for (entt::entity a = reg.get<Hierarchy>(e).parent; a != entt::null; a = reg.get<Hierarchy>(a).parent)
            m_reveal.insert(a);
        m_revealSelection = true;
    }
}

// ---- inspector ------------------------------------------------------------
void App::drawInspector()
{
    ImGui::Begin("Inspector", nullptr, kPanelFlags);
    const UsdPrim prim = m_scene.prim(m_selected);
    if (!prim)
    {
        if (m_scene.loaded())
        {
            const UsdStageRefPtr& stage = m_scene.stage();
            ImGui::SeparatorText("Stage");
            ImGui::TextWrapped("%s", m_scene.filePath().c_str());
            ImGui::Text("Up axis: %s", m_scene.zUp() ? "Z" : "Y");
            ImGui::Text("Meters per unit: %g", UsdGeomGetStageMetersPerUnit(stage));
            if (stage->HasAuthoredTimeCodeRange())
                ImGui::Text("Time codes: %g - %g", stage->GetStartTimeCode(), stage->GetEndTimeCode());
            if (SdfPrimSpecHandle def = stage->GetRootLayer()->GetPrimAtPath(stage->GetDefaultPrim().GetPath()))
                ImGui::Text("Default prim: %s", def->GetName().c_str());
            const FlatGeometry& g = m_scene.geometry();
            ImGui::Text("Materials: %zu   Textures: %zu   Cameras: %zu", g.materials.size(), g.textures.size(),
                        m_scene.cameras().size());
            if (!m_scene.warnings().empty())
            {
                ImGui::SeparatorText("Warnings");
                for (const std::string& w : m_scene.warnings())
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", w.c_str());
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Select a prim in the hierarchy or click on the viewport.");
        }
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Prim");
    ImGui::TextWrapped("%s", prim.GetPath().GetText());
    if (ImGui::BeginTable("prim", 2, ImGuiTableFlags_SizingStretchProp))
    {
        auto row = [](const char* key, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", key);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", value.c_str());
        };
        row("Type", prim.GetTypeName().IsEmpty() ? "(typeless)" : prim.GetTypeName().GetString());
        row("Specifier", TfStringReplace(TfStringify(prim.GetSpecifier()), "SdfSpecifier", ""));
        TfToken kind;
        if (UsdModelAPI(prim).GetKind(&kind) && !kind.IsEmpty())
            row("Kind", kind.GetString());
        row("Active", prim.IsActive() ? "yes" : "no");
        if (prim.IsInstance() || prim.IsInstanceProxy())
            row("Instancing", prim.IsInstance() ? "instance" : "instance proxy");
        if (prim.HasAuthoredReferences())
            row("Composition", "has references");
        const std::vector<std::string> schemas = [&] {
            std::vector<std::string> out;
            for (const TfToken& t : prim.GetAppliedSchemas())
                out.push_back(t.GetString());
            return out;
        }();
        if (!schemas.empty())
            row("API schemas", TfStringJoin(schemas, ", "));
        ImGui::EndTable();
    }

    if (const CameraView* view = m_scene.registry().try_get<CameraView>(m_selected))
    {
        const SceneCamera& c = m_scene.cameras()[view->index];
        ImGui::SeparatorText("Camera");
        if (c.orthographic)
            ImGui::Text("Orthographic, %g x %g units", 2.0f * c.halfWidth, 2.0f * c.halfHeight);
        else
            ImGui::Text("Perspective, vertical field of view %.1f deg", c.fovYDeg);
        ImGui::Text("Clipping range: %g - %g", c.clipNear, c.clipFar);
        const bool active = m_camera == int(view->index);
        ImGui::BeginDisabled(active);
        if (ImGui::Button(active ? "Looking through this camera" : "Look through"))
            lookThrough(int(view->index));
        ImGui::EndDisabled();
    }

    if (const MeshGeometry* mesh = m_scene.registry().try_get<MeshGeometry>(m_selected))
    {
        ImGui::SeparatorText("Rendered geometry");
        ImGui::Text("Triangles: %u", mesh->triangleCount);
        const FlatGeometry& g = m_scene.geometry();
        const Material& material = g.materials[mesh->material];
        ImGui::TextWrapped("Material: %s", material.name.c_str());
        if (material.texture >= 0)
        {
            const Texture& t = g.textures[material.texture];
            ImGui::TextWrapped("Diffuse texture: %s", t.path.c_str());
            ImGui::Text("  %d x %d, %s, uv set '%s'%s", t.width, t.height, t.srgb ? "sRGB" : "raw",
                        material.uvSet.c_str(), mesh->hasUVs ? "" : " (missing!)");
        }
        else
        {
            float color[3] = {material.color[0], material.color[1], material.color[2]};
            ImGui::ColorEdit3("Diffuse color", color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
        }
        if (material.normalTexture >= 0)
        {
            const Texture& t = g.textures[material.normalTexture];
            ImGui::TextWrapped("Normal map: %s", t.path.c_str());
            ImGui::Text("  %d x %d, %s, scale (%g, %g, %g), bias (%g, %g, %g)", t.width, t.height,
                        t.srgb ? "sRGB" : "raw", material.normalScale[0], material.normalScale[1],
                        material.normalScale[2], material.normalBias[0], material.normalBias[1],
                        material.normalBias[2]);
        }
        auto scalarText = [&](const char* label, const ScalarInput& in) {
            if (in.texture >= 0)
                ImGui::TextWrapped("%s: texture channel %c of %s (x%g %+g)", label, "rgba"[in.channel],
                                   g.textures[in.texture].path.c_str(), in.scale, in.bias);
            else
                ImGui::Text("%s: %g", label, in.value);
        };
        scalarText("Roughness", material.roughness);
        if (material.specularWorkflow)
        {
            if (material.specularTexture >= 0)
                ImGui::TextWrapped("Specular color: %s", g.textures[material.specularTexture].path.c_str());
            else
            {
                float color[3] = {material.specularColor[0], material.specularColor[1], material.specularColor[2]};
                ImGui::ColorEdit3("Specular color", color,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
            }
        }
        else
            scalarText("Metallic", material.metallic);
        ImGui::Text("IOR: %g", material.ior);
        if (mesh->subsetMaterials)
            ImGui::Text("GeomSubsets with their own material: %u", mesh->subsetMaterials);
        if (ImGui::Button("Frame (F)"))
            frameSelection();
    }

    ImGui::SeparatorText("Properties");
    if (ImGui::BeginTable("props", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();
        for (const UsdAttribute& attr : prim.GetAttributes())
        {
            VtValue value;
            attr.Get(&value, UsdTimeCode::Default());
            if (value.IsEmpty() && attr.GetNumTimeSamples() > 0)
                attr.Get(&value, UsdTimeCode::EarliestTime());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (attr.HasAuthoredValue())
                ImGui::TextUnformatted(attr.GetName().GetText());
            else
                ImGui::TextDisabled("%s", attr.GetName().GetText());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", attr.GetTypeName().GetAsToken().GetText());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", valueString(value).c_str());
            if (attr.GetNumTimeSamples() > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu samples)", attr.GetNumTimeSamples());
            }
        }
        for (const UsdRelationship& rel : prim.GetRelationships())
        {
            SdfPathVector targets;
            rel.GetTargets(&targets);
            std::vector<std::string> names;
            for (const SdfPath& p : targets)
                names.push_back(p.GetString());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rel.GetName().GetText());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("rel");
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", names.empty() ? "<none>" : TfStringJoin(names, ", ").c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---- viewport -------------------------------------------------------------
void App::updateCamera()
{
    if (m_camera >= 0)
    {
        const SceneCamera& c = m_scene.cameras()[m_camera];
        float halfW, halfH;
        fittedWindow(c, halfW, halfH);
        m_renderer.setCameraView(c.eye.data(), c.forward.data(), c.up.data(), halfW, halfH, c.orthographic,
                                 c.clipNear, c.clipFar);
    }
    else
    {
        float offset[3], up[3], eye[3];
        orbitOffset(offset, up);
        for (int k = 0; k < 3; ++k)
            eye[k] = m_target[k] + offset[k] * m_distance;
        m_renderer.setCamera(eye, m_target, up, m_fovY);
    }
    m_renderer.setLightDir(m_light);
    m_renderer.setShadows(m_shadows);
    m_renderer.setMipmaps(m_mipmaps);
    m_renderer.setNormalMaps(m_normalMaps);
    m_renderer.setReflections(m_reflections);
}

// Unit vector from the orbit target to the eye, and the world's up axis.
void App::orbitOffset(float offset[3], float up[3]) const
{
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch), cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    if (m_scene.zUp())
    {
        offset[0] = cp * cy, offset[1] = cp * sy, offset[2] = sp;
        up[0] = 0, up[1] = 0, up[2] = 1;
    }
    else
    {
        offset[0] = cp * sy, offset[1] = sp, offset[2] = cp * cy;
        up[0] = 0, up[1] = 1, up[2] = 0;
    }
}

// Eye and viewing direction of the desktop view, orbit or USD camera.
void App::viewFrame(float eye[3], float forward[3]) const
{
    if (m_camera >= 0)
    {
        const SceneCamera& c = m_scene.cameras()[m_camera];
        std::copy(c.eye.begin(), c.eye.end(), eye);
        std::copy(c.forward.begin(), c.forward.end(), forward);
        return;
    }
    float offset[3], up[3];
    orbitOffset(offset, up);
    for (int k = 0; k < 3; ++k)
    {
        eye[k] = m_target[k] + offset[k] * m_distance;
        forward[k] = -offset[k];
    }
}

// The camera's window grown along one axis to the viewport's aspect ratio, so
// the whole window stays visible (the "fit" conform policy of usdview).
void App::fittedWindow(const SceneCamera& camera, float& halfWidth, float& halfHeight) const
{
    const float aspect = float(m_renderer.width()) / float(std::max(m_renderer.height(), 1u));
    halfWidth = camera.halfWidth;
    halfHeight = camera.halfHeight;
    if (aspect > halfWidth / halfHeight)
        halfWidth = halfHeight * aspect;
    else
        halfHeight = halfWidth / aspect;
}

void App::lookThrough(int camera)
{
    m_camera = camera;
    m_needsRender = true;
}

// Hands the view over to the orbit camera, which starts where the USD camera
// is, looking the same way. It orbits a point in front of the camera at the
// depth of the scene's center. Roll is lost, and an orthographic camera
// becomes a perspective one that shows the same size at that point.
void App::orbitFromUsdCamera()
{
    if (m_camera < 0)
        return;
    const SceneCamera& c = m_scene.cameras()[m_camera];
    float halfW, halfH;
    fittedWindow(c, halfW, halfH);
    m_camera = -1;
    m_needsRender = true;

    const FlatGeometry& g = m_scene.geometry();
    const float* f = c.forward.data();
    float depth = 0.0f, radius = 0.0f;
    for (int k = 0; k < 3; ++k)
    {
        depth += (0.5f * (g.boundsMin[k] + g.boundsMax[k]) - c.eye[k]) * f[k];
        radius += (g.boundsMax[k] - g.boundsMin[k]) * (g.boundsMax[k] - g.boundsMin[k]);
    }
    if (depth < 1e-3f)
        depth = std::max(0.5f * std::sqrt(radius), 1.0f);
    for (int k = 0; k < 3; ++k)
        m_target[k] = c.eye[k] + f[k] * depth;
    if (c.orthographic)
        m_distance = halfH / std::tan(0.5f * m_fovY * float(M_PI) / 180.0f);
    else
    {
        m_distance = depth;
        m_fovY = 2.0f * std::atan(halfH) * 180.0f / float(M_PI);
    }
    // the orbit offset (from the target to the eye) is -forward
    if (m_scene.zUp())
        m_pitch = std::asin(std::clamp(-f[2], -1.0f, 1.0f)), m_yaw = std::atan2(-f[1], -f[0]);
    else
        m_pitch = std::asin(std::clamp(-f[1], -1.0f, 1.0f)), m_yaw = std::atan2(-f[0], -f[2]);
    m_pitch = std::clamp(m_pitch, -1.55f, 1.55f);
}

void App::frameBounds(const float lo[3], const float hi[3])
{
    orbitFromUsdCamera();
    float radius = 0.0f;
    for (int k = 0; k < 3; ++k)
    {
        m_target[k] = 0.5f * (lo[k] + hi[k]);
        radius += (hi[k] - lo[k]) * (hi[k] - lo[k]);
    }
    radius = std::max(0.5f * std::sqrt(radius), 1e-3f);
    m_distance = 1.15f * radius / std::sin(0.5f * m_fovY * float(M_PI) / 180.0f);
    m_needsRender = true;
}

void App::frameSelection()
{
    const FlatGeometry& g = m_scene.geometry();
    const MeshGeometry* mesh =
        m_scene.registry().valid(m_selected) ? m_scene.registry().try_get<MeshGeometry>(m_selected) : nullptr;
    if (!mesh)
    {
        frameBounds(g.boundsMin, g.boundsMax);
        return;
    }
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (unsigned t = mesh->firstTriangle; t < mesh->firstTriangle + mesh->triangleCount; ++t)
        for (int c = 0; c < 3; ++c)
        {
            const float* v = &g.vertices[3 * size_t(g.indices[3 * size_t(t) + c])];
            for (int k = 0; k < 3; ++k)
                lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
        }
    frameBounds(lo, hi);
}

void App::drawViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", nullptr, kPanelFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();

    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (!m_rendererOk || size.x < 4 || size.y < 4)
    {
        ImGui::TextWrapped("%s", m_rendererOk ? "" : "OptiX renderer unavailable.");
        ImGui::End();
        return;
    }

    ImGui::InvisibleButton("viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                 ImGuiButtonFlags_MouseButtonMiddle);
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();

    // camera controls: left = orbit, right/middle = pan, wheel = zoom; they
    // leave a USD camera for the orbit camera
    const bool dragging = active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) ||
                                     ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f) ||
                                     ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f));
    if (dragging || (hovered && io.MouseWheel != 0.0f))
        orbitFromUsdCamera();
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
    {
        m_yaw -= io.MouseDelta.x * 0.006f;
        m_pitch = std::clamp(m_pitch + io.MouseDelta.y * 0.006f, -1.55f, 1.55f);
        m_needsRender = true;
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)))
    {
        // move the target in the camera plane
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch), cy = std::cos(m_yaw), sy = std::sin(m_yaw);
        float right[3], up[3];
        if (m_scene.zUp())
            right[0] = -sy, right[1] = cy, right[2] = 0, up[0] = -sp * cy, up[1] = -sp * sy, up[2] = cp;
        else
            right[0] = cy, right[1] = 0, right[2] = -sy, up[0] = -sp * sy, up[1] = cp, up[2] = -sp * cy;
        const float k = m_distance * 2.0f * std::tan(0.5f * m_fovY * float(M_PI) / 180.0f) / size.y;
        for (int i = 0; i < 3; ++i)
            m_target[i] += (io.MouseDelta.y * up[i] - io.MouseDelta.x * right[i]) * k;
        m_needsRender = true;
    }
    if (hovered && io.MouseWheel != 0.0f)
    {
        m_distance *= std::pow(0.88f, io.MouseWheel);
        m_needsRender = true;
    }

    const float fbScale = io.DisplayFramebufferScale.x;
    const unsigned w = unsigned(size.x * fbScale), h = unsigned(size.y * fbScale);
    const bool mirroring = m_xrMirror && m_xr.state() == XrOutput::State::Running && !m_mirrorPixels.empty();

    // pick on click (left release without dragging)
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 16.0f &&
        m_renderer.width() == w && !mirroring)
    {
        const unsigned px = unsigned((io.MousePos.x - origin.x) * fbScale);
        const unsigned py = unsigned((io.MousePos.y - origin.y) * fbScale);
        const unsigned meshId = m_renderer.meshIdAt(px, py);
        const FlatGeometry& g = m_scene.geometry();
        select(meshId < g.meshEntity.size() ? g.meshEntity[meshId] : entt::null, true);
    }

    if (w != m_renderer.width() || h != m_renderer.height())
    {
        m_renderer.resize(w, h);
        if (m_texture)
            SDL_DestroyTexture(m_texture);
        m_texture = SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, int(w), int(h));
        m_needsRender = true;
    }
    // any change restarts the progressive accumulation; otherwise keep adding
    // samples until kMaxSamples (glossy reflections and antialiasing converge)
    if (m_needsRender)
    {
        updateCamera();
        m_renderer.resetAccumulation();
        m_needsRender = false;
    }
    if (m_texture && m_rendererOk && m_renderer.samples() < kMaxSamples && !mirroring)
    {
        try
        {
            m_renderer.render();
            SDL_UpdateTexture(m_texture, nullptr, m_renderer.pixels().data(), int(w * 4));
        }
        catch (const std::exception& e)
        {
            m_error = std::string("Rendering failed: ") + e.what();
            m_rendererOk = false;
        }
    }
    if (mirroring)
        drawXrMirror(origin.x, origin.y, size.x, size.y);
    else if (m_texture)
        ImGui::GetWindowDrawList()->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(m_texture))), origin,
                                             ImVec2(origin.x + size.x, origin.y + size.y));
    if (m_scene.loaded() && m_scene.geometry().empty())
        ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 12, origin.y + 12), IM_COL32(220, 220, 220, 255),
                                            "This stage has no renderable geometry (Mesh, Cube, Sphere).");
    ImGui::End();
}

void App::drawStatusBar()
{
    ImGui::Begin("Status", nullptr, kPanelFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    const FlatGeometry& g = m_scene.geometry();
    ImGui::Text("%s  |  prims: %zu  meshes: %zu  triangles: %zu  textures: %zu  |  camera: %s  |  "
                "OptiX %d.%d.%d on %s  |  samples %u/%u, %.2f ms%s",
                m_scene.loaded() ? m_scene.filePath().c_str() : "no file",
                m_scene.loaded() ? m_scene.registry().storage<PrimInfo>().size() - 1 : 0, g.meshEntity.size(),
                g.indices.size() / 3, g.textures.size(), m_camera >= 0 ? m_scene.cameras()[m_camera].path.c_str() : "orbit",
                OPTIX_VERSION / 10000, (OPTIX_VERSION / 100) % 100, OPTIX_VERSION % 100,
                m_renderer.deviceName().c_str(), m_renderer.samples(), kMaxSamples, m_renderer.lastRenderMs(),
                m_xr.state() == XrOutput::State::Off ? "" : ("  |  XR: " + xrStatus()).c_str());
    ImGui::End();
}

void App::drawPopups()
{
    if (!m_error.empty() && !ImGui::IsPopupOpen("Error"))
        ImGui::OpenPopup("Error");
    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(m_error.c_str());
        if (ImGui::Button("OK"))
        {
            m_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_showAbout)
    {
        ImGui::OpenPopup("About");
        m_showAbout = false;
    }
    if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("USD OptiX Viewer");
        ImGui::Separator();
        ImGui::Text("OpenUSD scene loading, EnTT scene graph, OptiX %d ray tracing,", OPTIX_VERSION / 10000);
        ImGui::Text("SDL3 windowing and Dear ImGui %s user interface.", IMGUI_VERSION);
        ImGui::Spacing();
        ImGui::TextDisabled("Left drag: orbit   Right/middle drag: pan   Wheel: zoom");
        ImGui::TextDisabled("Click: select   F: frame selection   Esc: clear selection");
        ImGui::TextDisabled("Camera menu: look through a USD camera (moving returns to the orbit camera)");
        ImGui::TextDisabled("XR menu: headset output through OpenXR / NVIDIA CloudXR (--xr, --cloudxr)");
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---- XR output ----------------------------------------------------------------
// --cloudxr runs the CloudXR runtime in this process; --xr alone uses the
// active OpenXR runtime (XR_RUNTIME_JSON), for instance a CloudXR runtime
// started by Isaac Teleop.
void App::startXr(bool cloudXr, std::string sdkDir, const std::string& profile)
{
    std::string error;
    if (cloudXr)
    {
        if (sdkDir.empty() && std::getenv("CLOUDXR_SDK_DIR"))
            sdkDir = std::getenv("CLOUDXR_SDK_DIR");
        if (sdkDir.empty())
        {
            m_error = "--cloudxr needs the CloudXR SDK directory: --cloudxr-sdk DIR or CLOUDXR_SDK_DIR.";
            return;
        }
        if (!m_cloudXr.start(sdkDir, profile, error))
        {
            m_error = "CloudXR: " + error;
            return;
        }
    }
    if (!m_xr.init(error))
    {
        m_error = "XR output unavailable: " + error;
        return;
    }
    m_xrEnabled = true;
}

// Called once per main-loop turn. When the headset appears, the renderer
// moves to the GPU the runtime chose, so that the eye buffers can be shared.
// While the session runs, xrWaitFrame paces the loop at the headset's rate,
// so the desktop stops waiting for vsync.
void App::updateXr()
{
    if (!m_xrEnabled)
        return;
    m_xrClient = m_cloudXr.clientConnected();
    std::string error;
    if (m_xr.update(error) == XrOutput::State::SystemReady && useRendererOn(m_xr.deviceUuid(), error))
        m_xr.createSession(m_xrResolution, error);
    if (m_xr.beginFrame(error))
    {
        const auto start = std::chrono::steady_clock::now();
        if (m_xr.shouldRender() && m_rendererOk)
            renderXrEyes(error);
        m_xr.endFrame(error);
        m_xrFrameMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (!error.empty())
    {
        m_error = "XR output stopped: " + error;
        m_xr.stop();
        m_xrEnabled = false;
    }
    const bool paced = m_xr.state() == XrOutput::State::Running;
    if (paced != m_xrPaced)
    {
        SDL_SetRenderVSync(m_sdlRenderer, paced ? 0 : 1);
        m_xrPaced = paced;
    }
}

// The XR origin sits at the desktop view's eye, facing its horizontal
// direction, so the headset starts where the desktop camera is and follows it.
void App::renderXrEyes(std::string& error)
{
    XrOutput::Anchor anchor{};
    viewFrame(anchor.eye, anchor.forward);
    anchor.zUp = m_scene.zUp();
    const double metersPerUnit = m_scene.loaded() ? UsdGeomGetStageMetersPerUnit(m_scene.stage()) : 1.0;
    anchor.unitsPerMeter = m_xrWorldScale / float(metersPerUnit);
    try
    {
        for (unsigned int i = 0; i < m_xr.eyeCount(); ++i)
            m_renderer.renderView(m_xr.eyeCamera(i, anchor), m_xr.eyeWidth(), m_xr.eyeHeight(), m_xr.eyeBuffer(i),
                                  unsigned(m_xrSamples));
        if (m_xrMirror)
        {
            m_mirrorPixels.resize(size_t(m_xr.eyeWidth()) * m_xr.eyeHeight());
            if (cudaMemcpy(m_mirrorPixels.data(), m_xr.eyeBuffer(0), m_mirrorPixels.size() * sizeof(uchar4),
                           cudaMemcpyDeviceToHost) != cudaSuccess)
                throw std::runtime_error("copying the mirror image failed");
        }
    }
    catch (const std::exception& e)
    {
        error = std::string("rendering failed: ") + e.what();
    }
}

// Moves the renderer to the GPU with the given UUID and uploads the scene again.
bool App::useRendererOn(const std::array<unsigned char, 16>& uuid, std::string& error)
{
    if (m_rendererOk && m_renderer.deviceUuid() == uuid)
        return true;
    m_renderer.release();
    m_rendererOk = m_renderer.init(error, uuid.data());
    if (!m_rendererOk)
        return false;
    if (m_scene.loaded())
        m_renderer.setGeometry(m_scene.geometry());
    entt::registry& reg = m_scene.registry();
    const MeshGeometry* mesh = reg.valid(m_selected) ? reg.try_get<MeshGeometry>(m_selected) : nullptr;
    m_renderer.setSelectedMesh(mesh ? mesh->meshId : kNoMesh);
    m_needsRender = true;  // the viewport also resizes the new image
    return true;
}

// The left eye's latest image, fitted inside the viewport.
void App::drawXrMirror(float x, float y, float width, float height)
{
    const int w = int(m_xr.eyeWidth()), h = int(m_xr.eyeHeight());
    if (!m_mirrorTexture || w != m_mirrorWidth || h != m_mirrorHeight)
    {
        if (m_mirrorTexture)
            SDL_DestroyTexture(m_mirrorTexture);
        m_mirrorTexture = SDL_CreateTexture(m_sdlRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        m_mirrorWidth = w;
        m_mirrorHeight = h;
    }
    if (!m_mirrorTexture || m_mirrorPixels.size() != size_t(w) * h)
        return;
    SDL_UpdateTexture(m_mirrorTexture, nullptr, m_mirrorPixels.data(), w * 4);
    const float scale = std::min(width / float(w), height / float(h));
    const ImVec2 lo(x + 0.5f * (width - w * scale), y + 0.5f * (height - h * scale));
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), IM_COL32(0, 0, 0, 255));
    list->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(m_mirrorTexture))), lo,
                   ImVec2(lo.x + w * scale, lo.y + h * scale));
}

std::string App::xrStatus() const
{
    switch (m_xr.state())
    {
    case XrOutput::State::Off:
        return "off";
    case XrOutput::State::WaitingForSystem:
        return m_cloudXr.running() ? "waiting for a CloudXR client" : "waiting for a headset";
    case XrOutput::State::SystemReady:
    case XrOutput::State::Idle:
        return "headset idle";
    case XrOutput::State::Running:
        break;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "running, %.1f ms per frame", m_xrFrameMs);
    return text;
}
