#pragma once

#include <stdint.h>

#include "render/renderer.h"

#define REMOTE_PLAYER_RENDER_CAPACITY 32u

typedef struct RemotePlayerRenderPose
{
    double feetPosition[3];
} RemotePlayerRenderPose;

typedef struct RemotePlayerRenderer
{
    RendererMesh *mesh;
    RendererMeshInstance instances[REMOTE_PLAYER_RENDER_CAPACITY];
} RemotePlayerRenderer;

// Mesh uploads are queued before RendererBeginFrame. A transient allocation
// failure is retryable, so callers may invoke this once per presentation
// frame until it succeeds.
bool RemotePlayerRendererEnsure(RemotePlayerRenderer *remotePlayers, Renderer *renderer);

void RemotePlayerRendererShutdown(RemotePlayerRenderer *remotePlayers, Renderer *renderer);

// Draws already interpolated presentation poses. Simulation and network
// state are intentionally not visible to this rendering-only boundary.
void RemotePlayerRendererDraw(RemotePlayerRenderer *remotePlayers, Renderer *renderer,
                              const RemotePlayerRenderPose *poses, uint32_t poseCount,
                              const int64_t cameraBlockPosition[3]);
