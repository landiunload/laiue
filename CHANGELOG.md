# Changelog

Versions follow [Semantic Versioning](https://semver.org). Before 1.0 the
API and ABI may change between minor versions.

## Unreleased

- On Windows frames go on while the window is dragged or resized, while a
  mouse button holds a caption button and while the window menu is open.
  `DefWindowProc` runs its own message loop until the button is released,
  and `WindowRunLoop` stood still. Now a small thread wakes the window
  procedure for each frame with `SendNotifyMessage`. Such a message gets
  through any wait for messages. A timer does not: the caption buttons'
  loop takes only mouse messages, and before the first mouse move a press
  on the caption takes none at all. Input is reset when the system takes
  the mouse, as on
  focus loss, and raw input reaches the program again only when the system
  gives the mouse back. A press on the caption no longer becomes a click in
  the program.
- Alt+Enter switches borderless full screen in the window module, on
  Windows and X11, with any graphics backend. Before, only D3D12 reacted,
  through DXGI's own handler, and it entered exclusive full screen. There a
  present that allows tearing fails, so without vertical sync the program
  stopped drawing. D3D12 now leaves Alt+Enter to the window. On Vulkan
  Alt+Enter did nothing.
- On Windows Alt or F10 pressed alone no longer stops frames. They started
  the system menu's modal loop until the next key; Alt+Space still opens
  the menu.
- On X11 `WindowSetFullscreen` asks the window manager for full screen
  (EWMH); before it only stored the flag. A held key no longer counts as
  repeated presses, because the window asks for detectable autorepeat.
- On X11 a key types the character of its keysym, so Cyrillic and every
  character a layout names by a Unicode keysym are typed, not only Latin-1.
  As on Windows, keys held with Ctrl or Alt type nothing. The conversion is
  portable and tested on every platform.
- On X11 moving the window no longer reports a resize, only a new size does.
  Losing focus with Enter held no longer swallows the next Alt+Enter, and
  only a `WM_PROTOCOLS` message can close the window. The window asks the
  server for all its atoms once, in one round trip.
- `WindowSetPowerPolicy` chooses how the process spends power, also through
  the window service's optional `setPowerPolicy` tail. `WINDOW_POWER_AUTO`,
  the default, is the background efficiency mode described below.
  `WINDOW_POWER_EFFICIENCY` keeps the process in efficiency mode even while
  it is active. `WINDOW_POWER_PERFORMANCE` never enters it and asks Windows
  for high QoS, so the program is not slowed even in the background. Only
  Windows applies a policy; Linux and Android store it and return false.
  Unloading the window module returns the process to the default mode.
- On Windows a program in the background goes into efficiency mode, as with
  the Task Manager switch: its process gets EcoQoS and low base priority, and
  Task Manager shows the green leaf. The window module enters the mode when
  another application becomes active and leaves it when the program is active
  again or its window closes, restoring the previous priority class. Audio
  output keeps high QoS, so playback in the background does not stutter.
- On Windows `PlatformAtomicLoadI64` reads with an ordinary load instead of a
  locked compare-exchange, and on ARM64 the 32-bit acquire load and release
  store use `LDAR` and `STLR` instead of locked instructions. Threads that
  read a shared counter no longer take its cache line from each other and
  from the thread that writes it.
- Without vertical sync the frame rate has no limit. D3D12 presents with
  `ALLOW_TEARING` where the system supports it, and Vulkan prefers
  `IMMEDIATE`, then `MAILBOX`, then `FIFO_LATEST_READY`. A window that covers
  the screen may show a tear, while a composed window shows the newest frame.
  Without tearing support D3D12 shows up to two frames per refresh. Vulkan
  keeps one more swapchain image than the minimum, so on NVIDIA it no longer
  keeps a CPU core busy waiting for a free image.
- The Vulkan renderer took the last suitable device, not the best. Without a
  discrete GPU it could choose a CPU driver such as lavapipe over the
  integrated GPU. It now ranks devices: discrete GPU first, then integrated,
  virtual and other GPUs, and a CPU driver last. Among devices of one type it
  takes the first one the loader lists. With more than eight devices it
  chooses among the first eight instead of failing.
- The graphics device kept a texture or a sampler destroyed between
  `beginFrame` and `endFrame`, and a shader destroyed while a pipeline used
  it, until the device itself was destroyed: `destroyHandle` dropped those
  calls. Now the texture and the sampler are released when the frame ends,
  and the shader with its last pipeline.
- Android runs programs on modules by name, like the desktop: with
  `LAIUE_MODULE_LIBRARY_TYPE=SHARED` every module is its own `.so` in the APK,
  and a missing one turns off only its technology. The `window` module brings
  NativeActivity and calls the program's ordinary `main`; `input` reads touch,
  two-finger gestures and keys. Presets `android-arm64-shared` and
  `android-x86_64-shared`; CI runs the viewer in the emulator.
- On Android `LAIUE_X86_64_LEVEL` defaults to `sse2`, the ABI baseline with
  SSE4.2 and POPCNT. x86_64 APKs were built for AVX2 and would stop with
  `SIGILL` on devices without it.
- The APK's activity takes every configuration change itself, the resource
  overlay Android applies right after installation included. Android used to
  restart it then, and `main` was stopped and run again in the same process.
- On Android a program opened again before its previous activity is
  destroyed, right after it exits or over a running one, waits for the
  previous run of `main` to end. Both runs used to share the program's state
  and output in one process, and destroying the old activity hung the app.
- The D3D12 and Vulkan renderers accept any number of geometry uploads between
  two frames; past 128 they used to refuse them until the next frame. Bytes
  that miss both staging rings of a frame share overflow blocks freed once the
  frame completes, instead of one staging buffer per upload.
- On Android, iOS, tvOS and visionOS `LAIUE_PLATFORM_BACKEND=AUTO` picks the
  bundled mobile adapter, and on Android `LAIUE_MODULE_LIBRARY_TYPE=AUTO` builds
  shared modules: a program needs only the NDK toolchain file.
- `laiue_add_program` builds one program for every platform, and
  `laiue_add_android_apk` packages it: stripped libraries stored uncompressed
  and aligned to 16 KiB pages. The walk APK uses it too and no longer claims
  `minSdkVersion` 1.
- `LaiueWindowServiceV1` gains the optional `setSurfaceCallback` tail and
  `LaiueGraphicsDeviceV2` the `setNativeWindow` tail: the surface Android takes
  away on pause is replaced without recreating the device.
- `LaiueWindowServiceV1` gains the optional `getContentScale` tail: window
  pixels per interface point, from the DPI on Windows, `Xft.dpi` on X11 and
  the screen density on Android. Check `LAIUE_WINDOW_SERVICE_V1_SCALE_SIZE`.
- `examples/viewer`: a PNG, JPEG and GIF viewer in one C file for Windows, Linux
  and Android.
- Decoded pictures may be up to 16384 pixels a side; the LTP layer keeps 4096.
- Input knows `INPUT_KEY_0`, `R`, `F`, `P`, `ENTER` and `BACKSPACE` on
  Windows and X11.
- `LaiueWindowServiceV1` gains the optional `consumeText` tail: the text typed
  since the last call as Unicode code points (`WindowConsumeText`), control
  characters left to the keys. Windows assembles surrogate pairs; X11 gives
  Latin-1 for now. Check `LAIUE_WINDOW_SERVICE_V1_TEXT_SIZE` before calling it;
  `LAIUE_WINDOW_SERVICE_V1_LEGACY_SIZE` is the service without it.
- `LaiueGraphicsDeviceV2` gains the optional `setVerticalSync` and
  `getVerticalSync` tail: presenting can stop waiting for the display's
  refresh and start again at run time. Check
  `LAIUE_GRAPHICS_DEVICE_V2_VERTICAL_SYNC_SIZE` before calling it.
- `LaiueGraphicsDeviceV2` gains the optional `setClearColor` tail: the colour
  every frame is cleared to, the sky behind all geometry. Check
  `LAIUE_GRAPHICS_DEVICE_V2_CLEAR_COLOR_SIZE` before calling it; without it the
  background keeps the previous dark default.
- Release builds carry nothing debug-only. Modules with the new
  `DIAGNOSTICS` role are not built in Release or MinSizeRel, stay out of
  `laiue::engine` and install only for Debug and RelWithDebInfo; the
  `laiue.build.release_hygiene` test installs the Release SDK and fails on
  tests, benchmarks, fuzzers, debug information or diagnostics modules.
  `laiue.graphics.profile` moves to this role.
- `laiue.frame_monitor` (diagnostics only) records frame and zone times,
  reports mean, p50, p90, p99, p99.9 and maximum, and logs spikes over a
  budget or a median multiple with the three heaviest zones. Its
  instrumentation macros compile to nothing without `LAIUE_DIAGNOSTICS`.
- `PlatformMonotonicNanoseconds` returns integer nanoseconds of the
  monotonic clock.
- The renderer keeps an on-disk pipeline cache when
  `RendererSetPipelineCacheDirectory` names a directory: `ID3D12PipelineLibrary`
  on D3D12 and `VkPipelineCache` on Vulkan, behind an engine header that pins
  the backend, vendor, device, driver version, Vulkan cache UUID and a SHA-256
  of the payload. A foreign or damaged file is rejected instead of reaching the
  driver. `RendererDestroy` saves new pipelines, `RendererSavePipelineCache`
  saves on demand and `RendererGetPipelineCacheStatusV1` reports cache hits.
  The graphics service gains the matching optional tail entries.
- `laiue.navigation` finds paths without game rules: a dynamic directed graph
  with per-edge costs and travel-mode masks (cheap cost updates, generation
  checked handles), A* with expansion and cost budgets, time-sliced and
  parallel batch queries, a sparse infinite grid for free movement with
  corner-safe 8-way A* and path smoothing, and flow fields for crowds. All
  arithmetic is integer and results do not depend on the thread count.
- `laiue.persist` saves and loads application state as named, versioned
  sections: CRC-32C on every byte, atomic replacement with rotating backups,
  fallback to the newest intact copy, block LZ compression in parallel,
  byte-for-byte copying of unchanged sections, schema migrations, and
  background save and load jobs with progress and cancellation.
- The platform boundary gains streaming file writes
  (`PlatformFileCreateWrite`, `PlatformFileWrite`, `PlatformFileFlush`),
  durable replacement (`PlatformFileReplaceDurable`, which also syncs the
  directory on POSIX) and UTF-8 delete and stat helpers. External platform
  adapters must implement them. Windows opens files for reading with
  `FILE_SHARE_DELETE`, so an open file can be renamed or replaced.
- Shaders compile with DXC for both backends: DXIL Shader Model 6.0 for
  D3D12 and SPIR-V for Vulkan. fxc and glslang are no longer used.
- Shader packs use contract 2: DXIL for D3D12, SPIR-V for Vulkan. Contract 1
  packs with DXBC are rejected.
- D3D12 requires a device with Shader Model 6.0 and falls back to WARP
  without it.
- Vulkan requests API 1.4. Generic draws use push descriptors on Vulkan 1.4
  or with `VK_KHR_push_descriptor`, which removes the per-frame limit of 256
  distinct descriptor sets.
- Builds use the freshly compiled shaders; previously the checked-in copies
  were always compiled in, and the build output was only verified.
- `laiue::bootstrap` loads modules by name: `LaiueModuleHostLoadNamedProfileV1`
  resolves `window` to `laiue_window.dll`, `liblaiue_window.so` or
  `liblaiue_window.dylib` in the executable directory, and
  `LaiueModuleArtifactFileNameV1` returns that file name.
- `mod/application.h` gives a bootstrap-only application its executable
  directory, a monotonic clock, sleep and a termination request flag.
- The SDK installs `window.h` and `window_service.h` under `platform/`;
  `window_service.h` did not compile from the old `window/` location.
  `ui/ui_service.h` now includes the export macros it uses.
- A source-tree build exposes the same include paths and the same
  `LAIUE_STATIC` definition of `laiue::bootstrap` as the SDK, so an
  application builds unchanged against either.
- The bootstrap archive no longer carries the engine's no-CRT `memcpy` and
  `memmove`; an application linked with its C runtime and LTCG got them twice.
- `laiue.mesher` builds smooth surfaces of density fields
  (`LaiueSurfaceMeshBuildV1`, surface nets) for volumetric terrain and caves.
  Caller-owned buffers make it allocation-free and thread-safe, and blocks of
  a field join without gaps through owned edge ranges.
- `laiue.scene_math` adds `Matrix4Perspective` and `Matrix4LookAt` in the
  renderer's conventions.
- `laiue.scene_math` adds `Matrix4Invert` and `ViewRayThroughScreenPoint`,
  the ray from the camera through a screen point for picking with a cursor.
  Both are an optional service tail checked with
  `LAIUE_SCENE_MATH_SERVICE_V1_PICKING_SIZE`.
- The bootstrap and platform archives are compiled without LTO, so an
  application links them with its own compiler; the Linux SDK built with
  Clang could not be linked by GCC.

## 0.1.0

First public release.

- Modules for worlds with infinite coordinates, physics, D3D12 and Vulkan
  graphics, meshing, scene, UI, audio, models, animation, content packs and
  native mods. Every module is optional.
- Windows, Linux and macOS on x86_64 and ARM64; the portable core also builds
  for Android and iOS.
- Prebuilt SDKs for Windows, Linux and macOS are attached to the release.
