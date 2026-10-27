# Answers to the notes in `UsdOptixViewer-literate.pdf`

The PDF (`LiterateP/UsdOptixViewer-literate.pdf`, as annotated on 2026-10-26)
has 13 notes. They are answered below in page order. Each answer gives the page,
the section of the book, the highlighted text and the note.

Some notes ask for changes (1, 2, 3, 4, 7, 8). This document only describes
what each change involves. None of them has been made yet.

---

## 1. Are the scene structures necessary, or redundant with OptiX?

*Page 4, overview. Highlight: "The scene reads a USD stage, mirrors its prims
into an EnTT registry, resolves materials and textures, and flattens all
geometry into arrays of world-space triangles."*

The scene part builds two structures, and the answer differs for each.

**The flat triangle arrays (`FlatGeometry`) are necessary.** OptiX has no
scene description of its own. It doesn't read files, and it knows nothing
about meshes, materials, textures, names or cameras. It takes raw buffers of
vertices and indices and builds an opaque acceleration structure (the GAS)
from them. When a ray hits a triangle, OptiX reports only the triangle's
index and its barycentric coordinates. Everything else the shading code needs
must be in arrays that the program builds itself and indexes by that
triangle number (`triMesh`, `triMaterial`, `triUVs`, `triNormals`,
`materials`, `textures`). These arrays are exactly what gets uploaded to the
GPU. They are the input OptiX needs, not a copy of something OptiX already
has (see answer 10).

**The EnTT registry is not needed for rendering.** The renderer never reads
it. It serves only the user interface: the hierarchy tree, the inspector,
selection, and mapping a picked mesh id back to a prim. It isn't redundant
with OptiX, which has no hierarchy. It is partly redundant with **USD**,
though, because a `UsdStage` already provides the prim hierarchy, names and
types. Answer 11 discusses removing it.

## 2. Delete the installation script

*Page 4, overview. Highlight: "Installation for all users shows the script
that moved the tools to a system-wide location." Note: "Delete this script".*

This makes sense: `install_all_users.sh` can no longer be run, because the
build trees it copied from were deleted after task 3. Deleting it touches:

- `install_all_users.sh`: `git rm`.
- `LiterateP/src/90-install.nw`: remove the whole part, and the bullet in the
  overview that announces it.
- `CLAUDE.md`: the book will then hold 16 project files instead of 17, so
  update "every line of the 17 project source files". `lp.py check` compares
  whatever root chunks the book defines, so it needs no change.
