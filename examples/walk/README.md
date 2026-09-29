# `examples/walk`

This is the smallest public-SDK walk slice. It owns the game-specific base
provider (grass at `z=0`, earth at `z=-1..-3`, stone below), loads the public
`laiue.character`, `laiue.physics`, `laiue.voxel`, `laiue.scene`, and
`laiue.graphics` service tables through the bootstrap, and advances simulation
at 128 Hz. Desktop and Android run the same 13-part active humanoid ragdoll
with pelvis, torso, head, upper/lower arms, thighs, shins, and feet. Rendering
is camera-relative; mouse-look, WASD, sprint, and jump are
enabled when the corresponding optional platform providers are present.

The character provider is optional. Without it the window and diagnostic
surface remain available, but no character is advanced. If the voxel module
is missing or cannot create its sparse world, the sample stays usable and
falls back to the game-owned infinite grass/earth/stone strata, reporting the
degraded mode at startup. Static/mobile profiles can set
`LAIUE_WALK_STATIC_WITH_VOXEL=OFF` to omit the voxel artifact at link time;
the same fallback is then used without changing the application code.

Build it with `-DLAIUE_BUILD_EXAMPLES=ON`. On Windows graphics builds the
same executable starts a small windowed session by default: WASD moves,
Shift sprints, Space jumps, mouse-look rotates the camera, `V` switches between
first- and third-person views, left-click breaks a block, right-click places
one, and `1`–`3` selects the material. Escape closes the game. `--headless` is retained for
CI and validates the no-window/no-render bootstrap profile. Window, input and
graphics are loaded as optional providers; if an artifact is absent or the
device cannot be created, the example reports the reason and runs the same
diagnostic headless check instead.

For shared desktop builds the profile names one backend artifact explicitly:
`laiue_graphics_d3d12.dll` for a D3D12 configure or
`laiue_graphics_vulkan.dll` for Vulkan (with the corresponding `.so`/`.dylib`
name on POSIX). The legacy `laiue_render` bundle is only used when standalone
provider targets are disabled; it is not an implicit dependency of the normal
desktop walk executable.

## Android NativeActivity

`android/` is a small static-registry client using the same module ABI. It
creates a Vulkan surface from `ANativeWindow`, recreates the device after
`APP_CMD_TERM_WINDOW`/`APP_CMD_INIT_WINDOW`, pauses its fixed 128 Hz loop when
the activity loses focus, and accepts keyboard plus touch input. On a phone,
the lower-left virtual stick moves relative to the camera with circular travel
and analog speed, the lower-right orange button toggles sprint, the purple
button jumps, and dragging the rest of the right half looks around. Touch state
is cleared on pause, surface recreation, and resize. Vulkan 1.3 uses dynamic
rendering; Vulkan 1.2 devices use the compatible render-pass
path, so the same APK does not require a 1.3-only device. The Android manifest
declares Vulkan 1.2 as the minimum supported API. The sparse
voxel module is optional: disabling `LAIUE_ANDROID_WALK_WITH_VOXEL` keeps the
ragdoll on the example's deterministic infinite base plane. On Android, the
lower-left stick drives the active ragdoll relative to the camera, the orange
button toggles its speed, and the purple button jumps only while a foot is
supported by a solid surface. The 13 colliding body boxes have rounded render
meshes, two physical feet, and 12 ball joints. A fixed-step pose controller
stabilizes the torso and limbs, targets an alternating leg stride, and keeps
body parts near their anatomical offsets. Directly joined parts skip
self-collision while all non-adjacent body and world contacts remain active.
The controller and contact solver are deterministic; a large external impact
can still knock the character down because the demo has no get-up animation.

Configure it with the API 35 emulator preset (put the build tree on the
scratch disk as shown below):

```powershell
$env:ANDROID_NDK_HOME = 'D:\Android\Sdk\ndk\29.0.14206865'
cmake --preset android-x86_64-walk-api35 -B D:\build\laiue\android-x86_64-walk
cmake --build D:\build\laiue\android-x86_64-walk --config Release --target laiue_walk_android_apk
```

The native target produces `android/Release/liblaiue_walk.so` and stages the
manifest. The `laiue_walk_android_apk` target also embeds the example textures,
aligns the package, and signs the APK with the supplied keystore. If
`ANDROID_SDK_ROOT`/`ANDROID_HOME` points at an SDK with build-tools, CMake
discovers `aapt2`, `zipalign`, `apksigner` and the highest installed
`android.jar`; set `LAIUE_ANDROID_KEYSTORE` to enable APK packaging. The Android
build-tools signer needs a Java runtime on `PATH` (for example Android Studio's
bundled JBR). The keystore remains application-owned and is never generated
or embedded by the engine.
