# Automated Windows releases

Every push to `main` starts the [Windows release workflow](../.github/workflows/release.yml). A successful run builds a Windows x64 package and publishes it with release notes at [GitHub Releases](https://github.com/rthomson83/380-taxi-cam/releases).

The workflow can also be started manually from GitHub Actions on `main`.

## Build pipeline

1. Check out the exact commit that triggered the run.
2. Download or restore the pinned compiler archive and verify its hash.
3. Compile the native EXE and DLL and run validation.
4. Test installation, rollback and package contents using isolated fixtures.
5. Create the ZIP and retain diagnostic artifacts.
6. Create a draft release, upload its assets, then publish it.

The runner is `windows-2025`. Official actions are pinned to commit SHAs, and the compiler archive is pinned in `dependencies.json`.

The validation step runs:

~~~powershell
./build.ps1 -Validate -WarpOnly
./smoke-test.ps1
./standalone/install_test.ps1
./standalone/package_test.ps1
~~~

`-WarpOnly` uses Windows' software Direct3D 12 renderer to exercise GPU capture, composition and PFD drawing. The receipt records WARP as `passed`, hardware GPU validation as `not-run` and `simulatorVerified: false`. Local `build.ps1 -Validate` runs both hardware and WARP tests.

## What to download

| Release asset | Contents |
| --- | --- |
| Windows x64 ZIP | EXE, DLL, install/uninstall scripts, settings defaults, notices, documentation, validation receipt and file manifest |
| `SHA256SUMS.txt` | SHA-256 checksum of the ZIP |
| `validation.json` | Test coverage and exact binary hashes |

The ZIP also contains `build-info.json` with the application version, build label and source commit. Extract the ZIP before running the installer. Installation instructions are in the [README](../README.md#install).

## Tags and release notes

Tags use `v<application-version>-build.<workflow-run-number>`, such as `v0.8.0-build.2`. The application version comes from the validation receipt.

Notes contain installation guidance, validation scope, a link to build logs and commits since the most recent published ancestor build. GitHub's generated notes add pull-request and contributor information. The first release includes the available commit history.

[The publication script](../ci/publish-release.ps1) targets the exact built commit. It marks a release Latest only when that commit is still the current `main` at publication time.

## Failures and reruns

Build, validation or packaging failure prevents release publication. Workflow artifacts retain logs and available package output for 30 days.

Publication keeps the release in draft until all three assets are uploaded. A rerun can finish an incomplete draft. If the release is already published, the script preserves its assets and returns its link. Reruns keep the same workflow run number and tag.

Independent pushes each receive a build; they are not cancelled by a newer push. A newer push can arrive during publication, so the Latest check is not a transactional lock.

The release job uses `contents: write` and GitHub's built-in token. No personal access token or additional repository secret is required.

CI validates the build and isolated rendering pipeline. Hardware GPU behaviour and live MSFS operation require their own checks.
