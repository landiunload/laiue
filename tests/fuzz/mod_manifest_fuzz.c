#include "mod/mod_manifest.h"
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    LaiueModManifest *manifest = malloc(sizeof(*manifest));
    LaiueModDiagnostic diagnostic;
    if (LaiueModManifestParse(data, size, manifest, &diagnostic) == LAIUE_MOD_STATUS_OK)
    {
        wchar_t entry[260];
        LaiueModManifestSelectNativeEntry(manifest, entry, 260u, &diagnostic);
    }
    free(manifest);
    return 0;
}
