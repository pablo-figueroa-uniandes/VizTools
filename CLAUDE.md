# VizTools

Visualization tool setup for this machine (Ubuntu 24.04, 2x RTX 6000 Ada,
X11 session), plus the sample app `UsdOptixViewer/`. Tasks and their history
live in `logbook.md`. Read it before starting a new task, and add a dated
"Results" entry when you finish one.

## Installed tools (all users)

| Tool | Location |
|------|----------|
| CUDA 13.2.1 | `/usr/local/cuda-13.2` |
| OptiX 9.1.0 (headers only; the runtime comes with the driver) | `/opt/viztools/optix-9.1.0` |
| SDL3 3.4.18 (X11/OpenGL/Vulkan, no Wayland) | `/opt/viztools/sdl3` |
| EnTT 4.0.0 (requires C++20) | `/opt/viztools/entt-4.0.0` |
| Dear ImGui 1.93.0 (sources + SDL3/SDL_Renderer/OpenGL3 backends) | `/opt/viztools/imgui-1.93.0` |
| OpenUSD 26.11.1 (no Python, imaging, MaterialX or Hydra) | `/opt/viztools/usd-26.11` |
| CMake 3.28, g++ 11.5 | `/usr/bin` |

- Environment: `/etc/profile.d/viztools.sh` (login shells). In a fresh non-login
  shell, run `source /etc/profile.d/viztools.sh` first.
- Older per-user copies remain in `~/.local/opt`; nothing uses them. Don't
  build against them.
- `install_all_users.sh` copied from build trees that have since been deleted,
  so it can't be re-run as is.

## Permissions and boundaries

- No sudo for Claude. For root steps, write a script and have the user run
  `sudo bash <script>` in their own terminal. Never ask for the password in chat.
- Don't edit `~/.bashrc` or other shell startup files.
- Put downloads and test files in the session scratchpad, and delete them
  when done.

## UsdOptixViewer

Build, install and run:

```bash
cd UsdOptixViewer
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/UsdOptixViewer
cmake --build build -j
cmake --install build
./build/UsdOptixViewer assets/textured_scene.usda --select /World/Box
```

Architecture (`src/`):
- `Scene.*`: USD stage to EnTT registry (one entity per prim: `PrimInfo`,
  `Hierarchy`, `MeshGeometry`). It also flattens geometry to world space
  (`FlatGeometry`: per-triangle mesh id, material and corner UVs), resolves
  UsdPreviewSurface materials and loads textures through `ArResolver`, so
  `.usdz` works. `UsdGeomCamera` prims become `SceneCamera` records
  (world-space eye/forward/up, window, clipping) plus a `CameraView` component.
- `OptixRenderer.*`: OptiX context, pipeline, SBT and a single GAS. Textures
  are mipmapped CUDA texture objects (CPU-built mip chain, anisotropic
  filtering). It renders to a host RGBA buffer plus a per-pixel
  mesh-id buffer, which is used for picking.
- `devicePrograms.cu`: raygen (pinhole or orthographic camera, clipping range
  as a depth along w; progressive, jittered after frame 0, which is
  also the picking frame), two miss programs (radiance, shadow), closest-hit
  (Lambert + GGX for the directional light, shadow ray, one VNDF-sampled glossy
  reflection ray at depth 0, firefly clamp on its result) and any-hit (alpha
  cutout). Payload: rgb, mesh id, depth, rng seed; maxTraceDepth 3.
  `sampleTexture` picks the mip level from ray differentials (camera rays
  only; shadow rays use level 0). `normalMapped` builds a per-triangle
  tangent frame from positions and st. Triangles are counter-clockwise from
  outside (Scene.cpp rewinds left-handed meshes), so `cross(e1, e2)` is the
  outward normal. It is compiled to PTX by nvcc and embedded
  via `cmake/EmbedFile.cmake`.
