#pragma once

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

void UiTextBuilderInit(UiTextBuilder* builder,
    wchar_t* destination, uint32_t capacity);
void UiTextBuilderAppend(UiTextBuilder* builder, const wchar_t* text);
void UiTextBuilderAppendChar(UiTextBuilder* builder, wchar_t character);
void UiTextBuilderAppendUnsigned(UiTextBuilder* builder, uint64_t value);

void UiFormatUnsigned(wchar_t* destination, uint32_t capacity, uint64_t value);
void UiFormatUnsignedSuffix(wchar_t* destination, uint32_t capacity,
    uint64_t value, const wchar_t* suffix);
void UiFormatDegrees(wchar_t* destination, uint32_t capacity, int32_t value);
void UiFormatClock(wchar_t* destination, uint32_t capacity,
    uint32_t timeMinutes);
