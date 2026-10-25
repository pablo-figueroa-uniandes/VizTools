// Application: SDL3 window, Dear ImGui panels (hierarchy, inspector,
// viewport) and the OptiX renderer.
#pragma once

#include <mutex>
#include <string>
#include <unordered_set>

#include <SDL3/SDL.h>

#include "OptixRenderer.h"
#include "Scene.h"

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
    void drawHierarchyNode(entt::entity e);
    void drawInspector();
    void drawViewport();
    void drawStatusBar();
    void drawPopups();

    void select(entt::entity e, bool reveal);
    void frameBounds(const float lo[3], const float hi[3]);
    void frameSelection();
    void updateFilter();
    void updateCamera();
    void openFileDialog();

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
    float m_light[3] = {0.4f, 0.8f, 0.45f};
    bool m_shadows = true;
    bool m_mipmaps = true;
    bool m_normalMaps = true;
    bool m_reflections = true;

    entt::entity m_selected = entt::null;
    bool m_revealSelection = false;
    std::unordered_set<entt::entity> m_reveal;  // ancestors to open in the tree
    char m_filter[128] = "";
    std::string m_appliedFilter;
    std::unordered_set<entt::entity> m_visibleByFilter;

    char m_pathInput[1024] = "";
    std::string m_error;
    bool m_showAbout = false;

    std::mutex m_pendingMutex;
    std::string m_pendingOpen;
};
