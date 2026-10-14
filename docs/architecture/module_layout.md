# Canonical technology layout

LAIUE is a collection of optional technology providers. The source tree uses
physical folders to show ownership without changing the public C ABI or the
logical include names (`physics/...`, `render/...`, and so on).

| Folder | Logical modules | Responsibility |
|---|---|---|
| `src/core/` | `math`, `runtime` | no-CRT runtime and scalar support |
| `src/assets/` | `content`, `media` | resource catalogs and format codecs |
| `src/graphics/` | `graphics`, `input`, `mesh`, `render`, `scene_math`, `scene`, `voxel_render` | graphics contract, input, meshing, backends, shared scene math, presentation scene and voxel-to-render adapter |
| `src/jobs/` | `task` | optional work scheduler |
| `src/simulation/` | `world`, `physics`, `character`, `voxel` | world coordinates, deterministic simulation and voxel provider |
| `src/modding/` | `mod` | bootstrap-compatible native module ABI and host |
| `src/platform/` | `platform`, `window` | OS boundary and window implementation |
| `src/audio/` | `audio`, `audio_output`, `audio_pack` | mixer/offscreen core, optional platform output provider, and optional sound-pack provider |
| `src/ui/` | `ui` | backend-neutral UI draw lists |
| `src/numeric/` | `numeric` | infinite-coordinate service provider |

The logical module names remain stable because they are part of target names,
service IDs, installed header paths and the native module manifest. A module
is compiled from exactly one canonical folder; source files are never copied
to preserve an old path. `tools/check_architecture.cmake` checks both include
boundaries and the canonical location, so a new flat duplicate is rejected at
configure time.

`scene_math` is the single owner of public matrix/frustum code. `scene` now
contains only camera and panorama. The renderer-facing `voxel_render` provider
owns chunk meshing/upload queues, while `voxel_raycast` is a core provider
under `simulation/voxel` that depends on `world` and has no renderer
dependency. World and physics remain under `simulation` and never gain a
dependency on a renderer.

The public SDK continues to install headers directly under
`include/laiue/<module>`, including the service table next to each technology
contract. Category folders are an implementation layout, not a second ABI or
a second copy of the SDK.
