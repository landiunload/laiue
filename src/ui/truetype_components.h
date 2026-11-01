#pragma once

#include "truetype_reader.h"

enum
{
    TRUE_TYPE_COMPOSITE_MAX_COMPONENTS = 1024,
    TRUE_TYPE_COMPONENT_WORD_ARGUMENTS = 0x0001,
    TRUE_TYPE_COMPONENT_XY_ARGUMENTS = 0x0002,
    TRUE_TYPE_COMPONENT_ROUND_XY = 0x0004,
    TRUE_TYPE_COMPONENT_SCALE = 0x0008,
    TRUE_TYPE_COMPONENT_MORE = 0x0020,
    TRUE_TYPE_COMPONENT_XY_SCALE = 0x0040,
    TRUE_TYPE_COMPONENT_MATRIX = 0x0080,
    TRUE_TYPE_COMPONENT_INSTRUCTIONS = 0x0100,
    TRUE_TYPE_COMPONENT_USE_METRICS = 0x0200,
    TRUE_TYPE_COMPONENT_OVERLAP = 0x0400,
    TRUE_TYPE_COMPONENT_SCALED_OFFSET = 0x0800,
    TRUE_TYPE_COMPONENT_UNSCALED_OFFSET = 0x1000,
};

typedef struct TrueTypeComponent
{
    uint16_t glyphIndex;
    uint16_t flags;
    int32_t argument1;
    int32_t argument2;
    // Raw signed F2Dot14: 16384 means 1.0. x' = xScale*x + scale10*y;
    // y' = scale01*x + yScale*y. No transform is applied by this reader.
    int16_t xScale;
    int16_t scale01;
    int16_t scale10;
    int16_t yScale;
} TrueTypeComponent;

typedef struct TrueTypeComponentStorage
{
    TrueTypeComponent *components;
    uint32_t capacity;
} TrueTypeComponentStorage;

typedef struct TrueTypeCompositeInfo
{
    uint32_t componentCount;
    TrueTypeSpan instructions;
} TrueTypeCompositeInfo;

// Внутренние records composite glyf, без обхода glyph graph и выполнения hints.
// glyph должен иметь отрицательный numberOfContours и хотя бы один компонент.
// Пустой span и простой glyph возвращают false.
// Arguments: signed XY либо unsigned point numbers в int32, по flags.
// Glyph IDs и point references проверяет resolver относительно font/outline.
// Противоречивые transform или scaled/unscaled offset flags дают false.
// До записи проверяется вся раскладка; false сохраняет storage и out.
// glyph, storage и out не перекрываются, glyph неизменен во время вызова;
// instructions заимствует память glyph и живёт не дольше исходного буфера.
bool TrueTypeCompositeRead(TrueTypeSpan glyph, TrueTypeComponentStorage storage,
                           TrueTypeCompositeInfo *out);
