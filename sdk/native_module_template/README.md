# Native module template

Set `LAIUE_SDK_INCLUDE_DIR` to the installed `include/laiue` directory and
build this directory with CMake. The resulting library exports only
`LaiueModuleGetApiV1`; it does not link `laiue_engine`, `laiue_mod`, graphics,
physics, or voxel code. Copy the platform artifact into an application profile
only after validating `module.laiue` with `LaiueModuleManifestValidate` and
checking that its descriptor matches with `LaiueModuleManifestValidateApi`.

Native code is trusted in-process code, not a sandbox. Build one artifact per
OS/architecture and choose it explicitly in the application profile.