- `README.md` (file table), `summary.md` (file table) and `CLAUDE.md` (the note
  that it can't be re-run): remove the references.
- `logbook.md`: keep it as history, and add a line saying that the script was
  removed.

The script stays in git history (commit `e603657` and earlier) if it is ever
needed as a reference. If the system-wide installation should still be
documented, a short prose paragraph in the book's build part can replace it.
It would say where the tools live and point to `/etc/profile.d/viztools.sh`.

## 3. Delete the local copy of the tools and update the CMake file

*Page 6, sections 8–9 ("Finding the tools"). Note: "delete the local copy of
the tools in Step 8, and update the cmake".*

Current state of `~/.local/opt`:

| Directory | Size | Action |
|-----------|------|--------|
| `cuda-13.2` | 7.2 GB | delete |
| `usd-26.11` | 84 MB | delete |
| `sdl3`, `imgui-1.93.0`, `entt-4.0.0`, `optix-9.1.0` | ~14 MB | delete |
| `viztools-env.sh` | 4 KB | delete (replaced by `/etc/profile.d/viztools.sh`) |
| `UsdOptixViewer` | 1.7 MB | **keep**: it is the install prefix used in `CLAUDE.md` |

These are the user's own files, so no sudo is needed.

CMake changes (section 9, chunk `<<Choose the root of the installed tools and of CUDA>>`):

- Drop the `~/.local/opt` fallback, and fail clearly instead:
  ```cmake
  set(VIZ_PREFIX "/opt/viztools" CACHE PATH "Root of the installed visualization tools")
  if(NOT EXISTS "${VIZ_PREFIX}")
    message(FATAL_ERROR "Visualization tools not found in VIZ_PREFIX=${VIZ_PREFIX}")
  endif()
  ```
- Drop the last CUDA fallback, `${VIZ_PREFIX}/cuda-13.2`. CUDA is in
  `/usr/local/cuda-13.2`, not under `/opt/viztools`, so that branch could only
  ever find the per-user copy.
- Update the comment block in section 8 and the prose of sections 8 and 9.
  They describe the history ("first installed for one user… falls back to the
  personal one"), which would no longer be true. Also update `CLAUDE.md`
  ("Older per-user copies remain in `~/.local/opt`").

Then reconfigure from a clean build directory, rebuild, and run
`python3 LiterateP/lp.py check`.

## 4. Install Vulkan and CloudXR for all users

*Page 7, section 13 ("Fetch the OpenXR loader and the Vulkan headers"). Note:
"Install locally both vulkan and cloudxr for all users."*

The build currently fetches three things. Each can move to `/opt/viztools` or
to system packages:

| Piece | Today | All-users install |
|-------|-------|-------------------|
| Vulkan **runtime** (`libvulkan.so.1`) | Already installed system-wide (`libvulkan1` 1.3.275, NVIDIA ICD from `libnvidia-gl-595`) | Nothing to do |
| Vulkan **headers** | Fetched from Khronos at tag `v1.3.275` | `apt install libvulkan-dev vulkan-tools`. The Ubuntu 24.04 package is the same version, 1.3.275 |
| OpenXR **loader** | Fetched at `release-1.1.58` and built statically | Build OpenXR-SDK 1.1.58 once and install it to `/opt/viztools/openxr-1.1.58`. Ubuntu's `libopenxr-dev` is an older 1.0.x release |
| CloudXR **SDK** 6.2.1 | Read from `/srv/isaac-shared/cxr/.../CloudXR-6.2.1-Linux-sdk` through `--cloudxr-sdk` | Copy it to `/opt/viztools/cloudxr-6.2.1` |

Plan (one root script for the user to run with `sudo bash`):

1. Run `apt install libvulkan-dev vulkan-tools`.
2. Build OpenXR-SDK `release-1.1.58` in the scratchpad (static loader, no
   tests or layers, as now), then `cmake --install` it to
   `/opt/viztools/openxr-1.1.58`. This step needs no root, apart from the copy.
3. Copy the CloudXR SDK to `/opt/viztools/cloudxr-6.2.1`.
4. Add `CLOUDXR_SDK=/opt/viztools/cloudxr-6.2.1` and the OpenXR prefix to
   `/etc/profile.d/viztools.sh`.

Code and book changes:

- Section 13: `find_package(Vulkan REQUIRED)` (it only needs the headers
  target) and `find_package(OpenXR REQUIRED CONFIG)` with
  `${VIZ_PREFIX}/openxr-1.1.58` on the prefix path. FetchContent can stay as a
  fallback, so the project still builds on machines without the install.
- `--cloudxr-sdk` defaults to `$CLOUDXR_SDK`, so the long path in `CLAUDE.md`'s
  test command goes away.
- Update `CLOUDXR.md`, `CLAUDE.md` (installed tools table) and the logbook.

Before copying CloudXR, check its NVIDIA license terms. Copying it within the
same machine should be fine, but it is a different case from the open-source
tools.

## 5. Do we lose performance by compiling for `compute_75`?

*Page 8, section 14 ("Device code"). Note: "Do we loose any performance for
compiling for compute_75 instead of a newer version?"*

**In practice, no.** The reason is that PTX isn't the code that runs.
`optixModuleCreate` hands the PTX to the OptiX compiler inside the driver. That
compiler translates it to native machine code (SASS) for the GPU that is
actually present, `sm_89` on the RTX 6000 Ada. The optimizer, the register
allocator, the instruction scheduling and the use of the RT cores are all
specific to Ada, whatever the PTX target was.

`compute_75` only limits which *PTX features* nvcc may use while it writes
the intermediate code. The features added after 7.5 are tensor-core
formats, `bf16`/`fp8` arithmetic, asynchronous copies, thread-block clusters,
and so on. This program uses none of them: it is FP32 math, texture fetches
and OptiX intrinsics. So the PTX for `compute_89` would be almost identical.

Costs and alternatives:

- **Startup time.** The JIT compile runs when the pipeline is created. OptiX
  caches the result on disk (`~/.cache/NVIDIA/OptixCache`), so only the first
  run after a change of the device code pays it. That holds for any PTX
  target.
- **Measuring it.** `OPTIX_ARCH` is a cache variable, so the comparison is
  cheap:
  `cmake -S . -B build-89 -DOPTIX_ARCH=compute_89 && cmake --build build-89 -j`.
  Then compare the time to 256 samples on the same scene. The expected
  difference is within noise.
- **What does matter more:** `--use_fast_math` (already on), the optimization
  level in `moduleOptions` (default: full), and `debugLevel` (currently
  `MINIMAL`, which keeps only line info and costs nothing measurable).
- **OptiX-IR** (`nvcc -optix-ir` instead of `-ptx`) is the format NVIDIA now
  recommends. It allows full device debugging and can give the OptiX compiler
  more information. It is a possible future change, not a speed fix.

## 6. Comment: "Code generation! :)"

*Page 8, section 14. Highlight: the `add_custom_command` that runs
`EmbedFile.cmake`.*

Yes. The build runs two code generators in a row: nvcc generates PTX from
CUDA C++, and a CMake script generates C++ from PTX. The resulting executable
carries its GPU programs and needs no data files.

The same thing could be done in other ways:

- `bin2c`, which ships with CUDA (`bin2c -c --name g_devicePrograms ...`).
  This is the classic way in older OptiX SDK samples.
- `xxd -i`. This needs the `xxd` package, which isn't always present.
- `#embed` from C23/C++26. It would remove the generator completely, but it
  needs a newer compiler than g++ 11.5.

The CMake script was chosen because it needs nothing beyond CMake itself.

## 7. Can Dear ImGui be a prebuilt library for all users?

*Page 8, section 15. Highlight: `add_library(imgui STATIC`. Note: "Is it
possible to create in advance a library for imgui that all users can refer
to, instead of creating the library here?"*

**Yes.** It works like this:

1. Build `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`,
   `imgui_widgets.cpp`, `imgui_demo.cpp` and the backends that the machine
   supports (`imgui_impl_sdl3`, `imgui_impl_sdlrenderer3`,
   `imgui_impl_opengl3`, and possibly `imgui_impl_vulkan`) into
   `libimgui.a`, compiled with `-fPIC` so it can also go into shared
   libraries.
2. Install it with its headers and a small CMake package to
   `/opt/viztools/imgui-1.93.0/{lib,include,lib/cmake/imgui}`. The package
   exports a target `imgui::imgui` that links `SDL3::SDL3`.
3. In the viewer, section 11 becomes `find_package(imgui REQUIRED CONFIG)`, and
   section 15 loses its `add_library` and links `imgui::imgui`.

**Why ImGui recommends against it.** Dear ImGui is designed to be compiled
into each application. That is why it is distributed only as sources.

- Compile-time options in `imconfig.h` (`IMGUI_USER_CONFIG`, `ImDrawIdx`,
  `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, custom asserts) change structure
  layouts. Every program that links the shared build must use the same
  options. `IMGUI_CHECKVERSION()` catches mismatches at startup.
- There is no ABI stability between versions. The version number in the
  directory name already handles that.
- The backends are tied to a specific SDL3 build, which is fine here because
  SDL3 is also in `/opt/viztools`.

The gain is small: it saves a few seconds of compile time per clean build.
The real benefit is consistency if several projects on the machine use
ImGui. A middle way is to keep compiling ImGui per project but ship a
`imgui.cmake` helper in `/opt/viztools/imgui-1.93.0` that defines the target.
Every project then gets the same target with one `include()`.

## 8. Move the alignment explanation to sections 24 and 25

*Page 11, section 23 ("Materials"). Highlight: the paragraph that begins "The
order of the fields is not accidental…". Note: "Maybe this explanation should
be in 24 and 25, where the details of the structures are explained."*

Agreed. Section 23 introduces `MaterialData` as a whole, and the field
layout belongs where the fields appear. A proposed split, checked against the
real struct (`sizeof(MaterialData)` is 192 bytes, with no padding):

- **Section 24**, after "…through these same coefficients.":
  > These six members come first for a reason. A `float4` must start on a
  > 16-byte boundary. Placed at the head of the structure, the six of them
  > fill 96 bytes with no gap.

- **Section 25**, at the end of its prose:
  > Every remaining member needs only 4-byte alignment, so no padding can
  > appear after the vectors. Each `float3` is followed by an `int`, which
  > completes a 16-byte line. The whole structure is 192 bytes, a multiple
  > of 16, and has the same layout on both sides of the bus. Texture
  > references are indices into the array of CUDA texture objects of
  > `<<The scene as seen by the device>>`, with −1 meaning "no texture".

- **Section 23** keeps only the first paragraph.

A `static_assert(sizeof(MaterialData) == 192)` in `LaunchParams.h` would turn
the claim into a check, if wanted.

## 9. The accumulation buffer: an example

*Page 12, section 27 ("The output of a launch"). Highlight: "accumBuffer keeps,
for every pixel, the running sum of the samples in its first three components
and their number in the fourth, and colorBuffer receives their average,
encoded for display." Note: "not clear… can you give an example?"*

Take one pixel on the edge of a red box seen against a blue sky. Frame 0
shoots its ray exactly through the pixel centre. Later frames jitter the ray
randomly inside the pixel, so some rays hit the box and others hit the sky.
Suppose the samples are:

- box: `(0.8, 0.1, 0.1)`
- sky: `(0.3, 0.5, 0.9)`

Each launch adds one sample. Each row shows what the ray-generation program
of `devicePrograms.cu` writes:

| `frameIndex` | Sample hits | `accumBuffer[pixel]` (r, g, b, n) | Average = sum / n | `colorBuffer` = 255·√avg |
|---|---|---|---|---|
| 0 | box | (0.8, 0.1, 0.1, **1**) | (0.80, 0.10, 0.10) | (228, 80, 80) |
| 1 | sky | (1.1, 0.6, 1.0, **2**) | (0.55, 0.30, 0.50) | (189, 139, 180) |
| 2 | box | (1.9, 0.7, 1.1, **3**) | (0.63, 0.23, 0.37) | (202, 123, 154) |
| 3 | sky | (2.2, 1.2, 2.0, **4**) | (0.55, 0.30, 0.50) | (189, 139, 180) |
| … | … | … | → about half box, half sky | an anti-aliased edge |

Points to notice:

- **Sum, not average.** The buffer stores the sum and the count instead of
  the average, so adding a sample is one addition. The average is computed
  only for display. The fourth component, `n`, always equals
  `frameIndex + 1`, as the code comment says.
- **Gamma.** The square root is an approximate sRGB encoding: linear 0.25
  becomes 0.5 on screen.
- **Reset.** When the camera moves, `frameIndex` returns to 0. On frame 0 the
  program *writes* the sample instead of adding it, so the old sums vanish
  without clearing the buffer.
- **Picking.** `meshIdBuffer` is written only on frame 0, the unjittered
  centre ray. This pixel is therefore picked as "the box", even though its
  final colour mixes box and sky.
- **Noise.** The same mechanism averages away the noise of soft effects,
  such as the random glossy reflection direction chosen on each frame.

## 10. Explain the device scene structure

*Page 13, section 29 ("The scene"). Highlight: "All geometry is flattened into
one list of world-space triangles and stored in a single geometry
acceleration structure (GAS), whose handle is what optixTrace needs.
Alongside it, plain arrays indexed by…". Note: "explain this structure".*

The scene on the GPU is a *structure of arrays*. Every per-triangle array is
indexed by the same number: the primitive index `i` that OptiX returns from
`optixGetPrimitiveIndex()` on a hit.

Example: a scene with two meshes. Mesh 0 is a quad (two triangles) with a
textured material. Mesh 1 is a single triangle with a plain red material.

```
vertices    = [ v0, v1, v2, v3,  v4, v5, v6 ]                 world-space float3
indices     = [ (0,1,2), (0,2,3), (4,5,6) ]                   3 vertex numbers per triangle
                 tri 0    tri 1    tri 2
triMesh     = [ 0, 0, 1 ]                                     which mesh each triangle came from
triMaterial = [ 0, 0, 1 ]                                     index into materials
triUVs      = [ st0,st1,st2, st0,st2,st3, 0,0,0 ]             3 corners per triangle
triNormals  = [ n,n,n, n,n,n, 0,0,0 ]                         3 corners per triangle; 0 = flat
materials   = [ {texture = 0, ...}, {color = red, texture = -1, ...} ]
textures    = [ cudaTextureObject for "wood.png" ]
handle      = GAS built from vertices + indices
```

When a ray hits triangle `i = 1` with barycentrics `(b1, b2)`, the
closest-hit program does this:

1. `params.indices[1] = (0,2,3)` gives the three corner positions in
   `params.vertices`, from which it computes the geometric normal.
2. `params.triMesh[1] = 0` is the mesh id. It goes into the payload, for
   picking and for the orange highlight of the selection.
3. `params.triMaterial[1] = 0` gives `params.materials[0]`, which says "diffuse
   from texture 0".
4. `params.triUVs[3·1 .. 3·1+2]` gives the three corner UVs. They are
   interpolated with the barycentrics, `uv = (1−b1−b2)·uv0 + b1·uv1 + b2·uv2`,
   and the result samples `params.textures[0]`.
5. `params.triNormals[3·1 .. ]` works the same way for smooth shading. A zero
   first corner means "use the flat normal".

The design has several advantages:

- OptiX gives the hit program only `i` and the barycentrics. Flat arrays turn
  that into all the other data with plain loads.
- **Per corner, not per vertex.** A cube corner shared by three faces has
  three different UVs and normals. Storing three values per *triangle*
  handles such face-varying data with no special case, at the price of some
  duplicated memory.
- **A single GAS** keeps traversal simple: one handle and one SBT record per
  ray type. The price is that moving an object means rebuilding the GAS. An
  instance acceleration structure (IAS) with one GAS per mesh would avoid
  that, at the cost of more complex setup.

## 11. Could the scene live in OptiX instead of EnTT? Can EnTT be removed?

*Page 14, section 30. Highlight: "Everything the viewer knows about a USD file
passes through one object of class Scene. It has three jobs…". Note: "Can the
scene be represented in OptiX instead of in EnTT? Can EnTT be removed from
this program and do everything with OptiX and the other libraries?"*

**OptiX can't hold the scene.** OptiX up to version 6 had a node-graph API,
with `Group`, `Transform` and `GeometryInstance` nodes that carried
variables. OptiX 7 removed it on purpose. Since then the only "scene" in
OptiX consists of *traversables* (GAS and IAS): opaque GPU acceleration
structures without names, parents, materials or a way to query them from the
host. All the data that the UI shows (prim paths, types, hierarchy, the
mesh-to-prim mapping) has to live on the CPU in some other structure.

**EnTT can be removed.** The other libraries cover its role:

| Used today | Replacement without EnTT |
|---|---|
| `PrimInfo` (path, name, type) | `UsdPrim::GetPath()`, `GetName()`, `GetTypeName()` |
| `Hierarchy` (parent, children) | `UsdPrim::GetParent()`, `GetFilteredChildren(UsdTraverseInstanceProxies())` |
| `MeshGeometry` component | `std::unordered_map<SdfPath, MeshGeometry, SdfPath::Hash>` |
| `CameraView` component | `std::unordered_map<SdfPath, unsigned>`, or the path stored in `SceneCamera` |
| `meshEntity[meshId]` for picking | `std::vector<SdfPath> meshPath` |
| `m_selected` (an entity) | an `SdfPath` (empty = nothing selected) |

USD *is* a scene graph, so walking the stage directly would also remove the
mirroring step in `createEntities`. The work involves `Scene.h/.cpp`, about 20
places in `App.cpp`, the build (`EnTT::EnTT`) and the corresponding parts of
the book.

**What would be lost:**

- A cheap place to attach application state to prims (visibility toggles,
  per-prim overrides, cached UI data) without touching USD. That is what an
  ECS is good at.
- Fast iteration over "all prims with component X". This doesn't matter at
  this program's scale.
- Its role as an EnTT sample. EnTT was one of the tools this machine was set
  up with, and the viewer is its only user.

**Recommendation:** if the goal is the simplest correct viewer, remove EnTT
and use USD as the scene graph. If the goal is to demonstrate the installed
tools, or to grow toward editing, animation or per-prim app state, keep it.

## 12. What is `entt::entity`? Literature and its use here

*Page 15, section 33. Highlight: `entt::entity` (in `Hierarchy::parent`).
Note: "explain the concept of entt::entity and give examples, both from the
literature in the web and the use in this program."*

### The concept

EnTT implements the **Entity–Component–System (ECS)** pattern:

- An **entity** is only an identifier. It has no data and no behaviour.
- A **component** is a plain struct of data attached to an entity, at most
  one of each type per entity.
- A **system** is any code that iterates over the entities that have a given
  set of components (a *view*) and processes them.

Unlike a class hierarchy (`class Mesh : public Prim`), an object's "type" is
just the set of components it carries. That set can change at run time.

In EnTT, `entt::entity` is `enum class entity : std::uint32_t`. The 32 bits
split into an **index** (low 20 bits, `entity_mask = 0xFFFFF`) and a
**version** (high 12 bits, `version_mask = 0xFFF`), as defined in
`entt/entity/entity.hpp`. The index selects a slot in the registry. The
version increases each time a slot is reused, so a stale handle to a
destroyed entity is detected by `registry.valid(e)`. `entt::null` is the
reserved "no entity" value. Each component type is stored in its own
**sparse set**: a sparse array maps entity index to a position in a packed,
contiguous array of components. Lookup is O(1), and iteration runs over
contiguous memory.

### Literature

- Michele Caini (author of EnTT), *ECS back and forth*, a blog series that
  explains sparse sets, groups and the design of EnTT:
  <https://skypjack.github.io/2019-02-14-ecs-baf-part-1/>
- EnTT wiki, *Crash Course: entity-component system*:
  <https://github.com/skypjack/entt/wiki/Entity-Component-System>
- Adam Martin, *Entity Systems are the future of MMOG development* (2007),
  the series that popularized the pattern:
  <https://t-machine.org/index.php/2007/09/03/entity-systems-are-the-future-of-mmog-development-part-1/>
- Scott Bilas, *A Data-Driven Game Object System*, GDC 2002 (Dungeon Siege),
  one of the earliest component-based game object descriptions.
- Sander Mertens (author of flecs), *Building an ECS*, a series comparing
  storage designs: <https://ajmmertens.medium.com/>
- Other ECS implementations: Unity DOTS/Entities, Bevy (Rust), flecs.

A typical example from those sources is a game loop:

```cpp
struct Position { float x, y; };
struct Velocity { float dx, dy; };

entt::registry reg;
entt::entity e = reg.create();
reg.emplace<Position>(e, 0.f, 0.f);
reg.emplace<Velocity>(e, 1.f, 0.f);    // a static entity would simply have no Velocity

for (auto [ent, pos, vel] : reg.view<Position, Velocity>().each())   // the "movement system"
    pos.x += vel.dx * dt, pos.y += vel.dy * dt;
```

### Use in this program

- **One entity per prim.** `Scene::createEntities` (`Scene.cpp:256`) walks the
  stage recursively. For every prim it calls `m_registry.create()` and attaches
  `PrimInfo` (path, name, type) and `Hierarchy` (parent entity and child
  entities). The pseudo-root becomes `m_root`.
- **Optional components mark capabilities.** Only prims that produced
  triangles get `MeshGeometry` (`Scene.cpp:607`). Only cameras get
  `CameraView` (`Scene.cpp:655`). The inspector asks
  `registry.try_get<MeshGeometry>(m_selected)`. A null result simply means
  "not a mesh", so the code needs no type switch.
- **Views as systems.** The hierarchy filter iterates
  `registry.view<PrimInfo>().each()` (`App.cpp:150`) to find matching names. It
  then walks `Hierarchy::parent` upward to keep their ancestors visible.
- **Entities as handles.** `m_selected` is an `entt::entity`. Picking maps
  the mesh id from `meshIdBuffer` to an entity through
  `FlatGeometry::meshEntity`. The tree view uses `entt::to_integral(e)` as a
  stable ImGui ID.

## 13. What is a stage in USD?

*Page 16, the `Scene` class interface (after section 36). Highlight:
`const pxr::UsdStageRefPtr& stage() const`.
Note: "explain what is a stage in USD".*

A **`UsdStage`** is the composed scene that a program sees when it opens a USD
file. It isn't the file itself.

- A USD scene is usually spread over many **layers** (`.usd`, `.usda`, `.usdc`,
  or inside a `.usdz`). Each layer holds *opinions* about prims and
  properties.
- Opening a file with `UsdStage::Open(path)` loads that file as the **root
  layer**. It then follows its sublayers, references, payloads, inherits,
  variant sets and specializes, and **composes** all opinions into a single
  hierarchy of `UsdPrim`s. When opinions conflict, the strongest wins. The
  strength order is known by the acronym LIVRPS: Local, Inherits, Variants,
  References, Payloads, Specializes.
- The stage also carries scene-wide metadata (`upAxis`, `metersPerUnit`, the
  start and end time codes), and it controls which payloads are loaded and
  which layer receives edits (the **edit target**).
- `UsdStageRefPtr` is a reference-counted pointer. The stage lives as long
  as someone holds it.

An analogy: layers are like source files, and the stage is like the linked
program. Composition plays the role of the linker, and stronger opinions
override weaker ones.

In this program (`Scene::load`, `Scene.cpp:214–224`):

- `UsdStage::Open(path)` opens the stage. Because asset paths go through
  `ArResolver`, the same call works for `.usda` files and for `.usdz`
  packages.
- `UsdGeomGetStageUpAxis(stage)` reads the up axis, so Z-up scenes are oriented
  correctly.
- The time code is `GetStartTimeCode()` when the stage has an authored time
  range, or `Default` otherwise. All attributes are read at that one time,
  which is why animation is a known gap.
- `stage->GetPseudoRoot()` is the invisible root prim (path `/`). The
  traversal that builds the entities starts there. It uses
  `UsdTraverseInstanceProxies()`, so instanced prims appear as ordinary
  prims.

The `stage()` accessor that the note highlights lets `App` read the composed
stage without reopening the file. When no prim is selected, the inspector
shows stage-level facts: the file, the up axis, `metersPerUnit` and the
authored time range (`App.cpp:515`).
