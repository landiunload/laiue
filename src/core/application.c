#include "api.h"
#include "platform/window.h"
#include "input/input.h"
#include "world/world.h"
#include "render/renderer.h"
#include "game/camera.h"

#include <stddef.h>
#include <windows.h>

#define MAX_CHUNK_MESHES 256
#define CAMERA_SPEED 80.0f

typedef struct ChunkMesh
{
    ChunkVertex* vertices;
    uint32_t     vertexCount;
    uint32_t*    indices;
    uint32_t     indexCount;
} ChunkMesh;

typedef struct ApplicationState
{
    Window*    window;
    Input*     input;
    World*     world;
    Renderer*  renderer;
    Camera     camera;
    ChunkMesh  meshes[MAX_CHUNK_MESHES];
    uint32_t   meshCount;
    int32_t    windowWidth;
    int32_t    windowHeight;
    int64_t    perfFreq;
    int64_t    prevPerfCounter;
    int64_t    lastCx;
    int64_t    lastCy;
    int64_t    lastCz;
} ApplicationState;

static void MulMatrix4(const float a[16], const float b[16], float out[16])
{
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[i * 4 + k] * b[k * 4 + j];
            out[i * 4 + j] = sum;
        }
    }
}

static void HandleRawInput(void* userData, void* rawInputHandle)
{
    InputHandleRawInput((Input*)userData, rawInputHandle);
}

static void BuildChunkMeshes(ApplicationState* app, int64_t centerCx, int64_t centerCy, int64_t centerCz, int rad)
{
    app->meshCount = 0;
    for (int64_t cx = centerCx - rad; cx <= centerCx + rad && app->meshCount < MAX_CHUNK_MESHES; ++cx)
    {
        for (int64_t cy = centerCy - rad; cy <= centerCy + rad && app->meshCount < MAX_CHUNK_MESHES; ++cy)
        {
            for (int64_t cz = centerCz - rad; cz <= centerCz + rad && app->meshCount < MAX_CHUNK_MESHES; ++cz)
            {
                ChunkVertex* verts;
                uint32_t vertCount;
                uint32_t* idxs;
                uint32_t idxCount;
                if (BuildChunkMesh(app->world, cx, cy, cz, &verts, &vertCount, &idxs, &idxCount))
                {
                    app->meshes[app->meshCount].vertices     = verts;
                    app->meshes[app->meshCount].vertexCount   = vertCount;
                    app->meshes[app->meshCount].indices      = idxs;
                    app->meshes[app->meshCount].indexCount    = idxCount;
                    app->meshCount++;
                }
            }
        }
    }
}

static void DestroyChunkMeshes(ApplicationState* app)
{
    for (uint32_t i = 0; i < app->meshCount; ++i)
    {
        if (app->meshes[i].vertices != NULL)
            HeapFree(GetProcessHeap(), 0, app->meshes[i].vertices);
        if (app->meshes[i].indices != NULL)
            HeapFree(GetProcessHeap(), 0, app->meshes[i].indices);
    }
    app->meshCount = 0;
}

