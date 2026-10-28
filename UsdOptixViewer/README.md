# UsdOptixViewer

Sample application that combines the visualization tools installed for all
users in `/opt/viztools` (CUDA in `/usr/local/cuda-13.2`):

| Tool        | Version  | Used for                                              |
|-------------|----------|-------------------------------------------------------|
| OpenUSD     | 26.11    | loading `.usd/.usda/.usdc/.usdz` stages; the stage is the scene graph |
| meshoptimizer | 1.3    | welding corners into indexed meshes, spatial and vertex-fetch order |
| OptiX       | 9.1.0    | ray tracing the 3D view (one GAS per mesh + an IAS)   |
| CUDA        | 13.2     | compiling the OptiX programs to OptiX-IR, device memory |
| OpenXR / Vulkan / CloudXR | 1.1.58 / 1.3.275 / 6.2.1 | XR output (optional at run time) |
| SDL3        | 3.4.18   | window, input, file dialog, drag & drop               |
| Dear ImGui  | 1.93.0   | hierarchy, inspector and viewport panels              |
| CMake / g++ | 3.28 / 11.5 | build and installation                             |

## Features

- **Load USD files**: command line, *File > Open* (Ctrl+O), *File > Open path*,
  or drag and drop a file onto the window.
- **Hierarchy view** of every prim (instance proxies included), with a name
  filter. Prims that produced geometry are shown in blue.
- **Inspector**: path, type, specifier, kind, applied schemas, and every
  attribute/relationship with its type and value.
- **OptiX viewport**: `Mesh`, `Cube` and `Sphere` prims are flattened to world
  space and ray traced. Invisible prims and proxy/guide purposes are skipped.
  Y-up and Z-up stages are supported.
  - Progressive rendering: while nothing changes, the image keeps accumulating
    jittered samples, up to 256 per pixel (about 0.4 ms each on the RTX 6000
    Ada). This gives antialiasing and noise-free glossy reflections. Any change
    restarts it, and the status bar shows the sample count.
  - Shading: Lambert diffuse plus GGX specular for one directional light with
    hard shadows, an ambient/headlight fill, and one glossy reflection bounce
    (VNDF-sampled GGX). Deeper bounces see the sky gradient.
  - Shading normals: authored `normals`/`primvars:normals` (any interpolation),
    otherwise smooth normals for subdivision meshes (USD's default
    `catmullClark`, as in Hydra) and flat for `subdivisionScheme = "none"`.
    Spheres get analytic normals.
