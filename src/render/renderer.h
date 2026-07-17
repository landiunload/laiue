#pragma once

#include "api.h"
#include "render/chunk_geometry.h"

#include <stdbool.h>
#include <stdint.h>

// Рендерер рисует абстрактные меши-квады и ничего не знает об устройстве
// мира: построение геометрии из вокселей — обязанность модуля mesher.
typedef struct Renderer Renderer;

typedef struct RendererStats
{
    uint64_t drawCalls;
    uint64_t drawnQuads;
    uint64_t uploadedBytes;
    uint64_t geometryPoolUsedBytes;
    uint64_t geometryPoolCapacityBytes;
    uint32_t scenePasses;
} RendererStats;

typedef enum RendererContentStatus
{
    RENDERER_CONTENT_NOT_ATTEMPTED = 0,
    RENDERER_CONTENT_OK,
    RENDERER_CONTENT_NO_ACTIVE,
    RENDERER_CONTENT_INVALID,
    RENDERER_CONTENT_IO_ERROR,
    RENDERER_CONTENT_GPU_ERROR,
    RENDERER_CONTENT_ACTIVATION_ERROR,
} RendererContentStatus;

// GPU-резидентный меш: квады один раз копируются в общий DEFAULT-буфер
// (суб-аллокация, без 64-КиБ ресурса на меш) и рисуются vertex pulling'ом
// без вершинных и индексных буферов.
typedef struct RendererMesh RendererMesh;

// === Кадр и широкий угол ===
//
// Кадр состоит из 1..6 проходов сцены. Классический режим — один проход
// прямо в back-буфер. Панорамный режим — проходы по граням кубмапы
// (грань 4 = вперёд в пространстве вида), после которых полноэкранный
// резолв разворачивает кубмапу в выбранную проекцию. Каждый проход несёт
// свой прямоугольник грани: рисуется и очищается только он (viewport +
// scissor), поэтому суммарная пиксельная работа панорамы близка к
// обычному кадру. Геометрию вызывающая сторона отсекает по viewProjection
// прохода — это off-center пирамида ровно под прямоугольник.

#define RENDERER_MAX_SCENE_PASSES 6

typedef enum RendererResolveMapping
{
    RENDERER_RESOLVE_FISHEYE = 0,   // равноудалённая (рыбий глаз)
    RENDERER_RESOLVE_CYLINDER = 1,  // цилиндрическая панорама
} RendererResolveMapping;

typedef struct RendererScenePass
{
    float viewProjection[16];
    uint32_t faceIndex;   // грань кубмапы 0..5 (в классическом режиме 0)
    // Задействованный прямоугольник грани в текселях [min, max).
    uint32_t rectMinX;
    uint32_t rectMinY;
    uint32_t rectMaxX;
    uint32_t rectMaxY;
} RendererScenePass;

typedef struct RendererFrameSetup
{
    bool panorama;
    uint32_t faceResolution;               // размер грани кубмапы, px
    RendererResolveMapping resolveMapping;
    float fovHalfRadians;                  // половина горизонтального поля зрения
    float resolveVerticalScale;            // вертикальный параметр проекции
    uint32_t passCount;
    RendererScenePass passes[RENDERER_MAX_SCENE_PASSES];

    // Свет кадра (цикл дня и ночи задаёт ядро; рендерер лишь передаёт
    // значения шейдеру и очищает цели цветом неба).
    float sunDirection[3];   // единичный, от источника к миру
    float sunColor[3];
    float ambientColor[3];
    float skyColor[3];
    float gamma;             // 1.0 — нейтрально; выход шейдера = pow(цвет, 1/gamma)
} RendererFrameSetup;

LAIUE_RENDER_API Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height);
LAIUE_RENDER_API void      RendererDestroy(Renderer* renderer);

// Начало кадра: применяет отложенный resize, записывает загрузки мешей
// и атласа, готовит ресурсы панорамы. Возвращает false, если кадр рисовать
// нельзя (например, resize не удался) — тогда проходы/EndFrame пропускаются.
LAIUE_RENDER_API bool RendererBeginFrame(Renderer* renderer,
    const RendererFrameSetup* frame);

