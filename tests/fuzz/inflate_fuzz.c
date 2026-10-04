#include "media/inflate.h"
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3 || size > (1u << 20))
        return 0;
    // Первый байт выбирает размер выхода, второй — точку разреза на сегменты.
    uint32_t outputBytes = ((uint32_t)data[0] << 8) | 1u;
    uint32_t split = data[1] % (uint32_t)(size - 2);
    InflateSegment segments[2] = {{data + 2, split},
                                  {data + 2 + split, (uint32_t)(size - 2 - split)}};
    uint8_t *output = malloc(outputBytes);
    InflateWork *work = malloc(sizeof(*work));
    uint32_t written = 0;
    InflateZlib(segments, 2u, output, outputBytes, work, sizeof(*work), &written);
    free(work);
    free(output);
    return 0;
}
