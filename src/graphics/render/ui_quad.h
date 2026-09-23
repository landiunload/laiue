#pragma once

/* Renderer-neutral UI upload contract.  The UI provider owns construction;
 * graphics providers only consume this fixed layout at the frame boundary. */

#include <stdint.h>

#define RENDERER_UI_MAX_QUADS 2048u
#define RENDERER_UI_QUAD_TEXT 1u
#define RENDERER_UI_QUAD_IMAGE 2u

typedef struct RendererUiQuad
{
    float rect[4];
    float uv[4];
    uint32_t colorRGBA;
    float cornerRadius;
    uint32_t flags;
    uint32_t reserved;
} RendererUiQuad;
