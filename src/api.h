#pragma once

// Макросы экспорта/импорта: внутри своей DLL символы экспортируются,
// для всех остальных модулей — импортируются.

#if defined(LAIUE_BUILD_WINDOW)
#define LAIUE_WINDOW_API __declspec(dllexport)
#else
#define LAIUE_WINDOW_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_INPUT)
#define LAIUE_INPUT_API __declspec(dllexport)
#else
#define LAIUE_INPUT_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_CORE)
#define LAIUE_CORE_API __declspec(dllexport)
#else
#define LAIUE_CORE_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_WORLD)
#define LAIUE_WORLD_API __declspec(dllexport)
#else
#define LAIUE_WORLD_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_MESHER)
#define LAIUE_MESHER_API __declspec(dllexport)
#else
#define LAIUE_MESHER_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_RENDER)
#define LAIUE_RENDER_API __declspec(dllexport)
#else
#define LAIUE_RENDER_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_PHYSICS)
#define LAIUE_PHYSICS_API __declspec(dllexport)
#else
#define LAIUE_PHYSICS_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_GAMEPLAY)
#define LAIUE_GAMEPLAY_API __declspec(dllexport)
#else
#define LAIUE_GAMEPLAY_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_INTERACTION)
#define LAIUE_INTERACTION_API __declspec(dllexport)
#else
#define LAIUE_INTERACTION_API __declspec(dllimport)
#endif

#if defined(LAIUE_BUILD_CONTENT)
#define LAIUE_CONTENT_API __declspec(dllexport)
#else
#define LAIUE_CONTENT_API __declspec(dllimport)
#endif
