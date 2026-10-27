# The `cloudxr` branch: XR output for UsdOptixViewer

This branch adds a stereo headset output to UsdOptixViewer, using OpenXR. It
works with any OpenXR runtime that supports Vulkan, and in particular with
NVIDIA CloudXR, which streams the images over the network to an Apple Vision
Pro, a Meta Quest (CloudXR.js in the browser) or a similar device. The
desktop viewer is unchanged when no XR option is given.

The branch starts from `main` at commit `f29e207` ("look through USD
cameras"). This document lists everything that differs from `main`.

## What it does

- The viewer renders both eyes with the same OptiX pipeline as the desktop
  view. It uses the same materials, textures, shadows and reflections, and
  highlights the selection.
- Each eye is written by OptiX straight into Vulkan memory that is shared
  with CUDA. One copy on the GPU then moves it into the runtime's swapchain
  image, so pixels never pass through main memory.
- The headset starts where the desktop view is. The XR origin, which is
  where the head was when the session started, is placed at the eye of the
  orbit camera or of the selected USD camera, facing its horizontal
  direction. When the desktop user orbits or picks another camera, the
  headset follows. One meter of head motion equals 1/`metersPerUnit` scene
  units, times the *World scale* setting.
- The XR runtime picks the GPU. If it isn't the one OptiX started on, the
  renderer restarts on the runtime's GPU (matched by UUID) and uploads the
  scene again, so the shared memory works. This matters on this two-GPU
  machine.
- With CloudXR's `auto-*` device profiles, the headset may connect long after
  the program starts. The desktop keeps working in the meantime, and XR
  starts when the client connects.
- While a session runs, `xrWaitFrame` paces the main loop at the headset's
  rate, and the desktop's vsync is turned off.

## How to run it

```bash
cd UsdOptixViewer
cmake -S . -B build && cmake --build build -j   # first configure downloads OpenXR + Vulkan headers

# 1. CloudXR runtime inside the viewer, from a CloudXR SDK (here: the one the
#    LÖVR sample on this machine downloaded)
SDK=/srv/isaac-shared/cxr/cloudxr-lovr-sample/build/cloudxr/CloudXR-6.2.1-Linux-sdk
./build/UsdOptixViewer assets/camera_test.usda --camera /World/Cameras/Front \
    --cloudxr --cloudxr-sdk $SDK --cloudxr-profile apple-vision-pro --xr-mirror

# 2. CloudXR runtime started separately (Isaac Teleop, as in ~/cxr/cxr-start.sh):
#    the viewer only needs the OpenXR runtime manifest
source ~/.cloudxr/run/cloudxr.env        # sets XR_RUNTIME_JSON
./build/UsdOptixViewer scene.usd --xr

# 3. Any other OpenXR runtime with XR_KHR_vulkan_enable2 (Monado, SteamVR, ...)
XR_RUNTIME_JSON=/path/to/runtime.json ./build/UsdOptixViewer scene.usd --xr
```

| Option | Meaning |
|---|---|
| `--xr` | Send the view to the active OpenXR runtime (`XR_RUNTIME_JSON`). |
| `--cloudxr` | Start the CloudXR runtime inside the viewer, then do `--xr`. |
| `--cloudxr-sdk DIR` | Directory with `libcloudxr.so` and `openxr_cloudxr.json`. The default is `$CLOUDXR_SDK_DIR`. |
| `--cloudxr-profile NAME` | CloudXR device profile: `apple-vision-pro`, `quest3`, `auto-native`, `auto-webrtc`, ... Empty means the runtime's default. |
| `--xr-samples N` | Samples per pixel and frame (1 to 64, default 1). Also in the *XR* menu (1 to 8). |
| `--xr-resolution F` | Eye resolution as a fraction of the headset's recommendation (0.1 to 2, default 1). |
| `--xr-mirror` | Show the left eye in the desktop viewport. Also in the *XR* menu. |

The new *XR* menu shows:
- the runtime, the CloudXR client state, the session state, the headset, the
  GPU and the eye size;
- with `--cloudxr`, where a client connects: the IPv4 address of each network
  interface and the service's port, read from its `server-port` property
  (48010 with the `apple-vision-pro` profile, or `NV_CXR_SERVER_PORT`);
- the *World scale*, *Samples per frame* and *Mirror left eye in viewport*
  settings.

The status bar adds the XR state and the time per headset frame.

On the client side, nothing changes from the existing Isaac Teleop setup:
- Vision Pro: the CloudXR client app.
- Quest: CloudXR.js with the `auto-webrtc` profile.
- Network and TLS: the same firewall and stunnel setup.

### Connecting an Apple Vision Pro (tested October 27th)

The headset's CloudXRViewer connects over TLS to port 48322 (it has no port
field). stunnel terminates TLS there and forwards to the runtime on
127.0.0.1:48324, so the viewer must run its CloudXR service on 48324. Without
stunnel the headset reports `0x800B1004`.

