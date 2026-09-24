#pragma once

#include "scene/camera.h"
#include "scene/panorama.h"
#include "mod/module_api.h"

#include <stdint.h>

#define LAIUE_SCENE_SERVICE_NAME "laiue.scene"
#define LAIUE_SCENE_SERVICE_ABI_VERSION_1 1u

typedef struct LaiueSceneServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    void (*cameraInit)(Camera *camera, double x, double y, double z,
                       float yaw, float pitch);
    void (*cameraUpdate)(Camera *camera, float deltaSeconds,
                         bool keyForward, bool keyLeft, bool keyBackward,
                         bool keyRight, bool keyUp, int32_t mouseDeltaX,
                         int32_t mouseDeltaY, float speed,
                         float mouseSensitivity);
    void (*cameraGetForwardVector)(const Camera *camera, float outForward[3]);
    void (*cameraGetViewMatrix)(const Camera *camera,
                                const float relativeEyePosition[3],
                                float outMatrix[16]);
    void (*cameraGetProjectionMatrix)(float aspectRatio, float fovRadians,
                                      float nearPlane, float farPlane,
                                      float outMatrix[16]);
    bool (*panoramaIsActive)(RenderProjection projection,
                             float fovHorizontalDegrees);
    void (*panoramaBuildFrameSetup)(PanoramaCache *cache,
                                    RenderProjection projection,
                                    float fovHorizontalDegrees, int32_t width,
                                    int32_t height, float nearPlane,
                                    float farPlane, const float view[16],
                                    RendererFrameSetup *outSetup);
} LaiueSceneServiceV1;

const LaiueModuleApiV1 *LaiueSceneGetStaticModuleApiV1(void);
