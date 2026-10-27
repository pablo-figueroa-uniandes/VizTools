# New Visualization Tools in ginebra

This file describes the advances in visualization tools for this computer.

## October 26th

I read 13 lines of the literate output, and leave notes in the document. I ask claude to answer to the following prompt:

create a document in Docs with the answers to the notes in LiterateP/UsdOptixViewer-literate.pdf. Enumerate the answers

After reading the document, I will:
- remove the use of ENTT, and leave the scene graph at the USD to handle all the work
- carefully install for everybody that is possible (CUDA and Vulkan), and remove the local versions and their mentions in the cmake
- keep the imgui just as source code, due the explanation
- check the new version for ptx... and maybe use it instead of ptx.

However, try first the XRCloud server

## October 25th

1. Add a way to change the viewpoint to one of the cameras in the USD file
2. create a new branch and add support for CloudXR. Create a document that explains the differences between the main branch and this one

### Results (October 25th): task 1, USD cameras

**Done, on `main`.**
- Scene: every `UsdGeomCamera` becomes a `SceneCamera`, evaluated at the stage's start time through `GfCamera`/`GfFrustum`. It stores the world-space eye, forward and up, the window, the projection and the clipping range. The full transform is used, with scale dropped as USD requires. The prim also gets a `CameraView` component. Cameras keep stage order.
- Renderer: the camera can now be orthographic, and camera rays honour the clipping range as a depth along the view axis. The new `setCameraView` takes these settings, and `setCamera` (the orbit camera) is built on it. The ray differentials used for mipmapping handle orthographic cameras: the neighbouring rays get shifted origins instead of new directions.
- UI:
  - A new *Camera* menu lists the orbit camera and every USD camera.
  - Camera prims have a *Look through* button and a summary (projection, field of view or view size, clipping range) in the inspector.
  - `--camera /Path` starts with a USD camera.
  - The status bar names the active camera.
  - The film back is fitted inside the viewport, like usdview's "fit" policy.
  - While a USD camera is active, the field-of-view slider is disabled.
- Moving the view (orbit, pan, zoom, frame) hands it to the orbit camera, which starts at the USD camera's position, direction and field of view, so the image doesn't jump. Roll is lost. An orthographic camera becomes a perspective one that shows the same size at the orbit target.
- New test scene `assets/camera_test.usda`. It references `sample_scene.usda` and adds Front, Telephoto, an orthographic Top, Clipped (its near plane cuts the scene) and Rig/Side (under a rotated and scaled parent). usdchecker passes.
- Verified:
  - All five cameras show the expected views: object order, the 12-unit ground inside the 14-unit orthographic view, the clipped half and the scale-independent field of view.
  - The hand-off to the orbit camera gives an identical image for perspective cameras. A temporary debug build was used for this, because mouse input can't be scripted here.
  - The texture, mipmap, normal-map and material scenes are pixel-identical (max difference 1/255) to the previous build.
  - Mipmapping still filters under an orthographic camera.
  - The build has no warnings, and the installed copy was updated.
- Literate book: updated in every part that changed (shared, scene, renderer, device, app). It now has 307 sections, 300 chunks and 108 pages. `lp.py check` is ok for all 13 files.
- Not supported: aperture offsets (ignored, with a warning), depth of field and animated cameras (only the start time is used).

### Results (October 25th): task 2, CloudXR

**Done, on branch `cloudxr`** (not merged into `main`). `CLOUDXR.md` at the repository root explains every difference from `main`, how to run it, what was verified and what is missing.
- What was already on this machine:
  - The NVIDIA CloudXR Runtime, which is an OpenXR runtime based on Monado. Isaac Teleop runs it for Vision Pro (`~/.cloudxr`, `/srv/isaac-shared/cxr`).
  - The CloudXR 6.2.1 SDK, downloaded by the LÖVR sample in `/srv/isaac-shared/cxr/cloudxr-lovr-sample`.
  - The runtime only accepts Vulkan apps (`XR_KHR_vulkan_enable2`).
- Design:
  - `XrOutput` is a plain OpenXR client. The runtime creates the Vulkan device and picks the GPU.
  - Each eye is a Vulkan buffer exported to CUDA, so OptiX renders straight into it. A queue copy then puts it into the swapchain image.
  - The renderer moves to the runtime's GPU (matched by UUID) if needed.
  - `CloudXrService` can run the CloudXR runtime inside the viewer, from an SDK directory, as in NVIDIA's LÖVR sample.
  - The headset is anchored at the desktop camera's eye and follows it.
- Usage:
  - `--cloudxr --cloudxr-sdk DIR [--cloudxr-profile apple-vision-pro|auto-webrtc|...]` runs the runtime inside the viewer.
  - `--xr` uses the active runtime (`XR_RUNTIME_JSON`, for example after `source ~/.cloudxr/run/cloudxr.env`).
  - `--xr-samples`, `--xr-resolution` and `--xr-mirror` set the headset view, along with a new *XR* menu.
- Dependencies: the OpenXR loader 1.1.58 (static) and the Vulkan headers 1.3.275 are fetched by CMake. Vulkan is dlopened. The CloudXR SDK is loaded at run time and is not in the repository. `/opt/viztools` is unchanged.
- Verified without a headset:
  - With the static `apple-vision-pro` profile, the session runs: 2 × 2400×1920 eyes, 63 mm apart, 2.8 ms per frame at 1 sample per pixel.
  - The left-eye mirror shows the correct view.
  - `--xr` works against a runtime hosted by another process, with textures, at 4.2 ms per frame and 4 samples per pixel.
  - Shutdown is clean.
  - Errors leave the desktop working.
  - Desktop rendering is unchanged.
  - The build has no warnings, from scratch.
  - A first test showed that the CloudXR service takes over SIGTERM and SIGINT, which left the app running without its runtime. This was fixed by restoring SDL's handlers after the service starts.