static void OnFrame(void* userData)
{
    ApplicationState* app = userData;

    if (InputWasKeyPressed(app->input, INPUT_KEY_ESCAPE))
    {
        WindowRequestClose(app->window);
        return;
    }

    if (InputWasKeyPressed(app->input, INPUT_KEY_F7))
    {
        WindowSetMouseLook(app->window, !InputIsKeyDown(app->input, INPUT_KEY_SHIFT));
    }

    if (WindowConsumeFocusLoss(app->window))
    {
        InputResetState(app->input);
    }

    if (WindowConsumeResize(app->window))
    {
        int32_t w, h;
        WindowGetClientSize(app->window, &w, &h);
        if (w > 0 && h > 0)
        {
            app->windowWidth = w;
            app->windowHeight = h;
            RendererResize(app->renderer, w, h);
        }
    }

    // Delta time
    int64_t now;
    QueryPerformanceCounter((LARGE_INTEGER*)&now);
    float dt = (float)(now - app->prevPerfCounter) / (float)app->perfFreq;
    app->prevPerfCounter = now;
    if (dt > 0.1f) dt = 0.1f;

    // Mouse delta
    int32_t mx, my;
    InputGetMouseDelta(app->input, &mx, &my);

    CameraUpdate(&app->camera, dt,
        InputIsKeyDown(app->input, INPUT_KEY_W),
        InputIsKeyDown(app->input, INPUT_KEY_A),
        InputIsKeyDown(app->input, INPUT_KEY_S),
        InputIsKeyDown(app->input, INPUT_KEY_D),
        mx, my, CAMERA_SPEED);

    // Dynamic chunk loading: if camera moved to a new chunk, reload
    int64_t ccx = (int64_t)(app->camera.position[0] / (float)CHUNK_SIZE);
    int64_t ccy = (int64_t)(app->camera.position[1] / (float)CHUNK_SIZE);
    int64_t ccz = (int64_t)(app->camera.position[2] / (float)CHUNK_SIZE);
    if (app->camera.position[0] < 0.0f) ccx--;
    if (app->camera.position[1] < 0.0f) ccy--;
    if (app->camera.position[2] < 0.0f) ccz--;
    if (ccx != app->lastCx || ccy != app->lastCy || ccz != app->lastCz)
    {
        WorldLoadArea(app->world, ccx, ccy, ccz, 1);
        DestroyChunkMeshes(app);
        BuildChunkMeshes(app, ccx, ccy, ccz, 1);
        app->lastCx = ccx;
        app->lastCy = ccy;
        app->lastCz = ccz;
    }

    float view[16], proj[16], vp[16];
    CameraGetViewMatrix(&app->camera, view);
    CameraGetProjectionMatrix((float)app->windowWidth / (float)app->windowHeight, 1.047197f, 0.1f, 1000.0f, proj);
    MulMatrix4(view, proj, vp);
    RendererSetViewProjection(app->renderer, vp);

    RendererBeginFrame(app->renderer);

    for (uint32_t i = 0; i < app->meshCount; ++i)
    {
        ChunkMesh* m = &app->meshes[i];
        if (m->vertexCount > 0)
            RendererDrawMesh(app->renderer, m->vertices, m->vertexCount, m->indices, m->indexCount);
    }

    RendererEndFrame(app->renderer);

    InputEndFrame(app->input);
}

LAIUE_CORE_API void Start(void)
{
    WindowConfiguration wc = {
        .title  = L"laiue",
        .width  = 1280,
        .height = 720,
    };

    Window* window = WindowCreate(&wc);
    if (window == NULL) return;

    Input* input = InputCreate(WindowGetNativeHandle(window));
    if (input == NULL) { WindowDestroy(window); return; }

    World* world = WorldCreate(42);
    if (world == NULL) { InputDestroy(input); WindowDestroy(window); return; }

    Renderer* renderer = RendererCreate(WindowGetNativeHandle(window), 1280, 720);
    if (renderer == NULL) { WorldDestroy(world); InputDestroy(input); WindowDestroy(window); return; }

    WorldLoadArea(world, 0, 0, 0, 1);

    int64_t perfFreq;
    QueryPerformanceFrequency((LARGE_INTEGER*)&perfFreq);
    int64_t startCounter;
    QueryPerformanceCounter((LARGE_INTEGER*)&startCounter);

    ApplicationState* app = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ApplicationState));
    if (app == NULL) { RendererDestroy(renderer); WorldDestroy(world); InputDestroy(input); WindowDestroy(window); return; }

    app->window        = window;
    app->input         = input;
    app->world         = world;
    app->renderer      = renderer;
    app->windowWidth   = 1280;
    app->windowHeight  = 720;
    app->perfFreq      = perfFreq;
    app->prevPerfCounter = startCounter;

    CameraInit(&app->camera, 0.0f, 80.0f, 0.0f, 0.0f, 0.0f);

    app->lastCx = 0;
    app->lastCy = 0;
    app->lastCz = 0;

    BuildChunkMeshes(app, 0, 0, 0, 1);

    WindowSetRawInputCallback(window, HandleRawInput, input);
    WindowRunLoop(window, OnFrame, app);

    DestroyChunkMeshes(app);
    HeapFree(GetProcessHeap(), 0, app);
    RendererDestroy(renderer);
    WorldDestroy(world);
    InputDestroy(input);
    WindowDestroy(window);
}
