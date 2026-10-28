# VizTools: summary (October 24th, 2026)

## Goal

Set up a GPU visualization toolchain on this machine and prove it works with
a sample app (see `logbook.md`, October 24th, tasks 1 to 3).

## What was done

1. **Tools installed (task 1, per user, then task 3, all users).**
   CUDA 13.2.1, OptiX 9.1.0, SDL3 3.4.18, EnTT 4.0.0, Dear ImGui 1.93.0 and
   OpenUSD 26.11.1 are now in `/usr/local/cuda-13.2` and `/opt/viztools`.
   `/etc/profile.d/viztools.sh` sets up the environment for every user. CMake
   3.28 and g++ 11.5 were already present.
2. **Sample app `UsdOptixViewer` (task 2).** It is about 2,100 lines of C++/CUDA
   and does the following:
   - Opens `.usd/.usda/.usdc/.usdz` files from the command line, a file
     dialog, a typed path, or drag and drop.
   - Shows the prim hierarchy (with a filter) and an inspector with every
     attribute and relationship.
   - Ray traces the scene with OptiX: flat shading and shadows, orbit/pan/zoom,
     click-to-select with highlight, frame selection. Frames take about 0.5 ms.
   - Renders UsdPreviewSurface materials: diffuse textures (including inside
     `.usdz`), GeomSubset materials, alpha cutouts, tangent-space normal maps,
     GGX roughness/metallic (or specular workflow) with progressive glossy
     reflections, and mipmapped, anisotropic texture filtering driven by ray
     differentials.
   - Builds and installs with CMake. The installed binary finds its libraries
     through RPATH.
3. **Verification.**
   - Clean builds against `/opt/viztools` with no warnings.
   - Screenshots confirmed the rendering, selection, hierarchy and inspector.
   - Texture orientation was checked on a purpose-built test scene and on the
     real `McUsd.usdz` asset.
   - The installed binary runs in a clean environment.

4. **Update (October 28th).** The `cloudxr` branch is merged into `main`. The
   viewer no longer uses EnTT: the USD stage is the scene graph, each prim is
   a local-space mesh welded by meshoptimizer 1.3, and OptiX renders one GAS
   per mesh under an IAS. The device code is compiled to OptiX-IR. meshoptimizer,
   the OpenXR loader, the Vulkan headers and the CloudXR SDK are installed for
   all users, and `install_all_users.sh` was removed. See `logbook.md`.

## Where things are

| Path | Contents |
|------|----------|
| `logbook.md` | Task list and detailed results |
| `CLAUDE.md` | Working notes for Claude Code sessions |
| `UsdOptixViewer/` | App sources, CMake project, test scenes, README |
| `~/.local/opt/UsdOptixViewer` | Installed app and sample assets |
| `/opt/viztools`, `/usr/local/cuda-13.2` | Installed tools |

## Open items

- Mouse interaction in the viewport was not automatically tested, because
  synthetic X input doesn't reach SDL on this display. Check it by hand.
- Possible next features: translucency, USD lights (dome/area) and indirect
  diffuse lighting, texture transforms, animation playback.
