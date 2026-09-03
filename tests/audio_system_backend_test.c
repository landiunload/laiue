// Системный вывод: устройство действительно открывается и его поток
// действительно тянет кадры. Микшер проверяется отдельно на offscreen —
// здесь важно именно то, что не проверить без звуковой подсистемы.
//
// Звука тест не издаёт: общая громкость нулевая, голосов нет. Там, где
// устройства нет вовсе (контейнер CI, машина без звуковой карты), тест
// сообщает о пропуске кодом 125, а не о ложном провале.

#include "audio/audio.h"
#include "audio/audio_offscreen.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#define SKIP_EXIT_CODE 125
#define OBSERVATION_MILLISECONDS 400u
#define POLL_INTERVAL_MILLISECONDS 20u

static void Expect(bool condition, const char *message)
{
    if (condition) return;
    LaiueTestRuntimeWrite("Audio system backend check failed: ");
    LaiueTestRuntimeWrite(message);
    LaiueTestRuntimeWrite("\n");
    LaiueTestRuntimeExit(1);
}

LAIUE_TEST_ENTRY(AudioSystemBackendTestEntryPoint)
{
    AudioDeviceConfiguration configuration = {
        .backend = AUDIO_BACKEND_SYSTEM,
        .sampleRate = 0u,
        .frameCountHint = 0u,
        .masterVolume = 0.0f,
    };

    AudioDevice *device = NULL;
    AudioResult result = AudioDeviceCreate(&configuration, &device);
    if (result != AUDIO_RESULT_OK)
    {
        LaiueTestRuntimeWrite("No system audio output available; skipping\n");
        LaiueTestRuntimeExit(SKIP_EXIT_CODE);
    }

    AudioDeviceStats stats;
    Expect(AudioDeviceGetStats(device, &stats), "stats must be readable");
    Expect(stats.sampleRate >= 8000u && stats.sampleRate <= 384000u,
           "the device must report a plausible sample rate");
    Expect(stats.channelCount == 2u, "mixing is always stereo");
    Expect(stats.bufferFrameCount > 0u, "the device must report its buffer size");

    // Поток вывода должен тянуть кадры сам, без участия приложения.
    uint64_t mixedFrames = 0u;
    for (uint32_t waited = 0u; waited < OBSERVATION_MILLISECONDS;
         waited += POLL_INTERVAL_MILLISECONDS)
    {
        PlatformSleepMilliseconds(POLL_INTERVAL_MILLISECONDS);
        if (!AudioDeviceGetStats(device, &stats)) continue;
        mixedFrames = stats.mixedFrames;
        if (mixedFrames > 0u) break;
    }
    Expect(mixedFrames > 0u, "the output thread must pull frames from the mixer");

    // Кадры готовит поток вывода, поэтому прямой рендер запрещён: иначе
    // два потока писали бы в один микс.
    float frames[8] = { 0.0f };
    Expect(!AudioDeviceRenderFrames(device, frames, 4u),
           "direct rendering must be refused for a system device");

    AudioDeviceDestroy(device);

    LaiueTestRuntimeWrite("Audio system backend checks passed\n");
    LAIUE_TEST_SUCCESS();
}
