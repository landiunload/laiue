# Changelog

Versions follow [Semantic Versioning](https://semver.org). Before 1.0 the
API and ABI may change between minor versions.

## Unreleased

- The Android walk check no longer fails on its own timing. It compares a picture
  after a turn only once the screen has come to rest, and it answers the system's
  "not responding" dialog about another process, which took the focus the scenario
  runs on. A dialog about the program itself still fails the run.
- A weekly workflow compares every pinned tool, SDK, Android package and GitHub
  action with the newest stable release, and only that workflow turns red when
  one falls behind. `tools/check_versions.py` does the comparison.
- The run on the Android baseline profiles now proves the profile layer really
  loaded: the loader skips a layer it cannot load without a word.
- Everything built with Clang now uses LLVM 23. On Windows CI took the runner's
  LLVM 20, so it did not check the compiler releases are built with.
- The integer square root of the A* heuristic no longer loops. LLVM 23
  vectorized the small correction loop, which almost never runs, and A* got
  up to 30 % slower. Rounding keeps the estimate within one of the result, so
  one correction each way is exact on the whole range, and a test proves it.
- Stereo voices at a whole-number speed mix four frames at a time. LLVM 23
  stopped vectorizing the loop over frames at an unknown stride and it became
  30 % slower; the block form does not depend on that estimate and is a third
  faster than it was with LLVM 22.
- A region read out of a dense chunk copies eight edits at a time: edits are
  sorted, so one check of the eighth proves the block has no gap. A solid chunk
  is read 2.7 times faster, and a test compares runs of every length with
  single reads.
- A compound query no longer sorts hits that arrive in reverse order, which is
  how boxes laid along an axis come out of the tree. Such a query is three times
  faster; LLVM 23 had made it 30 % slower while making random ones much faster.
- Vulkan now runs from version 1.1 instead of 1.2, the version most phones
  from Android 9 on have. Nothing in the renderer needed 1.2, and such a phone
  was simply turned away. `LAIUE_GRAPHICS_BACKEND_VULKAN_CORE` holds a device
  to core 1.1 with no optional extension, so the path of an old phone runs on
  any device: the Vulkan tests and the Android scenario now go through it too.
  A real phone with Vulkan 1.1 has not been tried.
- The Vulkan depth format is chosen from what the device can draw into:
  32-bit float, then 24-bit, then 16-bit. It was always 32-bit float, which the
  Android baseline does not promise, so such a phone could not draw at all.
- CI runs the Vulkan tests on the Android baseline profiles of 2022 and 2025,
  with the validation layer checking the calls against what a phone has.
- `LAIUE_GRAPHICS_DIAGNOSTICS_DYNAMIC_RENDERING` tells whether a device draws
  with dynamic rendering, and the Android scenario counts such frames.
- Shader packs use contract 3. A frame drawn turned for an Android screen came
  with new constants for the interface and panorama vertex shaders, and a pack
  under contract 2 would have drawn them sideways on a rotated screen. Such a
  pack no longer loads: rebuild it with the two lines described in
  `docs/shaderpacks.md`. A pack that only replaces chunk shaders needs a new
  contract line and nothing more, since `viewProjection` already holds the turn.
- On Android a swap chain built during a turn of the screen takes the size of
  the window. The surface still reports its old size at that moment, so since
  the frame went straight into the chain a turn to portrait left a chain of the
  wrong shape: every frame was stretched into it with one more copy, and the
  emulator showed it one gamma step too dark. It stayed so until the next resize.
- On Android a frame is drawn already turned for a rotated screen, so the system
  no longer rotates every frame of a game held in landscape, which often means
  one more GPU pass over the screen. The camera, the interface, input and
  `readbackFrame` still work in window coordinates. Nothing changes on other
  platforms. The gain has not been measured on a phone.
- `LAIUE_GRAPHICS_DIAGNOSTICS_DRAWS_TURNED` tells whether the last frame was drawn
  turned, and the Android scenario counts such frames.
- Without dynamic rendering a change of scene pass drops the cached instanced
  constants, so the faces of a panorama no longer draw instances with the matrix
  of the previous face.
- On Vulkan the interface is drawn in the pass of the scene instead of a pass of
  its own, which a tile GPU pays for by writing the whole frame to memory and
  reading it back. Without dynamic rendering the pass of the scene no longer
  loads the last contents of its targets just to clear them.
- On Vulkan a frame is drawn straight into the swap chain image, without two
  extra colour targets and a copy of the whole screen in every frame. That copy
  costs a phone GPU the most, but a phone has not been measured. Foxity on Windows
  holds 232 MB instead of 241. A screen that offers only BGRA gets BGRA targets.
  The copy stays for a surface that offers neither sRGB format and for a frame
  whose size differs from the window. A chain asks for three images, since with
  two a frame and its GPU work have to fit one refresh.
- `LAIUE_GRAPHICS_DIAGNOSTICS_DRAWS_INTO_SWAPCHAIN` tells whether the last frame
  went straight into the swap chain, and the Android scenario counts such frames.
- `LAIUE_GRAPHICS_DIAGNOSTICS_READBACK_REQUEST_REQUIRED` covers every device with
  a window: on Vulkan too `readbackFrame` now needs `requestFrameReadback` before
  the frame. Without a window nothing changes.
- Upload rings take a quarter of the memory, 1 MiB per frame instead of 4, and
  the 8 MiB arenas of a burst go back after 120 turns of their frame slot without
  use. A game loads its world in the first seconds and then uploads almost
  nothing: Foxity peaked at 2.4 MiB a frame in 58 frames of 3600. Walk uses 11 MB
  less. `LaiueGraphicsDiagnosticsV2` reports them as `uploadStagingBytes`, so a
  program that reads diagnostics has to be rebuilt.
- On Vulkan depth is no longer written to memory after a pass, since nothing reads
  it there. A phone GPU keeps depth in its tile memory, so a frame stops paying for
  a full-screen depth write, and where the device offers lazily allocated memory
  the depth target takes no RAM at all.
- The viewer plays a GIF frame by frame instead of unpacking it into memory first,
  so a long GIF plays to its end. It used to stop at 256 frames without a word and
  start over. A 6 MB GIF of 120 frames at 1920 x 1080 no longer takes 1.1 GB.
  `GifStreamOpen` and `GifStreamNext` are the new decoder calls.
- On Vulkan with vertical sync a frame reaches the screen in 33 ms instead of 90
  to 98 ms from its start, in a window at 60 Hz, at the same frame rate and under
  load too. The renderer used to wait for the GPU at the start of a frame, after
  the program had read its input, and the present queue held four more frames.
  It now waits at the end of a frame, and where the device has
  `VK_KHR_present_wait` it waits until the previous frame is on the screen, which
  keeps two frames queued, as on D3D12. Without the extension, on most Android
  devices, the wait for a free image moves to the end of a frame as well.
- On Vulkan a swap chain is no longer rebuilt on every frame of a rotated screen.
  Android answers each present with `VK_SUBOPTIMAL_KHR` while the screen is turned
  and the chain draws unrotated, and the renderer stopped the GPU and built a new
  chain each time, in the landscape orientation games run in: 171 rebuilds in 171
  frames on the emulator, 0 now. A frame acquired as suboptimal is drawn instead
  of dropped. `swapchainRebuilds` in the graphics diagnostics counts them, which
  grows `LaiueGraphicsDiagnosticsV2`, so a program that reads it has to be rebuilt.
- A vertex buffer keeps no CPU copy: its bytes stay only in the GPU, so vertices
  take half the memory and each upload one copy less. Foxity uses 21 MB less. A
  vertex buffer now takes whole uploads only; index buffers still take parts.
- The viewer sends its font atlas to the GPU only when the font was baked again,
  not at every new window size. A window resize costs 13 % less CPU.
- Destroying a texture no longer stops the GPU either. A texture that frames in
  flight may read waits until the GPU has passed them; Vulkan used to wait for
  the device to go idle and rebuild its descriptor sets.
- A minimized window on Windows reports a size of 0 x 0, as programs expected,
  and they stop drawing to nowhere. The viewer sleeps until it is restored, and
  a game goes on simulating. A window without pixels sleeps between frames even
  when it stays active.
- A window keeps its frame rate out of focus while it can be seen. About 20
  frames a second now belong to `WINDOW_POWER_EFFICIENCY` and to windows nobody
  sees: hidden, or on another virtual desktop. On X11 a window the window manager
  took off the screen reports a size of 0 x 0, as on Windows.
- `WINDOW_POWER_AUTO` enters efficiency mode only when no window of the program
  can be seen. A window out of focus used to fall to idle priority, and any busy
  program could stop it completely.
- Geometry uploaded without frames no longer piles up. Past 32 MiB of spilled
  staging the renderers copy what waits and reuse the memory, as they already
  did for textures.
- Uploading a texture no longer stops the GPU. D3D12 waited for it twice per
  mip level and Vulkan once; the pixels now go to staging memory, and the copy
  runs at the start of the next frame. In the viewer a page turn holds the
  main thread for 17 to 24 ms instead of 33 to 50 ms.
- Windows draws engine windows pixel for pixel on a scaled display. The first
  window declares the process aware of each monitor's DPI, the requested size
  counts in points, and a window follows the scale of the monitor it moves to.
  Before, the system stretched the whole window, and the viewer's 100 % was not
  one image pixel per screen pixel.
- The viewer turns a page in 35 to 45 ms instead of 200 to 300 ms on
  24-megapixel pictures. Reading and decoding took three quarters of that and
  need no graphics device, so the neighbour in the direction of browsing is
  read on its own thread, mip chain included. This holds one more picture in
  memory, 122 MiB at this size.
- A program can survive a driver reset. `getDiagnostics` reports
  `LAIUE_GRAPHICS_DIAGNOSTICS_DEVICE_LOST` when every frame is going to fail,
  and a new device can be created on the same window. The viewer does this and
  shows its picture again. It also no longer spins on a whole core while frames
  fail.
- On D3D12 a frame reaches the screen 33 ms sooner at 60 Hz: 50 ms instead of
  83 ms from the start of a frame in a window, 33 ms instead of 66 ms in
  borderless full screen. The present queue holds two frames instead of three,
  with the same frame rate under load, and without vertical sync a GPU-bound
  program gains too: 27 ms instead of 32 ms at 218 frames per second.
- A program can sleep while nothing changes, instead of drawing the same
  picture at the refresh rate. `WindowDeferNextFrame`, called from a frame,
  holds the next one until a window event, a deadline or `WindowWakeFrame`
  from any thread, and `WindowConsumeRedraw` reports when the system lost the
  window's picture. Without these calls frames run back to back, as before.
  The viewer uses them: a picture lying on the screen costs no CPU or GPU
  time at all, down from 2.5 % of a core and 7.3 % of the GPU, and an animated
  GIF draws only when its frame changes.
- JPEG decodes twice as fast on one core: a 24-megapixel 4:2:0 photo takes
  75 ms instead of 157 ms, a 4:4:4 one 91 ms instead of 183 ms, and a
  progressive one 216 ms instead of 298 ms. Valid files still match
  libjpeg-turbo byte for byte.
  - The inverse transform, chroma upsampling and color conversion run in
    SSE2 and NEON registers. A new test compares them with a scalar
    reference on every sampling and on widths around the vector steps.
  - Bits stay in registers for a whole block, and one table entry gives
    the length of a code together with the value behind it.
  - Values too large for a 16-bit lane now saturate the same way on every
    processor, which changes only the garbage from damaged files.
- The viewer starts sooner on a large picture: from start to exit with
  `--smoke`, a 24-megapixel JPEG takes 393 ms instead of 537 ms and a PNG of
  that size 379 ms instead of 497 ms. The first picture is read on its own
  thread while the window and the graphics device are created, which takes
  longer than decoding a photo.
- PNG decodes 2.3 to 2.5 times faster on one core: a 24-megapixel photo
  takes 119 ms instead of 276 ms, the same photo with Paeth on every row
  175 ms instead of 398 ms, and a noisy one that barely compresses 241 ms
  instead of 576 ms.
  - Inflate runs a separate loop in the middle of a block. It checks the
    input and output once a step instead of once a symbol and reads 64
    bits at a time, and one table entry gives a code's length, extra bits
    and base. Anything unusual stays with the careful decoder, so damaged
    streams end with the same status at the same byte.
  - Filters are undone a pixel at a time in SSE2 and NEON registers. Paeth
    has no branches: on a photo the winner changes from byte to byte, and
    mispredictions cost more than the predictor itself.
  - Adler-32 takes 16 bytes at a time, and RGB expands a word at a time.
  - New tests reach the loop, which short streams never did, and undo
    every filter on every pixel size. While making the change, the old and
    new code were compared under ASan and UBSan on 34 million mutated
    streams and files: status, count and bytes all matched.
- Halving an opaque picture, the step of every mip chain, is four times
  faster: 10 ms instead of 38 ms for all the levels of a 24-megapixel
  photo. Equal weights turn the weighted average into a plain one, which
  the compiler vectorizes.
- JPEG decodes 1.4 to 3.2 times faster, and byte for byte like
  libjpeg-turbo with its default settings. A 24-megapixel 4:2:0 photo
  takes 157 ms instead of 408 ms on one core, a progressive one 306 ms
  instead of 560 ms.
  - The inverse DCT is libjpeg's integer `islow` instead of a direct
    float transform. Integer arithmetic also gives the same pixels on
    every processor and compiler.
  - Subsampled chroma is restored a row at a time with libjpeg's triangle
    filter and its alternating rounding, instead of per pixel.
  - The bit reader refills 64 bits at a time, and coefficients and
    quantization tables are kept in natural order, as the transform reads
    them.
  - The texture converter test now requires the exact pixels Pillow
    decodes, and 192 more files were checked against Pillow while making
    the change: every subsampling, sizes from 1×1, progressive, restart
    markers and qualities from 1 to 100.
- On Windows frames go on while the window is dragged or resized, while a
  mouse button holds the caption or a caption button and while the window
  menu is open. `DefWindowProc` runs its own message loop until the button
  is released, and `WindowRunLoop` stood still.
  - Moving, sizing and the menu now run frames from a timer. `WM_TIMER`
    comes only when the queue is empty, so the loop keeps the mouse.
  - A held caption button and a press on the caption before the first mouse
    move use a small frame clock thread instead. No timer gets through
    there: those waits take only mouse messages, or none. The thread wakes
    the window procedure for each frame with `SendNotifyMessage`. After
    each frame it pauses for a millisecond: Windows hands the window
    thread sent messages before input, and without the pause the loop
    would not get the mouse while it moves.
  - Input is reset when the system takes the mouse, as on focus loss. Raw
    input reaches the program again only when the system gives the mouse
    back, so a press on the caption no longer becomes a click in the
    program.
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
