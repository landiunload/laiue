#pragma once

#include "api.h"
#include "render/chunk_geometry.h"
#include "render/shader_pack.h"

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
    // Пак загружен, но части материалов в нём не нашлось: они показаны
    // нейтральным слоем. Отказом это не считается — решает приложение.
    RENDERER_CONTENT_INCOMPLETE,
} RendererContentStatus;

// GPU-резидентный меш: квады один раз копируются в общий DEFAULT-буфер
// (суб-аллокация, без 64-КиБ ресурса на меш) и рисуются vertex pulling'ом
// без вершинных и индексных буферов.
typedef struct RendererMesh RendererMesh;

typedef struct RendererMeshInstance
{
    float originRelative[3];
    float scale;
} RendererMeshInstance;

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

    // Свет кадра полностью задаёт приложение; рендерер лишь передаёт
    // значения шейдеру и очищает цели цветом неба.
    float sunDirection[3];   // единичный, от источника к миру
    float sunColor[3];
    float ambientColor[3];
    float skyColor[3];
    float gamma;             // 1.0 — нейтрально; выход шейдера = pow(цвет, 1/gamma)

    // Часы анимации текстур в секундах. Расписание кадров лежит в
    // текстурпаке, а идти времени или стоять — решает приложение: пауза
    // это просто одно и то же значение два кадра подряд.
    double animationSeconds;
} RendererFrameSetup;

// Создаёт только swapchain и UI-слой. Ресурсы мира вызывающая сторона
// загружает отдельно, когда они действительно нужны.
LAIUE_RENDER_API Renderer* RendererCreate(void* windowHandle, int32_t width, int32_t height);
LAIUE_RENDER_API void      RendererDestroy(Renderer* renderer);
// Имена материалов внутри текстурпака: material id 1 берёт names[0],
// id 2 — names[1] и так далее, до 64 материалов. Имя вправе содержать
// '/', поэтому пак делится на подпапки, а расширение движок подбирает
// сам: `.lt`, `.png` или `.gif`.
//
// Имена копируются и применяются при следующей подготовке мира или
// перезагрузке пака. Пока приложение их не задало, рендерер показывает
// один нейтральный материал: движок не придумывает, что такое «камень».
LAIUE_RENDER_API bool RendererSetMaterialNames(Renderer *renderer, const wchar_t *const *names,
                                               uint32_t count);

LAIUE_RENDER_API bool RendererPrepareWorldFrom(Renderer *renderer, LaiueContentCatalog *catalog);
LAIUE_RENDER_API bool      RendererPrepareWorld(Renderer* renderer);
LAIUE_RENDER_API void      RendererReleaseWorld(Renderer* renderer);
LAIUE_RENDER_API bool      RendererIsWorldReady(const Renderer* renderer);

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
// с GPU и потому подходит для диагностики и внешнего профилировщика.
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
#define RENDERER_UI_QUAD_IMAGE 2u // цвет берётся из фоновой UI-текстуры

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

// Единственная статичная полноэкранная картинка оболочки. Декодируется
// системным WIC один раз; мир и его текстурпак для этого не создаются.
LAIUE_RENDER_API bool RendererUiLoadBackground(Renderer* renderer,
    const wchar_t* path, uint32_t* outWidth, uint32_t* outHeight);

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
LAIUE_RENDER_API void RendererDrawMeshInstances(Renderer* renderer,
    const RendererMesh* mesh, const RendererMeshInstance* instances,
    uint32_t instanceCount);

LAIUE_RENDER_API void RendererResize(Renderer* renderer, int32_t width, int32_t height);

// Explicit-catalog variant keeps content location an application policy.
// Both calls copy/upload the pack synchronously and do not retain catalog.
// The swap is transactional: an invalid pack, I/O error or GPU allocation
// failure leaves the previous textures and descriptors active.  With no
// active pack, a neutral built-in fallback is selected intentionally.
LAIUE_RENDER_API bool RendererReloadTexturePackFrom(Renderer *renderer,
                                                    LaiueContentCatalog *catalog);
// Compatibility wrapper: reloads from the executable-root default catalog.
LAIUE_RENDER_API bool RendererReloadTexturePack(Renderer* renderer);
LAIUE_RENDER_API RendererContentStatus RendererGetTexturePackLoadStatus(
    const Renderer* renderer);

// Настройки отладки рендера.
LAIUE_RENDER_API void RendererSetWireframe(Renderer* renderer, bool enabled);
LAIUE_RENDER_API bool RendererIsWireframe(const Renderer* renderer);

// Transactionally replaces every renderer pipeline represented by shaderSet.
// Missing overrides use embedded bytecode.  The renderer copies overrides and
// retains no pointer into shaderSet.  NULL selects the complete fallback set.
// On validation/allocation/PSO failure the previous working set is retained.
// Call outside BeginFrame/EndFrame.
LAIUE_RENDER_API bool RendererReloadShaderSet(Renderer *renderer, const LaiueShaderSet *shaderSet);

// Loads the active pack from catalog and applies it transactionally.  With no
// active pack the embedded complete fallback set is applied.  Invalid packs
// and PSO failures keep the previous working set active and return false.
LAIUE_RENDER_API bool RendererReloadShaderPackFrom(Renderer *renderer, LaiueContentCatalog *catalog,
                                                   ShaderPackLoadStatus *outStatus);
// Compatibility wrapper: uses the executable-root default catalog.
LAIUE_RENDER_API bool RendererReloadShaderPack(Renderer *renderer, ShaderPackLoadStatus *outStatus);

// Legacy adapter retained for source compatibility.  Prefer the versioned
// LaiueShaderSet API above.
LAIUE_RENDER_API bool RendererReloadShaders(Renderer* renderer,
    const void* chunkVS, uint32_t chunkVSLength,
    const void* chunkPS, uint32_t chunkPSLength,
    const void* panoramaVS, uint32_t panoramaVSLength,
    const void* panoramaPS, uint32_t panoramaPSLength,
    const void* uiVS, uint32_t uiVSLength,
    const void* uiPS, uint32_t uiPSLength);
