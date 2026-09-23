# Native module template

Set `LAIUE_SDK_INCLUDE_DIR` to the installed `include/laiue` directory and
build this directory with CMake. The template builds three independent
libraries:

* `example_service` — a minimal new service;
* `example_ui_extension` — a namespaced UI extension requiring `laiue.ui`;
* `example_material_shader` — a namespaced material/shader resource requiring
  the backend-neutral `laiue.graphics.device` service.

Each library exports only `LaiueModuleGetApiV1` and does not link
`laiue_engine`, `laiue_mod`, graphics, physics, or voxel code. Copy the
platform artifact into an application profile only after validating its
`module.laiue` manifest with `LaiueModuleManifestValidate` and checking that
its descriptor matches with `LaiueModuleManifestValidateApi`.

Native code is trusted in-process code, not a sandbox. Build one artifact per
OS/architecture and choose it explicitly in the application profile.
