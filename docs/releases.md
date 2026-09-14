# Automated Windows releases

[Windows release](../.github/workflows/release.yml) runs on every push to `main`. It can also be started manually from GitHub Actions on `main`. A successful run publishes a normal GitHub Release tagged `v<application-version>-build.<workflow-run-number>`, for example `v0.8.0-build.1`. The application version remains the version in the validation receipt; no bot version-bump commit is needed.

## Build and assets

The workflow uses `windows-2025`, the archive/hash pinned in `dependencies.json` and official actions pinned to commit SHAs. Only the compiler archive is cached; bootstrap verifies its checksum before extracting it.

It runs:

~~~powershell
./build.ps1 -Validate -WarpOnly
./smoke-test.ps1
./standalone/install_test.ps1
./standalone/package_test.ps1
~~~

`-WarpOnly` runs real software D3D12 capture/composition/PFD tests without requiring a physical GPU. Other native, camera lifecycle, ABI, settings and graphics-state checks still run. The receipt records hardware validation as `not-run`, WARP as `passed` and `simulatorVerified: false`. Local `build.ps1 -Validate` continues to require both hardware and WARP.

A release contains:

- A Windows x64 ZIP with the companion, bridge, installer/uninstaller, defaults, notices, documentation, validation receipt, per-file hash manifest and source-commit provenance.
- `SHA256SUMS.txt` for the ZIP.
- The native `validation.json` as a separate asset.

Workflow artifacts retain the package and diagnostic logs for 30 days, including logs from failed validation. The release is created only after all required checks pass. MSFS is neither installed nor launched on the runner.

## Notes and publication

[The publication script](../ci/publish-release.ps1) targets the exact checked-out commit. Notes include installation guidance, validation scope, a build-log link and all commits since the most recent published ancestor build. GitHub's generated notes add its pull-request/contributor summary. The first release includes the available commit history. See [GitHub's release CLI documentation](https://cli.github.com/manual/gh_release_create).

Publication creates a draft, uploads all three assets, then publishes. A retry can finish an incomplete draft. A published release and its assets are preserved on rerun. Tags use the workflow run number, which stays unchanged for reruns.

Separate pushes are not cancelled or collapsed into one build. A build is marked Latest only if its commit is still the current `main` when published, reducing the chance that a slower older build displaces the current result. Publication is not a transactional lock against another simultaneous push.

The workflow requests `contents: write` for its release job and uses the built-in `GITHUB_TOKEN` only in the publication step. No personal access token or extra repository secret is required. Checkout does not persist credentials.

## Local checks and troubleshooting

~~~powershell
# CI-equivalent GPU selection:
./build.ps1 -Bootstrap -Validate -WarpOnly
./smoke-test.ps1
./standalone/install_test.ps1
./standalone/package_test.ps1

# Normal local hardware + WARP validation:
./build.ps1 -Validate
~~~

A failed build, validation or packaging step does not publish a release. Read the failed step and its diagnostic artifact, fix the source and push again, or rerun the failed workflow. If publication alone fails, rerun it to complete the existing draft.

Publishing a CI package establishes repeatable build/isolated-test success. It does not establish correct live MSFS camera perspectives, frame time, light rendering or recovery; those remain simulator checks documented in [the architecture](architecture.md#validation-and-known-limits).
