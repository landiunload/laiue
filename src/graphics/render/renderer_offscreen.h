#pragma once

#include "api.h"

#include <stdbool.h>
#include <stdint.h>

// Диагностический доступ к собственному изображению кадра рендерера.
// В SDK заголовок не устанавливается — публичные потребители используют
// optional diagnostics хвост Graphics Device V2.
//
// Пиксели возвращаются как RGBA8, строками сверху вниз, без выравнивания
// строк. Вызывать после успешного RendererEndFrame; вызов синхронизирует
// CPU с GPU и потому не предназначен для горячего пути.

typedef struct Renderer Renderer;

LAIUE_RENDER_API bool RendererCaptureFrame(Renderer *renderer, void *outPixels,
                                           uint32_t capacityBytes, uint32_t *outWidth,
                                           uint32_t *outHeight);

// Capability means that explicit diagnostic capture can be requested.
// D3D12 FLIP_DISCARD does not retain the back buffer after Present, so that
// backend requires a request before EndFrame to preserve a private copy.
// Vulkan reads its private color target and needs no request.
LAIUE_RENDER_API bool RendererCanCaptureFrame(const Renderer *renderer);
LAIUE_RENDER_API bool RendererRequestFrameCapture(Renderer *renderer);
