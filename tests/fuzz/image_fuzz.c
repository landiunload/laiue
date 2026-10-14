#include "media/image.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 22))
        return 0;
    ImageInfo info;
    ImageProbe(data, (uint32_t)size);
    if (ImageInspect(data, (uint32_t)size, &info) != IMAGE_OK)
        return 0;
    if (info.pixelBytes > (48u << 20) || info.scratchBytes > (48u << 20))
        return 0;
    uint8_t *pixels = malloc(info.pixelBytes ? info.pixelBytes : 1u);
    void *scratch = info.scratchBytes ? malloc(info.scratchBytes) : NULL;
    ImageDecode(data, (uint32_t)size, &info, pixels, info.pixelBytes, scratch, info.scratchBytes);
    free(scratch);
    free(pixels);
    return 0;
}