// Начало прохода сцены passIndex из RendererFrameSetup: назначает цель,
// очищает её и ставит viewProjection прохода. Между вызовами проходов
// вызывающая сторона рисует меши.
LAIUE_RENDER_API void RendererBeginScenePass(Renderer* renderer, uint32_t passIndex);

// Конец кадра: резолв панорамы (если была), слой UI, present.
// Возвращает false, если DXGI не смог показать кадр.
LAIUE_RENDER_API bool RendererEndFrame(Renderer* renderer);

// Статистика последнего успешно показанного кадра. Не синхронизирует CPU
// с GPU и потому подходит для HUD и внешнего профилировщика.
LAIUE_RENDER_API void RendererGetStats(const Renderer* renderer,
    RendererStats* outStats);

LAIUE_RENDER_API void RendererSetVerticalSync(Renderer* renderer, bool enabled);
LAIUE_RENDER_API bool RendererIsVerticalSyncEnabled(const Renderer* renderer);

// === Слой интерфейса ===
//
// Квады в пиксельных координатах окна, рисуются поверх кадра в EndFrame
// с альфа-смешиванием. Раскладка повторяет shaders/ui.hlsl (48 байт).

#define RENDERER_UI_MAX_QUADS 2048u
#define RENDERER_UI_QUAD_TEXT 1u  // альфа берётся из атласа шрифта

typedef struct RendererUiQuad
{
    float rect[4];        // x0, y0, x1, y1 в пикселях окна
    float uv[4];          // u0, v0, u1, v1 атласа (для текста)
    uint32_t colorRGBA;   // R в младшем байте, A в старшем
    float cornerRadius;   // радиус скругления, px (0 — прямые углы)
    uint32_t flags;
    uint32_t reserved;
} RendererUiQuad;

// Атлас шрифта: 8-битная альфа. Вызов заменяет предыдущий атлас
// (дожидается GPU — вызывать редко, при смене масштаба интерфейса).
LAIUE_RENDER_API bool RendererUiSetFontAtlas(Renderer* renderer,
    const uint8_t* alphaPixels, uint32_t width, uint32_t height);

// Ставит квады в очередь текущего кадра. Вызывать между BeginFrame
// и EndFrame; порядок вызовов задаёт порядок отрисовки.
LAIUE_RENDER_API void RendererUiQueue(Renderer* renderer,
    const RendererUiQuad* quads, uint32_t count);

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
// (origin rebasing — камера всегда около нуля).
LAIUE_RENDER_API void RendererDrawMesh(Renderer* renderer, const RendererMesh* mesh,
    const float chunkOriginRelative[3]);

LAIUE_RENDER_API void RendererResize(Renderer* renderer, int32_t width, int32_t height);

// Перезагружает текстурный пак из active.txt (вызывать вне BeginFrame/EndFrame).
LAIUE_RENDER_API bool RendererReloadTexturePack(Renderer* renderer);
LAIUE_RENDER_API RendererContentStatus RendererGetTexturePackLoadStatus(
    const Renderer* renderer);

// Настройки отладки рендера.
LAIUE_RENDER_API void RendererSetWireframe(Renderer* renderer, bool enabled);
LAIUE_RENDER_API bool RendererIsWireframe(const Renderer* renderer);

// Перезагрузка шейдеров из байткода. Каждый параметр — указатель на DXBC
// и его длина. Если указатель NULL, используется встроенный шейдер.
// Вызывать вне BeginFrame/EndFrame.
LAIUE_RENDER_API bool RendererReloadShaders(Renderer* renderer,
    const void* chunkVS, uint32_t chunkVSLength,
    const void* chunkPS, uint32_t chunkPSLength,
    const void* panoramaVS, uint32_t panoramaVSLength,
    const void* panoramaPS, uint32_t panoramaPSLength,
    const void* uiVS, uint32_t uiVSLength,
    const void* uiPS, uint32_t uiPSLength);
