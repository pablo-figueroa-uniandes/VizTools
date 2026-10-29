# VizTools

Setup notes for a GPU visualization toolchain on Linux, and **UsdOptixViewer**,
a sample application that uses all of it: an OpenUSD viewer that uses the USD
stage as its scene graph, with meshes prepared by meshoptimizer, a Dear ImGui
interface and an OptiX ray-traced viewport.

## UsdOptixViewer

- **Loading**: `.usd`, `.usda`, `.usdc` and `.usdz` files, from the command line,
  a file dialog, a typed path, or drag and drop.
- **Hierarchy view** of every prim, with a name filter, and an **inspector**
  showing each prim's metadata, attributes, relationships and resolved material.
- **OptiX viewport**, progressively refined up to 256 samples per pixel:
  - UsdPreviewSurface materials:
    - diffuse color/texture
    - roughness, metallic and ior (constants or texture channels), or the
      specular workflow
    - tangent-space normal maps
    - alpha cutouts
    - per-face materials through GeomSubsets
  - Textures load from disk or from inside `.usdz` packages. They are
    mipmapped, with anisotropic filtering whose mip level comes from ray
    differentials.
  - Lighting: GGX specular with glossy reflections, a directional light with
    shadows, and smooth or authored shading normals.
- **Interaction**: orbit, pan and zoom, double-click to orbit around a point, click to select (highlighted in the
  viewport and revealed in the tree), and frame selection.
- **Cameras**: look through any USD camera (perspective or orthographic, with
  its clipping range) from the Camera menu or `--camera`.
- **XR / CloudXR**: stereo output to a headset through
  OpenXR, streamed with NVIDIA CloudXR. See [CLOUDXR.md](CLOUDXR.md).

See [UsdOptixViewer/README.md](UsdOptixViewer/README.md) for the full feature
list, controls, command-line options and test scenes.

## Requirements

Tested on Ubuntu 24.04 with two NVIDIA RTX 6000 Ada GPUs (driver 595), X11.

| Dependency | Tested version | Notes |
|------------|----------------|-------|
| NVIDIA driver | 595 | OptiX 9.1 needs R590 or newer |
| CUDA toolkit | 13.2 | nvcc compiles the OptiX programs to OptiX-IR |
| OptiX SDK headers | 9.1.0 | from the SDK or [NVIDIA/optix-dev](https://github.com/NVIDIA/optix-dev) |
| CMake | 3.28 | 3.24 or newer |
| C++ compiler | g++ 11.5 | C++20 |
| SDL3 | 3.4.18 | |
| meshoptimizer | 1.3 | welds and reorders the meshes |
| OpenXR loader | 1.1.58 | static; XR output |
| Vulkan headers | 1.3.275 | XR output; the loader is opened at run time |
| NVIDIA CloudXR SDK | 6.2.1 | optional, at run time (`--cloudxr`) |
| Dear ImGui | 1.93.0 | sources plus the SDL3 and SDL_Renderer backends |
| OpenUSD | 26.11.1 | core libraries only; Python and imaging are not needed |

## Building

The CMake project looks for the dependencies in `/opt/viztools` (the
all-users layout described in [logbook.md](logbook.md)), CUDA in
`/usr/local/cuda-13.2` and the Vulkan headers in the system. Other locations
can be given explicitly:

```bash
cd UsdOptixViewer
cmake -S . -B build \
    -DVIZ_PREFIX=/path/to/tools \
    -DCMAKE_PREFIX_PATH="/path/to/sdl3;/path/to/usd;/path/to/meshoptimizer;/path/to/openxr" \
    -DOptiX_INSTALL_DIR=/path/to/optix \
    -DIMGUI_DIR=/path/to/imgui \
    -DCUDAToolkit_ROOT=/path/to/cuda \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/opt/UsdOptixViewer
cmake --build build -j
cmake --install build

./build/UsdOptixViewer assets/material_test.usda
```

## Repository layout

| Path | Contents |
|------|----------|
| [UsdOptixViewer/](UsdOptixViewer/) | Viewer sources, CMake project and test scenes |
| [LiterateP/](LiterateP/) | The whole program as a Knuth-style literate program: [PDF](LiterateP/UsdOptixViewer-literate.pdf) (149 pages), noweb sources, and the tool that tangles, checks and weaves them |
| [logbook.md](logbook.md) | Task log: tool installation and every feature added, with how it was verified |
| [summary.md](summary.md) | One-page summary of the work |
| [CLAUDE.md](CLAUDE.md) | Working notes for Claude Code sessions in this repository |

## License

[MIT](LICENSE) © 2026 Pablo Figueroa.

`UsdOptixViewer/src/third_party/stb_image.h` is
[stb_image](https://github.com/nothings/stb) by Sean Barrett, available under
the MIT license or as public domain (see the end of that file). The other
dependencies are not included in this repository and keep their own licenses.
