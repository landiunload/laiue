# `examples/walk`

This is the smallest public-SDK walk slice. It owns the game-specific base
provider (grass at `z=0`, earth at `z=-1..-3`, stone below), loads the public
`laiue.character` and `laiue.voxel` service tables through the bootstrap, keeps
authoritative coordinates as integer cell plus local offsets, and advances a
kinematic character at 128 Hz. It does not link `physics`, `render`, or
`scene`; those can be supplied later as independent modules through the same
service contracts.

Build it with `-DLAIUE_BUILD_EXAMPLES=ON`. Window/input/graphics/UI adapters
can register their services around this core without changing the world or
character code. The current sample is intentionally headless so it also
validates the no-graphics bootstrap profile.
