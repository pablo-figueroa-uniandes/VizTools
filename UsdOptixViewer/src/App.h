// Application: SDL3 window, Dear ImGui panels (hierarchy, inspector,
// viewport) and the OptiX renderer.
#pragma once

#include <array>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>

#include "CloudXrService.h"
#include "OptixRenderer.h"
#include "Scene.h"
#include "XrOutput.h"

class App
{
public:
    ~App();
    bool init(int argc, char** argv);
    void run();

    // Called from the SDL file dialog callback (possibly on another thread).
    void requestOpen(const std::string& path);

private:
    void loadFile(const std::string& path);
    void processEvent(const SDL_Event& event);
    void drawUi();
    void drawMenuBar();
    void drawHierarchy();
    void drawHierarchyNode(const pxr::UsdPrim& prim);
    void drawInspector();
    void drawViewport();
    void drawStatusBar();
    void drawPopups();

    void select(const pxr::SdfPath& path, bool reveal);
    void frameBounds(const float lo[3], const float hi[3]);
    void frameSelection();
    void updateFilter();
    void updateCamera();
    void orbitOffset(float offset[3], float up[3]) const;
    void viewFrame(float eye[3], float forward[3]) const;
    void lookThrough(int camera);
    void orbitFromUsdCamera();
    void orbitAround(const float target[3], const float offset[3], float distance);
    void pivotAt(unsigned int x, unsigned int y);
    void fittedWindow(const SceneCamera& camera, float& halfWidth, float& halfHeight) const;
    void openFileDialog();

    void startXr(bool cloudXr, std::string sdkDir, const std::string& profile);
    void updateXr();
    void renderXrEyes(std::string& error);
    bool useRendererOn(const std::array<unsigned char, 16>& uuid, std::string& error);
    void drawXrMirror(float x, float y, float width, float height);
    std::string xrStatus() const;

    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_sdlRenderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    bool m_running = true;

    Scene m_scene;
    OptixRenderer m_renderer;
    bool m_rendererOk = false;
    bool m_needsRender = true;

    // orbit camera
    float m_target[3] = {0, 0, 0};
    float m_distance = 5.0f, m_yaw = 0.6f, m_pitch = 0.4f, m_fovY = 45.0f;
    int m_camera = -1;  // USD camera we look through (index into Scene::cameras()), -1 for the orbit camera
    float m_light[3] = {0.4f, 0.8f, 0.45f};
    bool m_shadows = true;
    bool m_mipmaps = true;
    bool m_normalMaps = true;
    bool m_reflections = true;

    using PathSet = std::unordered_set<pxr::SdfPath, pxr::SdfPath::Hash>;
    pxr::SdfPath m_selected;  // empty: nothing selected
    bool m_revealSelection = false;
    PathSet m_reveal;  // ancestors to open in the tree
    char m_filter[128] = "";
    std::string m_appliedFilter;
    PathSet m_visibleByFilter;

    char m_pathInput[1024] = "";
    std::string m_error;
    bool m_showAbout = false;

    std::mutex m_pendingMutex;
    std::string m_pendingOpen;

    // XR output. The CloudXR service (if it runs in this process) is declared
    // first, so that the XR session ends before the service stops.
    CloudXrService m_cloudXr;
    XrOutput m_xr;
    bool m_xrEnabled = false;
    bool m_xrPaced = false;         // xrWaitFrame paces the loop instead of vsync
    bool m_xrClient = false;        // a CloudXR client is connected
    float m_xrResolution = 1.0f;    // fraction of the headset's recommended eye resolution
    float m_xrWorldScale = 1.0f;    // > 1 makes the user larger than the scene's units say
    int m_xrSamples = 1;            // samples per pixel and frame
    float m_xrFrameMs = 0.0f;
    bool m_xrMirror = false;        // show the left eye in the viewport
    SDL_Texture* m_mirrorTexture = nullptr;
    int m_mirrorWidth = 0, m_mirrorHeight = 0;
    std::vector<uchar4> m_mirrorPixels;
};
