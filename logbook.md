# New Visualization Tools in Ginebra

This file describes the advances in visualization tools for this computer.

## October 24th

1. Locally to this account, install compatible versions of the following development tools (if they are not installed)
1.1. NVidia´s optix sdk: https://developer.nvidia.com/designworks/optix/download
1.2. CMake: https://cmake.org/download/
1.3. a c++ compiler
1.4. sdl3: https://www.libsdl.org/
1.5. ENTT: https://github.com/skypjack/entt
1.6. IMGUI: https://github.com/ocornut/imgui

2. Create a sample app that uses all these tools, with the following functionality:
2.1 Load USD files
2.2 Inspect the contents of the USD file, with a hierarchical view
2.3 Show a 3D view of the file, rendered with Optix
2.4 a cmake based installation for this program

3. If everything goes right, install the tools in step 1 for all users

### Results (October 24th)

Machine: Ubuntu 24.04, 2x NVIDIA RTX 6000 Ada, driver 595.99.02 (CUDA 13.2), X11 session. No sudo was used.

**Task 1: done.** Everything is installed under `~/.local/opt`. Sources and build trees are in `~/.local/opt/src`.

| Tool | Version | Location | Notes |
|------|---------|----------|-------|
| OptiX SDK | 9.1.0 | `~/.local/opt/optix-9.1.0` | Official headers from github.com/NVIDIA/optix-dev, the same headers as the SDK download (which needs a developer login). The OptiX runtime ships with the driver (R590+ required, 595 installed). SDK samples are not included. |
| CUDA toolkit | 13.2.1 | `~/.local/opt/cuda-13.2` | Needed to compile OptiX programs (nvcc). Toolkit-only runfile install; the driver was not touched. |
| CMake | 3.28.3 | `/usr/bin/cmake` | Already installed system-wide. |
| C++ compiler | g++ 11.5.0 | `/usr/bin/g++` | Already installed system-wide (C++20 capable). |
| SDL3 | 3.4.18 | `~/.local/opt/sdl3` | X11, OpenGL and Vulkan enabled. Wayland is disabled because its dev headers are not installed. |
| EnTT | 4.0.0 | `~/.local/opt/entt-4.0.0` | Header only, with a CMake package (`EnTT::EnTT`). Requires C++20. |
| Dear ImGui | 1.93.0 | `~/.local/opt/imgui-1.93.0` | Sources plus the SDL3 / SDL_Renderer / OpenGL3 backends (ImGui is compiled into each app). |
| OpenUSD (extra) | 26.11.1 | `~/.local/opt/usd-26.11` | Needed for task 2. Built with oneTBB, without Python, imaging or MaterialX. Includes the usdcat, usdtree and usdchecker tools. |

To set up the environment (PATH, LD_LIBRARY_PATH, CMAKE_PREFIX_PATH, OptiX_INSTALL_DIR, IMGUI_DIR), run `source ~/.local/opt/viztools-env.sh`. It is not added to `~/.bashrc` yet; add it if you want it in every shell.

**Task 2: done.** The sample app is `UsdOptixViewer/` (see its README.md).
- 2.1: Loads USD files from the command line, File > Open (SDL3 dialog), a typed path, or drag and drop.
- 2.2: Hierarchy panel built from an EnTT registry (one entity per prim), with a name filter. The inspector shows prim metadata and every attribute and relationship value.
- 2.3: OptiX 9.1 viewport: Mesh, Cube and Sphere prims with material or displayColor colors, a directional light, hard shadows, orbit/pan/zoom, click-to-select and selection highlight. The sample scene renders in about 0.5 ms per frame.
- 2.4: CMake build and `cmake --install`. Installed to `~/.local/opt/UsdOptixViewer`. The binary carries an RPATH to its dependencies and was verified to run in a clean environment.
- Not verified automatically: viewport mouse interaction (orbit and click-to-pick), because synthetic X events do not reach SDL on this display. Selection, highlight, hierarchy reveal and the inspector were verified with `--select`.