1. Once per boot, as root (the same steps as `/srv/isaac-shared/cxr/cxr-start.sh`,
   without starting its own runtime):
   ```bash
   ufw allow from 157.253.113.0/24 to any port 48322 proto tcp
   ufw allow from 157.253.113.0/24 to any port 47998:48012 proto udp
   pgrep -x stunnel || stunnel /etc/stunnel/cloudxr.conf
   ```
2. Start the viewer with the runtime on 48324:
   ```bash
   NV_CXR_SERVER_PORT=48324 ./build/UsdOptixViewer assets/textured_scene.usda --cloudxr \
       --cloudxr-sdk /srv/isaac-shared/cxr/cloudxr-lovr-sample/build/cloudxr/CloudXR-6.2.1-Linux-sdk \
       --cloudxr-profile apple-vision-pro --xr-mirror
   ```
3. In the headset, on the same Wi-Fi: CloudXRViewer, Zone *Manual IP Address*,
   `157.253.113.156` (the address of `wlp195s0`, which the TLS certificate
   names), *Connect*.

The XR menu shows the runtime's port (48324 here), not stunnel's 48322.

CloudXR can only serve one OpenXR app at a time. Run either the viewer or
Isaac Sim, not both.

## File-by-file differences from `main`

| File | Change |
|---|---|
| `UsdOptixViewer/src/XrOutput.h/.cpp` | **New.** The OpenXR client: instance, system discovery (retried for `auto` profiles), Vulkan instance and device created through the runtime (`XR_KHR_vulkan_enable2`), session state machine, swapchains, shared eye buffers (Vulkan memory exported as an opaque file descriptor and imported into CUDA), and the frame loop (wait, locate views, copy buffer to image, end frame with a projection layer). It also builds each eye's camera: maps the XR axes (Y up, -Z forward, meters) to the scene, and handles asymmetric frusta. Vulkan is loaded at run time from `libvulkan.so.1`. |
| `UsdOptixViewer/src/CloudXrService.h/.cpp` | **New.** Loads `libcloudxr.so` from a CloudXR SDK, sets the device profile, starts the service, points `XR_RUNTIME_JSON` at the SDK's manifest, reports client connect/disconnect events, and gives the addresses and port a client connects to. It restores SDL's SIGINT/SIGTERM handlers after the service installs its own, so that Ctrl+C ends the session before the service stops. |
| `UsdOptixViewer/src/third_party/cxrServiceAPI.h` | **New.** NVIDIA's CloudXR service API header (Apache-2.0 OR MIT), copied from the CloudXR 6.2.1 SDK. The CloudXR libraries themselves are not in the repository. |
| `UsdOptixViewer/CMakeLists.txt` | Fetches Vulkan-Headers v1.3.275 and OpenXR-SDK release-1.1.58 with `FetchContent`. The OpenXR loader is built static. Links `OpenXR::openxr_loader` and `Vulkan::Headers`, and defines `VK_NO_PROTOTYPES` and `XR_USE_GRAPHICS_API_VULKAN`. Adds the two new source files. |
| `UsdOptixViewer/src/OptixRenderer.h/.cpp` | `init(error, deviceUuid)` picks the CUDA device by UUID. `release()` (the old destructor body, which now also resets every handle) allows a restart on another GPU. `deviceUuid()` is new. `renderView(camera, w, h, target, samples)` renders an eye into a device buffer with its own scratch buffers, so the desktop image and its accumulation are not touched. |
| `UsdOptixViewer/src/LaunchParams.h` | Comment only: `Camera::w` isn't unit length for XR eyes (window centre of an asymmetric frustum). No layout change, so `devicePrograms.cu` is identical to `main`. |
| `UsdOptixViewer/src/App.h/.cpp` | New options. The XR runtime starts before the renderer. New: `updateXr()` once per loop turn (also while minimized), `renderXrEyes()`, `useRendererOn()`, `drawXrMirror()`, `xrStatus()`, the *XR* menu, an XR segment in the status bar, and the mirror in the viewport (no desktop rendering or picking while it is shown). The orbit math moved into `orbitOffset()`, and `viewFrame()` gives the desktop eye and direction to the XR anchor. |
| `LiterateP/` | New part `75-xr.nw` documents `XrOutput` and `CloudXrService`. Updated parts: intro, build, shared parameters, renderer and application. `lp.py` covers the 4 new files (17 in total). |
| `CLOUDXR.md` | This document. |

