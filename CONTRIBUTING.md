# Contributing

Module contracts, the ABI, platform limits, build commands and CI are
described in [docs/development.md](docs/development.md). Read it before
changing code.

## Checks

1. Run the narrow regression test for your change first.
2. Build and run the full CTest suite in Debug and Release. There must be
   no errors or warnings; don't silence diagnostics to get there.
3. Check the architecture and the diff:

   ```powershell
   pwsh -NoProfile -File tools/check_architecture.ps1
   git diff --check
   ```

4. Apply `.clang-format` only to the C and H files you touched. CI runs
   `clang-tidy` with `.clang-tidy`; fix findings instead of suppressing them.
5. Back optimizations with repeatable before and after measurements.
   Update the nearest documentation when a contract changes.

## Pull requests

Every change reaches `main` through a pull request that passes CI and is
squash-merged, so each change becomes one commit titled after the pull
request. A commit message describes the change and nothing else: no
co-authors, sign-offs, generated-by lines or session links.

## Releases

Versions follow [Semantic Versioning](https://semver.org). A release updates
the version in `CMakeLists.txt` and the entry in `CHANGELOG.md` through a
pull request, then tags the merged commit:

```sh
git tag -a v0.2.0 -m "laiue 0.2.0"
git push origin v0.2.0
```

CI builds the SDKs and publishes the GitHub release. Tags are used only for
releases.
