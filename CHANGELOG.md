# Changelog

Versions follow [Semantic Versioning](https://semver.org). Before 1.0 the
API and ABI may change between minor versions.

## Unreleased

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
