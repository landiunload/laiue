#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/texture_pack.h"

typedef enum TexturePackLoadStatus
{
    TEXTURE_PACK_LOAD_NOT_ATTEMPTED = 0,
    TEXTURE_PACK_LOAD_OK,
    TEXTURE_PACK_LOAD_NO_ACTIVE_PACK,
    TEXTURE_PACK_LOAD_INVALID,
    TEXTURE_PACK_LOAD_IO_ERROR,
    // Пак загружен, но части материалов в нём нет: они показаны
    // нейтральным слоем. Это состояние приложения, а не отказ.
    TEXTURE_PACK_LOAD_INCOMPLETE,
} TexturePackLoadStatus;

// LTP хранит по слою на кадр материала. Material id 1 в меше берёт
// описание 0; лишние id зажимаются к последнему материалу.
#define TEXTURE_PACK_MAX_MIP_COUNT 13u
#define TEXTURE_PACK_MAX_SUBRESOURCES (TEXTURE_PACK_MAX_SLICES * TEXTURE_PACK_MAX_MIP_COUNT)

// Расписание одного материала. Неанимированный материал — это кадр
// длиной ноль: отдельного признака для него не нужно.
typedef struct TexturePackAnimation
{
    uint16_t firstSlice;
    uint16_t frameCount;
    // Сумма длительностей кадров материала: длина одного цикла.
    uint32_t cycleMilliseconds;
} TexturePackAnimation;

typedef struct TexturePackData
{
    uint16_t width;
    uint16_t height;
    uint16_t sliceCount;
    uint16_t mipCount;
    uint16_t materialCount;
    const uint8_t *pixels;
    uint32_t pixelBytes;
    const uint8_t *normalPixels;
    void *allocation;
    TexturePackAnimation animation[TEXTURE_PACK_MAX_LAYERS];
    // Длительность каждого слоя. Слоёв в паке не больше 256, поэтому
    // общая таблица дешевле, чем массив длительностей на материал.
    uint16_t sliceMilliseconds[TEXTURE_PACK_MAX_SLICES];
} TexturePackData;

// Расписание пака, отделённое от его пикселей: рендерер хранит его
// дальше, а сам пак освобождает сразу после загрузки в GPU.
typedef struct TexturePackAnimationSet
{
    uint32_t sliceCount;
    uint32_t materialCount;
    TexturePackAnimation animation[TEXTURE_PACK_MAX_LAYERS];
    uint16_t sliceMilliseconds[TEXTURE_PACK_MAX_SLICES];
} TexturePackAnimationSet;

// Имена материалов, которые приложение просит у пака. Хранение
// вынесено сюда, чтобы оба бэкенда рендера держали одно и то же, а не
// каждый своё.
typedef struct TexturePackMaterialNames
{
    wchar_t storage[TEXTURE_PACK_MAX_LAYERS][LAIUE_CONTENT_NAME_CAPACITY];
    const wchar_t *pointers[TEXTURE_PACK_MAX_LAYERS];
    uint32_t count;
} TexturePackMaterialNames;

bool TexturePackMaterialNamesSet(TexturePackMaterialNames *names, const wchar_t *const *source,
                                 uint32_t count);

typedef struct TexturePackSubresource
{
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t rowBytes;
    uint32_t byteCount;
} TexturePackSubresource;

// Собирает массив из активного пака по именам материалов приложения.
// Имя вправе содержать '/', и файл ищется по расширениям `.lt`, `.png`
// и `.gif`. Материал, которого в паке нет, получает нейтральный слой, а
// статус становится INCOMPLETE.
TexturePackLoadStatus TexturePackLoadActiveFrom(LaiueContentCatalog *catalog,
                                                const wchar_t *const *materialNames,
                                                uint32_t materialCount, TexturePackData *outPack);
TexturePackLoadStatus TexturePackBuildFrom(LaiueContentCatalog *catalog,
                                           const wchar_t *const *materialNames,
                                           uint32_t materialCount, TexturePackData *outPack);
bool TexturePackGetSubresource(const TexturePackData *pack, uint32_t slice, uint32_t mip,
                               TexturePackSubresource *outSubresource);
bool TexturePackGetNormalSubresource(const TexturePackData *pack, uint32_t slice, uint32_t mip,
                                     TexturePackSubresource *outSubresource);

// Слой, который материал показывает в заданный момент. Время берётся у
// приложения и может идти как угодно; отрицательное считается нулём.
uint32_t TexturePackResolveSlice(const TexturePackAnimationSet *set, uint32_t material,
                                 double animationSeconds);

void TexturePackCaptureAnimation(TexturePackAnimationSet *outSet, const TexturePackData *pack);

// Готовая для шейдера таблица: по байту слоя на материал, 64 материала в
// шестнадцати словах. Материалы сверх materialCount повторяют последний,
// чтобы некорректный меш не читал чужой слой.
void TexturePackFillSliceTable(const TexturePackAnimationSet *set,
                               double animationSeconds, uint32_t *outSlices);
void TexturePackRelease(TexturePackData *pack);
