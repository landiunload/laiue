# Animation

`laiue_animation_demo` loads a three-joint model from `Demo.lop`, samples
`wave.lk` from `Demo.lkp`, and renders its textured, deforming sleeve.
Run it normally for a window. The model and clip use matching joint indices.

Set `LAIUE_ANIMATION_DEMO_MODE=cpu` for sampling without graphics, or `capture`
for five fixed GPU frames. `LAIUE_ANIMATION_CAPTURE_DIR` selects an existing
directory for BMP captures; `LAIUE_ANIMATION_DEMO_BACKEND=d3d12` selects the
Windows backend instead of Vulkan. A missing animation provider displays
the bind mesh. These are animation assets; this example uses no physics.

`LAIUE_ANIMATION_WINDOW_FRAMES=12` closes after twelve presented frames;
`LAIUE_ANIMATION_REQUIRE_WINDOW=1` rejects a missing window or graphics provider.
`LAIUE_ANIMATION_REQUIRE_FULL=1` requires the animated binding instead of fallback.

`generate_assets.py` rebuilds the original `.lo` and `.lk` fixtures using
Python's standard library. The renderer uses the generic device API; vertex
uploads currently replace backend geometry, so GPU streaming is not allocation-free.