**Task 3: done.** The user ran `sudo bash install_all_users.sh`. It reused the step-1 builds and installed:

| Tool | All-users location |
|------|--------------------|
| CUDA toolkit 13.2.1 | `/usr/local/cuda-13.2` (+ `/usr/local/cuda` symlink) |
| OptiX 9.1.0 headers | `/opt/viztools/optix-9.1.0` |
| SDL3 3.4.18 | `/opt/viztools/sdl3` |
| EnTT 4.0.0 | `/opt/viztools/entt-4.0.0` |
| Dear ImGui 1.93.0 | `/opt/viztools/imgui-1.93.0` |
| OpenUSD 26.11.1 | `/opt/viztools/usd-26.11` (cmake/pkg-config files rewritten to point to /opt) |
| CMake, g++ | already system-wide (`/usr/bin`) |

- `/etc/profile.d/viztools.sh` sets CUDA_HOME, OptiX_INSTALL_DIR, IMGUI_DIR, PATH and CMAKE_PREFIX_PATH for every user at login.
- `/etc/ld.so.conf.d/viztools.conf` adds the CUDA, SDL3 and USD library directories to the dynamic loader.
- Verification: UsdOptixViewer was rebuilt from scratch in an environment containing only `/etc/profile.d/viztools.sh`. Every dependency resolved to `/opt/viztools` or `/usr/local/cuda-13.2`, and the build had no warnings. It is installed in `~/.local/opt/UsdOptixViewer`, with an RPATH into `/opt/viztools`, so it no longer depends on the per-user tools. It ran correctly with a clean environment.
- The CMakeLists now defaults to `/opt/viztools`, falling back to `~/.local/opt` when that does not exist.
- Cleanup: the downloads and build trees (`~/.local/opt/src`, about 5 GB) were deleted, so `install_all_users.sh` can no longer be re-run as is.
- Still present: the per-user copies in `~/.local/opt` (cuda-13.2, sdl3, entt-4.0.0, imgui-1.93.0, optix-9.1.0, usd-26.11, viztools-env.sh). Nothing uses them any more, so they can be deleted.


### Follow-up (October 24th): texture support in UsdOptixViewer

A check showed that textures were not displayed at all: each object got one solid colour. Texture support has now been added:
- `UsdUVTexture` on `diffuseColor`, with scale/bias, wrap modes, sRGB/raw and fallback. Images are decoded with stb_image, including textures inside `.usdz`, and sampled as CUDA textures (hardware sRGB to linear).
- Texture coordinates from the primvar reader's `varname`, with any interpolation, indexed or not.
- Per-face materials via `GeomSubset`.
- Alpha cutouts (`opacity` + `opacityThreshold`) through an OptiX any-hit program, which also cuts out shadows.
- Inspector: shows the material, texture path, size and colour space, and lists warnings for unsupported features.
- `--select` now also frames the selected prim.

Verified:
- `assets/textured_scene.usda` (new, passes usdchecker): orientation correct on all box faces, 2x2 repeat, scale/bias tint, GeomSubset materials, file path through a material input.
- `McUsd.usdz` from github.com/usd-wg/assets: all 23 textures load from the usdz, the fern cutout and its shadow work.
- The solid-colour sample scene is unchanged.
- The installed binary was tested.

Not supported yet: translucency, UsdTransform2d, UDIM, normal/roughness/metallic maps.

### Follow-up (October 24th): texture mipmaps

- Each texture gets a full mip chain, built on the CPU with a 2x2 box filter (sRGB colours averaged in linear space). It is uploaded as a CUDA mipmapped array with trilinear and 8x anisotropic filtering.
- The mip level comes from ray differentials: in `sampleTexture`, the rays through the neighbouring pixels are intersected with the hit triangle's plane, converted to st gradients, and passed to `tex2DGrad`. Shadow rays (cutout any-hit) use the finest level.
- Toggle: View > Texture mipmaps, or `--no-mipmaps`.
- New test scene `assets/mipmap_test.usda`: a 40x40 floor with the grid texture repeated 20x20.
- Verified:
  - At a grazing angle, the far floor shows strong moire without mipmaps and fades smoothly with them.
  - Close-ups are unchanged (mean pixel difference 0.26/255, so no over-blurring).
  - McUsd.usdz still renders correctly, including the fern cutout; distant grass is now smooth instead of noisy.
  - The build has no warnings, and the installed copy was updated.