- Not verified: a real headset (image, tracking, `auto-*` client connect and disconnect) and an actual GPU switch.
- Literate book (branch): new part `75-xr.nw` for `XrOutput` and `CloudXrService`, written by a sub-agent. Intro, build, shared, renderer and app parts are updated. It covers 17 files, with 396 sections, 388 chunks and 143 pages. `lp.py check` is ok.
- The installed copy in `~/.local/opt/UsdOptixViewer` is still `main`'s build (USD cameras). The branch was only built in its build tree.

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

### Follow-up (October 24th): literate program

- `LiterateP/` holds the whole program as a literate program in Knuth's style: `UsdOptixViewer-literate.pdf`, 102 pages, 293 sections and 286 chunks.
- The book covers all 13 source files: the CMake files, every C++/CUDA file and the install script. The third-party stb_image.h is not covered.
- Format:
  - The sources are written in noweb syntax, one file per part (`src/*.nw`).
  - `lp.py` tangles them back into the sources, and `lp.py check` compares the result byte for byte with the repository files.
  - `lp.py weave` produces CWEB-style LaTeX: numbered sections, ⟨chunk n⟩ ≡ definitions with "used in" cross-references, bold keywords, italic identifiers, the ≠ ≤ ∧ → operators, and an index of chunk names.
  - The PDF is typeset with Tectonic, installed in `~/.local/bin` (no TeX Live was available).
- Status: `lp.py check` reports ok for all 13 files. The PDF builds with no TeX errors and no overfull lines.
- The intro, build and install parts were written directly. The scene, renderer, device and application parts were written in parallel by four sub-agents, each verifying its own files with `check`, then merged and checked as a whole.

### Follow-up (October 25th): slide presentation

- `Docs/UsdOptixViewer-code.pptx` walks through the code in 24 slides: architecture, scene loading, OptiX, the application, USD cameras and the XR output. It has speaker notes and screenshots of the running viewer (`Docs/images/`), including the CloudXR left-eye mirror.
- `Docs/make_slides.js` regenerates it with pptxgenjs (`npm install pptxgenjs@3.12.0`). It lives on the `cloudxr` branch, because it covers the XR code.

### Follow-up (October 26th): answers to the PDF notes

- `Docs/LiterateNotes-Answers-261026.md` answers, in numbered order, the 13 notes added to `LiterateP/UsdOptixViewer-literate.pdf` (extracted from its PDF annotations).
- Notes 1, 2, 3, 4, 7 and 8 ask for changes (delete the install script and `~/.local/opt`, install Vulkan/OpenXR/CloudXR for all users, prebuild ImGui, move the alignment prose). The document describes each change but none has been made yet.
- Checked while writing: `sizeof(MaterialData)` is 192 bytes with no padding, and the GPUs are `sm_89` (relevant to the `compute_75` question).

### Follow-up (October 27th): CloudXR connection address in the XR menu

- With `--cloudxr`, the XR menu now has a "Connect a client to" section: the IPv4 address of every network interface that is up (except loopback), with its name, plus the service's port. This is what the Vision Pro client needs.
- The port is read from the service's `server-port` property (`nv_cxr_service_get_int64_property`) instead of being hard-coded, because it depends on the profile (48010 for `apple-vision-pro`, 49100 for WebRTC) and on `NV_CXR_SERVER_PORT`. If the read fails, the menu says the port is unknown.
- The addresses are collected once, when the service starts (`CloudXrService::findAddresses`, using `getifaddrs`).
- Verified: with `--cloudxr-profile apple-vision-pro`, the service reports 48010, and `ss` shows the app listening on TCP 0.0.0.0:48010. The addresses match `ip -4 addr` (enp66s0f0 157.253.192.148, wlp195s0 157.253.113.156, tailscale0 100.116.114.43). The build has no warnings, `lp.py check` is ok for all files, the PDF was rebuilt, CLOUDXR.md was updated, and the installed copy was updated. The menu itself wasn't screenshotted, because synthetic input can't open it.

### Follow-up (October 27th): first run with an Apple Vision Pro

- The viewer streams to the Vision Pro. The steps are in CLOUDXR.md under "Connecting an Apple Vision Pro".
- Two things stopped the first attempts:
  - My launch was killed by the command runner's 2-minute timeout, so the service was gone. Long runs must be started with `run_in_background`.
  - The headset client connects over TLS to 48322 only. The working Isaac Teleop setup has stunnel there, forwarding to the runtime on 127.0.0.1:48324, and ufw allows 48322/tcp and 47998-48012/udp from 157.253.113.0/24 only. Without stunnel the headset shows `0x800B1004`. The fix: the user runs the ufw + stunnel steps as root, and the viewer starts with `NV_CXR_SERVER_PORT=48324`.
- Result: two sessions (3 and 4 minutes) with all streams connected, ended from the headset. The user confirmed that it runs.
- Open: the XR menu shows the runtime's port (48324), not stunnel's 48322. A `--cloudxr-port` option and a menu hint for the TLS proxy would help.
