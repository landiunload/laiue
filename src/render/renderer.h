#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Рендерер рисует абстрактные меши и ничего не знает об устройстве мира:
// построение геометрии из вокселей — обязанность модуля mesher.
typedef struct Renderer Renderer;

// GPU-резидентный меш: геометрия загружается один раз при создании
// и рисуется без копирования каждый кадр.
typedef struct RendererMesh RendererMesh;

// Упакованная вершина: локальная позиция в чанке (по 7 бит на ось, 0..64)
// и номер грани (3 бита). Цвет и нормаль восстанавливаются
// в пиксельном шейдере из мировой позиции и номера грани.
typedef struct ChunkVertex
{
    uint32_t packedData;
} ChunkVertex;

LAIUE_RENDER_API Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height);
LAIUE_RENDER_API void      RendererDestroy(Renderer* renderer);

LAIUE_RENDER_API void RendererBeginFrame(Renderer* renderer);
LAIUE_RENDER_API void RendererEndFrame(Renderer* renderer);

LAIUE_RENDER_API void RendererSetViewProjection(Renderer* renderer, const float matrix[16]);

// Создание меша: геометрия копируется в GPU-буфер один раз.
// Возвращает NULL при пустой геометрии или нехватке памяти.
LAIUE_RENDER_API RendererMesh* RendererCreateMesh(Renderer* renderer,
    const ChunkVertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount);

// Удаление меша безопасно в любой момент: ресурс освобождается отложенно,
// когда GPU гарантированно закончил кадры, которые могли его читать.
LAIUE_RENDER_API void RendererDestroyMesh(Renderer* renderer, RendererMesh* mesh);

// Отрисовка меша со смещением чанка (мировая позиция угла чанка).
LAIUE_RENDER_API void RendererDrawMesh(Renderer* renderer, const RendererMesh* mesh, const float chunkOrigin[3]);

LAIUE_RENDER_API void RendererResize(Renderer* renderer, int32_t width, int32_t height);
