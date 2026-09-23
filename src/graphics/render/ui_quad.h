#pragma once

/* Compatibility spelling for the old renderer headers.  The canonical
 * layout belongs to the backend-neutral graphics contract; UI no longer
 * imports this renderer-private header. */

#include "graphics/graphics_api.h"

#define RENDERER_UI_MAX_QUADS LAIUE_GRAPHICS_UI_MAX_QUADS
#define RENDERER_UI_QUAD_TEXT LAIUE_GRAPHICS_UI_QUAD_TEXT
#define RENDERER_UI_QUAD_IMAGE LAIUE_GRAPHICS_UI_QUAD_IMAGE

typedef LaiueGraphicsUiQuadV1 RendererUiQuad;
