# `examples/walk`

This is the smallest public-SDK walk slice. It owns the game-specific base
provider (grass at `z=0`, earth at `z=-1..-3`, stone below), loads the public
`laiue.character`, `laiue.voxel`, `laiue.scene`, and `laiue.graphics` service
tables through the bootstrap, keeps authoritative coordinates as integer cell
plus local offsets, and advances a kinematic character at 128 Hz. Rendering is
camera-relative; mouse-look, WASD, sprint, and jump are enabled when the
corresponding optional platform providers are present.

The character provider is optional. Without it the window and diagnostic
surface remain available, but no character is advanced. If the voxel module
is missing or cannot create its sparse world, the sample stays usable and
falls back to the game-owned infinite grass/earth/stone strata, reporting the
degraded mode at startup. Static/mobile profiles can set
`LAIUE_WALK_STATIC_WITH_VOXEL=OFF` to omit the voxel artifact at link time;
the same fallback is then used without changing the application code.

Build it with `-DLAIUE_BUILD_EXAMPLES=ON`. On Windows graphics builds the
same executable starts a small windowed session by default: WASD moves,
Shift sprints, Space jumps, and Escape closes it. `--headless` is retained for
CI and validates the no-window/no-render bootstrap profile. Window, input and
graphics are loaded as optional providers; if an artifact is absent or the
device cannot be created, the example reports the reason and runs the same
diagnostic headless check instead.

## Android NativeActivity

`android/` is a small static-registry client using the same module ABI. It
creates a Vulkan surface from `ANativeWindow`, recreates the device after
`APP_CMD_TERM_WINDOW`/`APP_CMD_INIT_WINDOW`, pauses its fixed 128 Hz loop when
the activity loses focus, and accepts keyboard plus touch input. Vulkan 1.3
uses dynamic rendering; Vulkan 1.2 devices use the compatible render-pass
path, so the same APK does not require a 1.3-only device. The sparse
voxel module is optional: disabling `LAIUE_ANDROID_WALK_WITH_VOXEL` keeps the
character on the example's deterministic infinite base plane.

Configure it with the API 35 emulator preset (put the build tree on the
scratch disk as shown below):

```powershell
$env:ANDROID_NDK_HOME = 'D:\Android\Sdk\ndk\29.0.14206865'
cmake --preset android-x86_64-walk-api35 -B D:\build\laiue\android-x86_64-walk
cmake --build D:\build\laiue\android-x86_64-walk --config Release --target laiue_walk_android
```

The result is `android/Release/liblaiue_walk.so` plus the manifest staged by
the `laiue_walk_android_manifest` target. APK signing/zipalign intentionally
remain a packaging step. If `ANDROID_SDK_ROOT`/`ANDROID_HOME` points at an SDK
with build-tools, CMake discovers `aapt2`, `zipalign`, `apksigner` and the
highest installed `android.jar`; set `LAIUE_ANDROID_KEYSTORE` to enable the
`laiue_walk_android_apk` target. A keystore is still supplied by the
application owner, never generated or embedded by the engine.
