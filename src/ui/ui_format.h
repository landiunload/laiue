#pragma once

#include "api.h"

#include <stdint.h>
#include <wchar.h>

// Небольшой безопасный builder для UI-строк без CRT.
// Всегда поддерживает нулевой терминатор при capacity > 0.
typedef struct UiTextBuilder
{
    wchar_t* destination;
    uint32_t capacity;
    uint32_t length;
} UiTextBuilder;

LAIUE_UI_API void UiTextBuilderInit(UiTextBuilder* builder,
    wchar_t* destination, uint32_t capacity);
LAIUE_UI_API void UiTextBuilderAppend(
    UiTextBuilder* builder, const wchar_t* text);
LAIUE_UI_API void UiTextBuilderAppendChar(
    UiTextBuilder* builder, wchar_t character);
LAIUE_UI_API void UiTextBuilderAppendUnsigned(
    UiTextBuilder* builder, uint64_t value);

LAIUE_UI_API void UiFormatUnsigned(
    wchar_t* destination, uint32_t capacity, uint64_t value);
LAIUE_UI_API void UiFormatUnsignedSuffix(
    wchar_t* destination, uint32_t capacity,
    uint64_t value, const wchar_t* suffix);
LAIUE_UI_API void UiFormatDegrees(
    wchar_t* destination, uint32_t capacity, int32_t value);
LAIUE_UI_API void UiFormatClock(
    wchar_t* destination, uint32_t capacity,
    uint32_t timeMinutes);
