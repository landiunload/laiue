#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Диагностический доступ к последнему показанному кадру. Существует
// только у offscreen-бэкендов: без swapchain кадр иначе невозможно ни
// увидеть, ни проверить. В SDK заголовок не устанавливается — это
// внутренний контракт движка и его тестов, а не часть публичного API.
//
// Пиксели возвращаются как RGBA8, строками сверху вниз, без выравнивания
// строк. Вызывать после успешного RendererEndFrame; вызов синхронизирует
// CPU с GPU и потому не предназначен для горячего пути.

typedef struct Renderer Renderer;

LAIUE_RENDER_API bool RendererCaptureFrame(Renderer *renderer, void *outPixels,
                                           uint32_t capacityBytes, uint32_t *outWidth,
                                           uint32_t *outHeight);
