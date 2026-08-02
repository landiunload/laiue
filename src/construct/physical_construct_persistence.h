#pragma once

#include "construct/physical_construct.h"

#include <stdbool.h>
#include <wchar.h>

// Commits World and PhysicalConstructSystem as one crash-consistent unit.
// Two generation slots are retained. Data files are written first and a
// bounded, hashed commit record publishes the pair last with atomic replace.
// Load selects the newest complete generation and falls back to the previous
// committed generation when the newest record or either data file is damaged.
// With no commit records, the unsuffixed paths are read as the legacy layout.
LAIUE_CONSTRUCT_API bool PhysicalConstructPersistenceSave(
    World* world, const PhysicalConstructSystem* constructs,
    const wchar_t* worldPath, const wchar_t* constructPath,
    const wchar_t* commitPath);

LAIUE_CONSTRUCT_API bool PhysicalConstructPersistenceLoad(
    World* world, PhysicalConstructSystem* constructs,
    const wchar_t* worldPath, const wchar_t* constructPath,
    const wchar_t* commitPath);