- **Materials and textures** (UsdPreviewSurface subset):
  - `diffuseColor` as a value, or driven by a `UsdUVTexture` (PNG, JPEG, TGA, BMP,
    HDR, ...) with `scale`/`bias`, `wrapS`/`wrapT`, sRGB/raw color space and
    `fallback`. Without a material, `primvars:displayColor` is used.
  - Texture coordinates come from the `UsdPrimvarReader_float2` `varname`
    (`st` by default), with vertex/varying/faceVarying/uniform interpolation,
    indexed or not.
  - Textures inside `.usdz` packages and file paths taken from material
    interface inputs both work.
  - Per-face materials through `GeomSubset` bindings.
  - Alpha cutouts: `opacity` from a texture channel with `opacityThreshold > 0`,
    implemented in an any-hit program, so shadows are cut out too.
  - Roughness and metallic: `roughness`, `metallic` and `ior` as constants or
    any texture channel (`outputs:r/g/b/a`, with that channel's scale/bias), so
    packed maps work. `useSpecularWorkflow = 1` with `specularColor` (constant
    or texture) is supported too. Toggle the traced reflections with
    *View > Reflections* or `--no-reflections` (the sky is still reflected).
  - Normal maps: `normal` driven by a `UsdUVTexture`, decoded as
    texture * scale + bias exactly as authored. This also handles DirectX-style
    maps flipped through scale/bias. If neither scale nor bias is authored, the
    usual (2, -1) remap is assumed with a warning. Each triangle gets a tangent
    frame from its positions and st (mirrored st handled), and
    `orientation = "leftHanded"` meshes are rewound so the frame faces outward.
    Toggle it with *View > Normal maps* or `--no-normal-maps`.
  - Mipmaps: each texture gets a full mip chain (box filter, averaged in linear
    space for sRGB) with trilinear and 8x anisotropic filtering. The level comes
    from ray differentials: the neighbouring pixels' camera rays are intersected
    with the hit triangle's plane and turned into st gradients for `tex2DGrad`.
    Toggle it with *View > Texture mipmaps* or `--no-mipmaps`.
  - Not supported, and listed as warnings in the inspector: translucency,
    `UsdTransform2d` and UDIMs. Emission, clearcoat, displacement, occlusion,
    indirect diffuse lighting, scene lights and subdivision refinement are not
    implemented.
- Click on the viewport to select the prim under the cursor. The selection is
  highlighted and revealed in the hierarchy.
- USD cameras: the *Camera* menu lists every `UsdGeomCamera` of the stage (and
  the orbit camera). The inspector of a camera prim has a *Look through*
  button, and `--camera /Path` starts with that camera. Perspective and
  orthographic projections, the focal length and aperture, the clipping range
  and the full transform (scale ignored, as USD requires) are honoured, at the
  stage's start time. The camera's film back is fitted inside the viewport
  (usdview's "fit" policy). Orbiting, panning or zooming switches to the
  orbit camera, which starts from the USD camera's position and direction.
  Aperture offsets are ignored with a warning.
- XR output: stereo rendering to a headset through any
  OpenXR runtime with Vulkan support, in particular NVIDIA CloudXR (Apple
  Vision Pro, Quest through CloudXR.js). `--cloudxr` runs the CloudXR runtime
  inside the viewer, from `--cloudxr-sdk DIR`, `$CLOUDXR_SDK_DIR` or
  `/opt/viztools/cloudxr-6.2.1`; `--xr` uses the active runtime
  (`XR_RUNTIME_JSON`, for example one started by Isaac Teleop). The eyes are
  rendered by OptiX into Vulkan memory shared with CUDA. The headset starts at
  the desktop view's eye and follows it. See the *XR* menu and
  [../CLOUDXR.md](../CLOUDXR.md) for all options and how to connect a headset.

Controls: left-drag to orbit, right/middle-drag to pan, wheel to zoom, `F` to
frame the selection, `Esc` to clear the selection, *Camera* menu to look
through a USD camera.

## Build and install

```bash
source /etc/profile.d/viztools.sh   # already loaded in login shells
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/UsdOptixViewer
cmake --build build -j
cmake --install build
```

The installed binary has an RPATH to its dependencies, so it runs without
setting `LD_LIBRARY_PATH`:

```bash
~/.local/opt/UsdOptixViewer/bin/UsdOptixViewer \
    ~/.local/opt/UsdOptixViewer/share/UsdOptixViewer/assets/sample_scene.usda \
    [--select /World/Props/BlueSphere] [--camera /World/Cameras/Front] [--no-mipmaps] [--no-normal-maps] \
    [--no-reflections] [--xr] [--cloudxr] [--cloudxr-sdk DIR] [--cloudxr-profile NAME] \
    [--xr-samples N] [--xr-resolution F] [--xr-mirror]
```

`--select` selects and frames a prim, `--camera` looks through a USD camera.
`assets/camera_test.usda` references the sample scene and adds five cameras:
*Front* (cube left, pyramid right), *Telephoto*, an orthographic *Top* (the
12-unit ground fits inside its 14-unit view), *Clipped* (the near plane cuts
the scene in half) and *Rig/Side* (under a rotated, scaled parent, at
(16, 3, 0) looking along -X). `assets/textured_scene.usda` is a
texture test scene: on every face the "UP" arrow must point up, with red at
the top-left of the image. `assets/mipmap_test.usda` is a large floor with a
repeated texture: with mipmaps its far end fades smoothly to gray, and with
`--no-mipmaps` it shows moire. `assets/normalmap_test.usda` has three walls
(plain, left-handed and mirrored st) with a bump normal map: with the default
light, every bump must be lit on its upper right on all three walls.
`assets/material_test.usda` is a sphere chart: gold (metallic 1) and red
plastic, roughness 0.05 to 1.0 from left to right, a specular-workflow sphere,
and a floor with a roughness checker texture whose smooth squares mirror the
spheres.

Dependency locations can be overridden with `CMAKE_PREFIX_PATH`,
`-DOptiX_INSTALL_DIR=...`, `-DIMGUI_DIR=...`, `-DCUDAToolkit_ROOT=...` and
`-DVIZ_PREFIX=...` (default `/opt/viztools`; configuring fails if it does not
exist). `-DCLOUDXR_SDK_DIR=...` sets the built-in default for `--cloudxr-sdk`.

## Layout

- `src/Scene.*`: geometry extraction and triangulation into one local-space `RenderMesh` per prim (welded with meshoptimizer), materials, texture loading, cameras
- `src/StbImage.cpp`, `src/third_party/stb_image.h`: image decoding (stb_image v2.30, public domain / MIT)
- `src/OptixRenderer.*`: OptiX context, pipeline, SBT, per-mesh GAS + IAS build and launch
- `src/devicePrograms.cu`: raygen / miss / closest-hit / any-hit programs (compiled
  to OptiX-IR by nvcc and embedded into the binary via `cmake/EmbedFile.cmake`)
- `src/App.*`: SDL3 + ImGui application and panels
- `src/XrOutput.*`: OpenXR session, Vulkan device, CUDA-shared eye buffers and eye cameras
- `src/CloudXrService.*`, `src/third_party/cxrServiceAPI.h`: in-process NVIDIA CloudXR runtime
- `assets/sample_scene.usda`: solid color test scene
- `assets/textured_scene.usda`, `assets/textures/uv_grid.png`: texture test scene
- `assets/mipmap_test.usda`: texture minification and aliasing test
- `assets/normalmap_test.usda`, `assets/textures/bumps_normal.png`: normal map test
- `assets/material_test.usda`, `assets/textures/roughness_checker.png`: roughness/metallic test
- `assets/camera_test.usda`: USD camera test (references `sample_scene.usda`)