- `XrOutput.*` (branch `cloudxr`): OpenXR client. The runtime creates the
  Vulkan instance/device (`XR_KHR_vulkan_enable2`); eye buffers are Vulkan
  memory exported as opaque fds and imported into CUDA, copied to the
  swapchain images on the Vulkan queue. Vulkan is dlopened (`VK_NO_PROTOTYPES`).
  `CloudXrService.*` dlopens `libcloudxr.so` from a CloudXR SDK and restores
  SDL's SIGINT/SIGTERM handlers after starting it. `OptixRenderer` can move to
  the runtime's GPU (`release()` + `init(error, uuid)`) and renders eyes with
  `renderView`. The differences from `main` are listed in `CLOUDXR.md`; keep
  it current.
- `LaunchParams.h`: shared by host and device. Keep both sides in sync when
  changing it.
- `App.*`: SDL3 window, ImGui panels (hierarchy, inspector, viewport,
  status bar), an orbit camera and a Camera menu to look through USD cameras
  (`m_camera`; any mouse move hands the view back to the orbit camera).
  Setting `m_needsRender` restarts the accumulation; the app then adds a sample every frame until kMaxSamples (256).

Conventions: C++20, 4-space indent, braces on their own line, `m_` members,
sparse comments that explain the why. Keep `-Wno-deprecated` for the USD headers,
and keep builds warning-free.

## Literate book (LiterateP/)

`LiterateP/src/*.nw` contains every line of the 17 project source files
(everything except stb_image.h and cxrServiceAPI.h) in noweb syntax, with Knuth-style prose.
**When you change any source file, update the matching chunk in the book**,
then run `python3 LiterateP/lp.py check`. It must print `ok` for all files.
Rebuild the PDF with `LiterateP/build.sh` (needs tectonic in `~/.local/bin`).
Chunk names are unique across the book. A chunk definition goes on its own
line after `@`. Never hard-code section numbers: cite chunks as `<<Name>>`.

## Testing

- Synthetic X input (xdotool) does not reach SDL on this display, so use
  `--select <primPath>` (which selects and frames) and `--camera <cameraPath>`
  to drive the UI from the command line.
- Screenshots: launch the app in the background, then
  `xwd -id $(xdotool search --name "USD OptiX Viewer" | head -1) -silent`.
  The XWD pixels are packed 24-bit BGR: convert with PIL using
  `Image.frombuffer('RGB', (w, h), data, 'raw', 'BGR', bytes_per_line, 1)`.
- Test scenes: `assets/sample_scene.usda` (solid colors),
  `assets/textured_scene.usda` (on every face, the "UP" arrow must point up) and
  `assets/mipmap_test.usda` (compare with and without `--no-mipmaps`),
  `assets/normalmap_test.usda` (every bump lit on its upper right),
  `assets/material_test.usda` (roughness/metallic chart),
  `assets/camera_test.usda` (USD cameras; use `--camera /World/Cameras/Front`).
  Wait about 9 s before taking screenshots so the 256 samples converge. Hard-surface test
  meshes need `subdivisionScheme = "none"`, or they shade smooth.
  Validate new scenes with `usdchecker`. `McUsd.usdz` from
  github.com/usd-wg/assets is a good real-world texture and cutout test.
- XR without a headset: `--cloudxr --cloudxr-sdk
  /srv/isaac-shared/cxr/cloudxr-lovr-sample/build/cloudxr/CloudXR-6.2.1-Linux-sdk
  --cloudxr-profile apple-vision-pro --xr-mirror` gives a running session (a
  static profile needs no client); the mirror and the status bar show the
  result. Stop it with SIGTERM so the session ends before the service. Only
  one app can hold a CloudXR session.
- Kill only the app's PID. `pkill -f` patterns can match and kill the shell
  running them.

## Known gaps

Translucency, UsdTransform2d, UDIM, emission/clearcoat/displacement/occlusion,
indirect diffuse (only an ambient fill), USD lights (one fixed directional
light), subdivision refinement (only smooth normals), curves/points/instancers other than instance proxies, and
time-sampled animation (only the start time code is shown), camera aperture
offsets and depth of field.
