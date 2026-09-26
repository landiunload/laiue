#pragma once

#include "render/renderer.h"

// First member of each backend's private Renderer. Set before publication and
// immutable until destruction; callers must keep the Renderer alive during use.
// C permits conversion between a struct pointer and its first member pointer.
typedef struct RendererHeader
{
    RendererBackendKind backend;
} RendererHeader;
