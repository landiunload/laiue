#pragma once

// Макросы экспорта/импорта: внутри своей DLL/.so символы экспортируются,
// для всех остальных модулей — импортируются. На ELF сборка использует
// hidden visibility и открывает только публичный ABI.

#if defined(LAIUE_STATIC)
#define LAIUE_EXPORT
#define LAIUE_IMPORT
#elif defined(_WIN32)
#define LAIUE_EXPORT __declspec(dllexport)
#define LAIUE_IMPORT __declspec(dllimport)
#else
#define LAIUE_EXPORT __attribute__((visibility("default")))
#define LAIUE_IMPORT __attribute__((visibility("default")))
#endif

#if defined(LAIUE_BUILD_WINDOW)
#define LAIUE_WINDOW_API LAIUE_EXPORT
#else
#define LAIUE_WINDOW_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_INPUT)
#define LAIUE_INPUT_API LAIUE_EXPORT
#else
#define LAIUE_INPUT_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_AUDIO)
#define LAIUE_AUDIO_API LAIUE_EXPORT
#else
#define LAIUE_AUDIO_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_SCENE)
#define LAIUE_SCENE_API LAIUE_EXPORT
#else
#define LAIUE_SCENE_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_UI)
#define LAIUE_UI_API LAIUE_EXPORT
#else
#define LAIUE_UI_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_WORLD)
#define LAIUE_WORLD_API LAIUE_EXPORT
#else
#define LAIUE_WORLD_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_MESHER)
#define LAIUE_MESHER_API LAIUE_EXPORT
#else
#define LAIUE_MESHER_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_RENDER)
#define LAIUE_RENDER_API LAIUE_EXPORT
#else
#define LAIUE_RENDER_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_PHYSICS)
#define LAIUE_PHYSICS_API LAIUE_EXPORT
#else
#define LAIUE_PHYSICS_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_CONTENT)
#define LAIUE_CONTENT_API LAIUE_EXPORT
#else
#define LAIUE_CONTENT_API LAIUE_IMPORT
#endif

#if defined(LAIUE_BUILD_MOD)
#define LAIUE_MOD_API LAIUE_EXPORT
#else
#define LAIUE_MOD_API LAIUE_IMPORT
#endif