Not changed: `Scene.*`, `devicePrograms.cu`, `main.cpp`, `StbImage.cpp`, the
test scenes and the install script. The desktop image is identical to
`main`'s.

## New dependencies

| Dependency | How it is obtained | Needed at |
|---|---|---|
| OpenXR loader + headers 1.1.58 | `FetchContent` from github.com/KhronosGroup/OpenXR-SDK, built static | build (network on first configure) |
| Vulkan headers 1.3.275 | `FetchContent` from github.com/KhronosGroup/Vulkan-Headers | build |
| Vulkan loader `libvulkan.so.1` | Ubuntu package `libvulkan1` (already installed) and the NVIDIA driver | run time, XR only |
| CloudXR SDK (`libcloudxr.so`, `libopenxr_cloudxr.so`, ...) | NVIDIA NGC, under NVIDIA's license. Found at run time through `--cloudxr-sdk`. Not in the repository. | run time, `--cloudxr` only |

The all-users toolchain in `/opt/viztools` was not changed.

## Verified on this machine (no headset attached)

- The build has no warnings, including the fetched OpenXR loader.
- `--cloudxr` with the `apple-vision-pro` profile:
  - The CloudXR 6.2.1 runtime started in-process and a session reached
    *running* (a static profile needs no client).
  - Two eyes of 2400 × 1920 pixels, 63 mm apart along the camera's right
    axis, on the RTX 6000 Ada that CUDA renders with.
  - About 2.8 ms per headset frame for both eyes at 1 sample per pixel
    (render plus copy, without the wait).
  - The left-eye mirror shows the scene from the `Front` camera's eye at
    standing height, with the objects in the right order.
- `--xr` against a CloudXR runtime hosted by another process (standing in
  for Isaac Teleop): the session ran with textures at 4 samples per pixel,
  in 4.2 ms per frame.
- Shutdown (window close or SIGTERM) ends the session first and then stops
  the service. The runtime's lock file is removed and the exit code is 0.
- Errors leave the desktop viewer working, with a popup:
  - `--xr` with no runtime: "no OpenXR runtime is available ...".
  - A second app on a single-session runtime: `XR_ERROR_LIMIT_REACHED`.
- Desktop rendering is the same as on `main`.

**Verified with a Vision Pro (October 27th):** the viewer streamed to the
headset through stunnel, with the color, alpha/depth, input and audio streams
connected, in two sessions of 3 and 4 minutes.

**Not verified:** the image in a Quest, head
tracking with real motion, `auto-*` profiles with a real client connecting
and disconnecting, and the renderer switching GPUs (here the runtime always
picked the GPU CUDA was already using).

## Limitations and possible next steps

- **No input from the headset.** Controllers and hands aren't read. The
  desktop user moves the XR origin with the orbit camera or the USD cameras.
- **1 sample per pixel by default.** Edges alias, and glossy surfaces show a
  fixed noise pattern. Raise *Samples per frame* if the GPU budget allows
  (90 Hz leaves about 11 ms per frame).
- **No reprojection help.** The runtime gets no depth layer
  (`XR_KHR_composition_layer_depth` is available and would improve the
  runtime's reprojection).
- **No foveation and no passthrough.** The opaque blend mode is used, and
  the eyes are rendered at uniform resolution.
- **One extra copy per eye per frame** (buffer to swapchain image). Importing
  the swapchain images into CUDA as surfaces would remove it, but OptiX would
  then write through surface objects.
- **Audio** isn't streamed. CloudXR supports audio only on Windows.
- CloudXR's own lock file (`$XDG_RUNTIME_DIR/runtime_started`) can be left
  behind by a crash. The next start then fails and says so. Delete the file
  if no other CloudXR runtime is running.

## Merging back

The XR code sits behind the `--xr` and `--cloudxr` flags, so merging into
`main` would not change the desktop behaviour. The cost is the network
fetch at configure time. Before merging, consider making it optional
(`option(VIEWER_XR ...)`) or installing the OpenXR SDK into `/opt/viztools`
like the other tools.
