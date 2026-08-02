#pragma once

#include "construct/physical_construct.h"

#include <stdbool.h>
#include <wchar.h>

// Laiue Physical Construct Store v1 is a bounded, explicitly little-endian
// format. Player grab ownership is transient and is intentionally not saved.
// Save uses the platform atomic-replace primitive; load validates the complete
// file before replacing the current in-memory construct set.
LAIUE_CONSTRUCT_API bool PhysicalConstructStoreSave(const PhysicalConstructSystem *system,
                                                    const wchar_t *path);
LAIUE_CONSTRUCT_API bool PhysicalConstructStoreLoad(PhysicalConstructSystem *system,
                                                    const wchar_t *path);
