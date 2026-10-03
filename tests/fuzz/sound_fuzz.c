#include "media/sound.h"
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 22))
        return 0;
    SoundInfo info;
    SoundProbe(data, (uint32_t)size);
    if (SoundInspect(data, (uint32_t)size, &info) != SOUND_OK)
        return 0;
    if ((uint64_t)info.sampleCount * 2u > (64u << 20) || info.scratchBytes > (64u << 20))
        return 0;
    int16_t *samples = malloc((size_t)info.sampleCount * 2u + 2u);
    void *scratch = info.scratchBytes ? malloc(info.scratchBytes) : NULL;
    SoundDecodeSamples(data, (uint32_t)size, &info, samples, info.sampleCount, scratch,
                       info.scratchBytes);
    free(scratch);
    free(samples);
    return 0;
}
