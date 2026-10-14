#include "mod/module_manifest.h"
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20))
        return 0;
    LaiueModuleManifestStorageV1 *storage = malloc(sizeof(*storage));
    LaiueModuleManifestDiagnostic diagnostic;
    // Копия без хвостового нуля: парсер обязан не читать за textSize.
    char *text = malloc(size ? size : 1u);
    for (size_t i = 0; i < size; ++i)
        text[i] = (char)data[i];
    LaiueModuleManifestParseTextV1(text, (uint32_t)size, storage, &diagnostic);
    free(text);
    free(storage);
    return 0;
}
