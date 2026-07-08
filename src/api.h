#ifndef LAIUE_API_H
#define LAIUE_API_H

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

#endif
