#pragma once

#include "api.h"
#include "render/chunk_geometry.h"

#include <stdbool.h>
#include <stdint.h>

// Рендерер рисует абстрактные меши-квады и ничего не знает об устройстве
// мира: построение геометрии из вокселей — обязанность модуля mesher.
typedef struct Renderer Renderer;

// GPU-резидентный меш: квады один раз копируются в общий DEFAULT-буфер
// (суб-аллокация, без 64-КиБ ресурса на меш) и рисуются vertex pulling'ом
// без вершинных и индексных буферов.
typedef struct RendererMesh RendererMesh;

LAIUE_RENDER_API Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height);
LAIUE_RENDER_API void      RendererDestroy(Renderer* renderer);

// Начало кадра: применяет отложенный resize, записывает загрузки мешей,
// ставит матрицу view-projection. Возвращает false, если кадр рисовать
// нельзя (например, resize не удался) — тогда Draw/EndFrame пропускаются.
LAIUE_RENDER_API bool RendererBeginFrame(Renderer* renderer, const float viewProjection[16]);
LAIUE_RENDER_API void RendererEndFrame(Renderer* renderer);

// Создание меша: квады ставятся в очередь загрузки на GPU (выполняется
// в ближайшем RendererBeginFrame). Вызывать до начала кадра.
// NULL при нехватке памяти или пустой геометрии — вызывающая сторона
// может повторить попытку в следующем кадре.
LAIUE_RENDER_API RendererMesh* RendererCreateMesh(Renderer* renderer,
    const ChunkQuad* quads, uint32_t quadCount);

// Удаление меша безопасно в любой момент: диапазон пула освобождается
// отложенно, когда GPU гарантированно закончил кадры, читавшие его.
LAIUE_RENDER_API void RendererDestroyMesh(Renderer* renderer, RendererMesh* mesh);

// Отрисовка меша: смещение чанка относительно начала координат рендера
// (origin rebasing — камера всегда около нуля) и низшие 32 бита мировых
// блочных координат угла чанка (для процедурного цвета в шейдере).
LAIUE_RENDER_API void RendererDrawMesh(Renderer* renderer, const RendererMesh* mesh,
    const float chunkOriginRelative[3], const uint32_t chunkBaseLow[3]);

LAIUE_RENDER_API void RendererResize(Renderer* renderer, int32_t width, int32_t height);
