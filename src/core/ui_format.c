#include "core/ui_format.h"

static void Terminate(UiTextBuilder* builder)
{
    if (builder->destination != NULL && builder->capacity > 0)
    {
        uint32_t index = builder->length < builder->capacity
            ? builder->length : builder->capacity - 1u;
        builder->destination[index] = L'\0';
    }
}

void UiTextBuilderInit(UiTextBuilder* builder,
    wchar_t* destination, uint32_t capacity)
{
    builder->destination = destination;
    builder->capacity = capacity;
    builder->length = 0;
    Terminate(builder);
}

void UiTextBuilderAppendChar(UiTextBuilder* builder, wchar_t character)
{
    if (builder->destination == NULL || builder->capacity == 0)
    {
        return;
    }
    if (builder->length + 1u < builder->capacity)
    {
        builder->destination[builder->length++] = character;
    }
    Terminate(builder);
}

void UiTextBuilderAppend(UiTextBuilder* builder, const wchar_t* text)
{
    if (text == NULL)
    {
        return;
    }
    while (*text != L'\0')
    {
        UiTextBuilderAppendChar(builder, *text++);
    }
}

void UiTextBuilderAppendUnsigned(UiTextBuilder* builder, uint64_t value)
{
    wchar_t reversedDigits[20];
    uint32_t digitCount = 0;
    do
    {
        reversedDigits[digitCount++] =
            (wchar_t)(L'0' + (wchar_t)(value % 10u));
        value /= 10u;
    }
    while (value != 0u && digitCount < 20u);

    while (digitCount > 0)
    {
        UiTextBuilderAppendChar(builder, reversedDigits[--digitCount]);
    }
}

void UiFormatUnsigned(wchar_t* destination, uint32_t capacity, uint64_t value)
{
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, destination, capacity);
    UiTextBuilderAppendUnsigned(&builder, value);
}

void UiFormatUnsignedSuffix(wchar_t* destination, uint32_t capacity,
    uint64_t value, const wchar_t* suffix)
{
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, destination, capacity);
    UiTextBuilderAppendUnsigned(&builder, value);
    UiTextBuilderAppend(&builder, suffix);
}

void UiFormatDegrees(wchar_t* destination, uint32_t capacity, int32_t value)
{
    UiTextBuilder builder;
    UiTextBuilderInit(&builder, destination, capacity);
    UiTextBuilderAppendUnsigned(&builder,
        value > 0 ? (uint32_t)value : 1u);
    UiTextBuilderAppendChar(&builder, 0x00B0);
}

void UiFormatClock(wchar_t* destination, uint32_t capacity,
    uint32_t timeMinutes)
{
    uint32_t hours = (timeMinutes / 60u) % 24u;
    uint32_t minutes = timeMinutes % 60u;

    UiTextBuilder builder;
    UiTextBuilderInit(&builder, destination, capacity);
    UiTextBuilderAppendChar(&builder, (wchar_t)(L'0' + hours / 10u));
    UiTextBuilderAppendChar(&builder, (wchar_t)(L'0' + hours % 10u));
    UiTextBuilderAppendChar(&builder, L':');
    UiTextBuilderAppendChar(&builder, (wchar_t)(L'0' + minutes / 10u));
    UiTextBuilderAppendChar(&builder, (wchar_t)(L'0' + minutes % 10u));
}
