# Image viewer

`laiue_viewer` shows PNG, JPEG and GIF pictures, animated GIFs included. One
C file with an ordinary `main` runs unchanged on Windows, Linux and Android:
the window, input, graphics and UI are engine modules that the program loads
by name, so it has no platform code of its own.

```text
laiue_viewer [directory or file] [--smoke]
```

Without an argument it lists the current directory, and on Android the app's
shared files directory. A file argument opens its directory at that picture.
An empty directory shows a test pattern.

| Keys | Action |
|---|---|
| ←/→, PgUp/PgDn, Space/Backspace | previous and next picture |
| Home/End | first and last picture |
| wheel, `+`/`−` | zoom |
| `0` / `1` | fit the window / one to one |
| `R` | rotate by 90° |
| `F`, F11 | full screen |
| Esc | quit |

Dragging pans the picture. On a touch screen, a tap near the left or right
edge turns the page, a tap in the middle hides the caption, a double tap
switches between fit and one to one, and two fingers pan and zoom.

## Modules

The program asks for `window`, `input`, `ui` and one graphics provider: on
Windows `graphics_d3d12` first, then `graphics_vulkan`, elsewhere
`graphics_vulkan`. Each missing module turns off only its own technology.
Without `laiue_graphics_d3d12.dll` Windows draws through Vulkan, and without
the UI module the picture shows with no caption. With no graphics provider
at all the program reports it and exits.

## Build

Desktop builds need modules as shared libraries, the default:

```powershell
cmake --preset windows-msvc -DLAIUE_BUILD_EXAMPLES=ON
cmake --build --preset windows-msvc-release --target laiue_viewer
```

On Android every module is its own `.so` inside the APK. Point
`ANDROID_NDK_HOME` at an NDK and give a keystore to sign with:

```powershell
cmake --preset android-arm64-shared -DLAIUE_ANDROID_KEYSTORE=<keystore>
cmake --build --preset android-arm64-shared-release --target laiue_viewer_apk
adb install -r build/android-arm64-shared/android/laiue_viewer_apk.apk
adb push picture.png /sdcard/Android/data/dev.laiue.viewer/files/
```

Arguments reach `main` through the intent:
`adb shell am start -n dev.laiue.viewer/android.app.NativeActivity --es args --smoke`.
The program's output goes to logcat under the package name.

The whole build description is one call; see [CMakeLists.txt](CMakeLists.txt):

```cmake
laiue_add_program(laiue_viewer viewer.c
    MODULES window input ui graphics_d3d12 graphics_vulkan
    WINDOWED
    ANDROID_PACKAGE dev.laiue.viewer
    ANDROID_LABEL "laiue viewer")
```

`--smoke` reads the third frame back from the GPU, checks that the picture
differs from the background and exits; CI runs it in the Android emulator.
