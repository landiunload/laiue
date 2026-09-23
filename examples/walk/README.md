# `examples/walk`

This is the smallest public-SDK walk slice. It owns the game-specific base
provider (grass at `z=0`, earth at `z=-1..-3`, stone below), loads the public
`laiue.character` and optional `laiue.voxel` service tables through the
bootstrap, keeps
authoritative coordinates as integer cell plus local offsets, and advances a
kinematic character at 128 Hz. It does not link `physics`, `render`, or
`scene`; those can be supplied later as independent modules through the same
service contracts.

The character provider is required for this sample. If the voxel module is
missing or cannot create its sparse world, the sample stays usable and falls
back to the game-owned infinite grass/earth/stone strata, reporting the
degraded mode at startup. Static/mobile profiles can set
`LAIUE_WALK_STATIC_WITH_VOXEL=OFF` to omit the voxel artifact at link time;
the same fallback is then used without changing the application code.

Build it with `-DLAIUE_BUILD_EXAMPLES=ON`. Window/input/graphics/UI adapters
can register their services around this core without changing the world or
character code. The current sample is intentionally headless so it also
validates the no-graphics bootstrap profile.