### Follow-up (October 24th): normal maps

- UsdPreviewSurface `normal` driven by a UsdUVTexture, decoded as texture * scale + bias exactly as authored. This also covers DirectX-style maps, as in McUsd, which use scale (2,-2,2) and bias (-1,1,-1). When neither scale nor bias is authored, the usual (2,-1) remap is assumed and a warning is shown. Normal maps default to the raw colour space.
- Each triangle gets a tangent frame from its positions and st (mirrored st handled), orthonormalized around the geometric normal. Left-handed meshes are rewound so that normal faces outward. The shading normal is kept facing the viewer, and light only counts when it reaches both the geometric and the shading normal's side.
- Toggle: View > Normal maps, or `--no-normal-maps`. The inspector shows the normal map, its scale and its bias.
- New test scene `assets/normalmap_test.usda` with `textures/bumps_normal.png` (4x4 hemispheres, OpenGL convention). It passes usdchecker. It has three walls (plain, leftHanded, mirrored st) plus a textured floor.
- Verified:
  - Every bump is lit on its upper right on all three walls, matching the light direction, so the green channel, handedness and mirroring are all correct.
  - With `--no-normal-maps` the walls are flat.
  - McUsd.usdz shows its carved quartz relief and pillar grooves.
  - The fallback for unauthored scale/bias gives the warning and correct bumps.
  - The build has no warnings, and the installed copy was updated.

### Follow-up (October 24th): roughness and metallic

- UsdPreviewSurface specular:
  - `roughness`, `metallic` and `ior` as constants or any texture channel, with that channel's scale/bias, so packed maps work.
  - `useSpecularWorkflow` with `specularColor` (constant or texture).
  - The inspector shows these inputs.
- Shading:
  - Lambert + GGX (Smith G, Schlick Fresnel; F0 from metallic and ior, or from specularColor) for the directional light.
  - One glossy reflection bounce, sampled with GGX visible normals (Heitz 2018). Deeper bounces reflect the sky.
  - A firefly clamp (2.0) on reflected radiance.
- Progressive rendering:
  - Jittered samples accumulate up to 256 per pixel while the view is static (about 0.4 ms per sample), giving antialiasing for free.
  - Frame 0 stays at the pixel centres and provides the picking ids.
  - The status bar shows the sample count. View > Reflections or `--no-reflections` turns off the traced bounce.
- Smooth shading normals: authored normals (any interpolation), otherwise smooth normals for subdivision meshes (USD's default catmullClark, as in Hydra) and flat for `subdivisionScheme = "none"`. Spheres get analytic normals, and the generated sphere's winding was fixed to face outward.
- Pipeline: 6 payload values, maxTraceDepth 3, stack size from `optixPipelineSetStackSizeFromCallDepths`.
- New test scene `assets/material_test.usda` with `textures/roughness_checker.png`: a gold and red-plastic sphere chart (roughness 0.05 to 1.0), a specular-workflow sphere, and a floor with a roughness checker texture.
- All test meshes now set `subdivisionScheme = "none"`. Without it, the box shaded like a rounded blob, which is correct USD behaviour for catmullClark.
- Verified (all 5 test scenes pass usdchecker):
  - The chart behaves as expected: sharp to blurred reflections and highlights, the floor mirrors the spheres on the smooth squares only.
  - The first attempt showed glitter and fireflies on rough metals. These were fixed with VNDF sampling and the clamp.
  - McUsd's gold block renders as a reflective metal from its roughness/metallic textures.
  - The sample, texture, mipmap and normal-map scenes still look correct.
  - The build has no warnings, and the installed copy was updated.
