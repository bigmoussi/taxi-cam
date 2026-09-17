# Windows builds and approved releases

The [Windows build and release workflow](../.github/workflows/release.yml) runs on pushes to `main` that change application source, tests, diagnostic tools, installer or CI code, the root build scripts, dependency or version configuration, camera defaults, the project licence, bundled runtime notices, or the release workflow itself. A successful run retains a downloadable Windows x64 release candidate for checking. Publication waits for your approval in the same workflow run.

README, documentation, issue-template and other repository-only changes do not trigger an automatic build. Markdown files and formatting/ignore files are excluded even inside the code directories. A push containing both documentation and a qualifying code change still runs. Keep the workflow's `paths` list current when adding a new build input outside the listed directories.

The build workflow can also be started manually from GitHub Actions. Only successful builds from `main` can be published.

## Download, check, then publish

1. Open the **Windows build and release** run in GitHub Actions. Wait for **Build and validate** to finish; **Publish release** will show **Waiting** for approval.
2. Download `windows-release-<build-number>-attempt-<attempt>` from that run's Artifacts section. Extract it and test the setup EXE in `packages/`. The runtime ZIP is beside it; `native/` contains the validated binaries and receipt.
3. Return to the same run and click **Review deployments**. Select **release**, then **Approve and deploy**.

The publish job downloads the exact candidate produced by the build job and checks its commit, build number and binary hashes before publishing to [GitHub Releases](https://github.com/rthoms334/taxi-cam/releases). It does not compile or repackage the application. There is no second workflow or run-ID form. If testing fails, reject the deployment or cancel the run.

The `release` repository environment must have **Required reviewers** enabled with `rthoms334` as reviewer. **Prevent self-review** is disabled so you can approve your own builds, and administrator bypass is disabled. These settings live in **Settings > Environments > release**, not in YAML; keep them enabled to preserve the gate. GitHub pauses the publish job before starting its runner until approval is granted.

Candidates expire after 30 days. If a candidate is missing or expired, build and check a new one. A full rerun creates a new candidate that needs checking and approval again. Rerunning only a failed publish job uses the original build job's artifact name, even though the workflow attempt number changes.

## Build pipeline

1. Check out the exact commit that triggered the run.
2. Download or restore the pinned compiler archive and verify its hash.
3. Compile the native EXE and DLL and run validation.
4. Test installation, rollback and package contents using isolated fixtures.
5. Create the minimal runtime ZIP and compile the Windows installer with pinned Inno Setup.
6. Test the installer using isolated fixtures and retain diagnostic artifacts.
7. Retain a release candidate, then wait for approval before the publish job starts.

The runner is `windows-2025`. Official actions are pinned to commit SHAs. The native compiler archive and Inno Setup compiler download are pinned in `dependencies.json` and verified before use.

The validation step runs:

~~~powershell
./build.ps1 -Validate -WarpOnly
./smoke-test.ps1
./tests/installer/prerequisites_test.ps1
./tests/installer/install_test.ps1
./tests/installer/settings_test.ps1
./tests/installer/package_test.ps1
~~~

`-WarpOnly` uses Windows' software Direct3D 12 renderer to exercise GPU capture, composition and PFD drawing. The receipt records WARP as `passed`, hardware GPU validation as `not-run` and `simulatorVerified: false`. Local `build.ps1 -Validate` runs both hardware and WARP tests.

After packaging and compiling setup, `tests/installer/test-installer.ps1 -Installer <setup-path>` verifies the exact installer and its receipt in isolated fixtures. Package checks require the complete five-file payload and byte-for-byte copies of the project licence and third-party notices. Installer checks cover those files during installation, update, rollback and removal, including default settings preservation, the one-shot camera-rate write to 5, explicit reset/removal, and restoration of settings when setup fails. Windows shell extraction checks verify the app and setup icons at small and large sizes.

## Saved settings

Installation and uninstallation **keep settings by default**, including unattended runs. Setup's **Keep existing settings** checkbox starts selected on every run. The first keep-install of this version writes `camera_rate=5` into existing `settings.ini` and known aircraft profile INIs (the minimum and shipped default; range 5–60) and records `camera_rate_revision=1` on `settings.ini`. Later upgrades leave a user-changed rate (10/15/30) alone. Calibration, mounts, hotkeys and other keys stay in place. Clearing the checkbox resets camera profiles, reference guides, display preferences, hotkeys and first-launch state, and uses the bundled camera defaults. Known profiles under the former `380 Taxi Cam` name are also cleared so they cannot be imported again. Uninstall offers **Keep settings** first, with **Remove saved settings** as the explicit alternative. Neither choice deletes logs or unrelated files.

For unattended operations, setup accepts `/RESETSETTINGS=1` and the uninstaller accepts `/REMOVESETTINGS=1`. Omitting these parameters preserves settings except for the one-shot camera-rate migrate above. The corresponding source scripts expose `install.ps1 -ResetSettings` and `uninstall.ps1 -RemoveSettings`. Reset/removal requires all Taxi Cam companions and MSFS to be closed. Setup snapshots affected settings and restores them on failure; concurrent changes are retained and reported rather than overwritten by rollback.

## What to download

| Release asset | Contents |
| --- | --- |
| Windows x64 setup EXE | Guided installation, upgrade and uninstallation, with simulator startup integration |
| Windows x64 ZIP | EXE, DLL, camera defaults, GPLv3 licence and third-party runtime notices |
| `SHA256SUMS.txt` | SHA-256 checksums of setup and the ZIP |
| `validation.json` | Test coverage and exact binary hashes |

The runtime ZIP contains exactly five files in its application directory: `taxi-cam.exe`, `taxi-camera-bridge.dll`, `taxi-camera-mounts.cfg`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`. It has no loose PowerShell scripts, documentation folders, build manifests or validation receipts. Use setup for a normal installation.

Original Taxi Cam code is licensed under GPLv3 only. Release notes link to the corresponding source archive at the exact built commit, including the build and installation scripts; GitHub also provides source archives for the release tag. Keep that source accessible to binary recipients. The installer places `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` alongside the application, and third-party licensing remains separate.

Provenance and manifests remain beside the ZIP in the ignored build directory as `<package>.build-info.json` and `<package>.manifest.json`, and are retained in workflow artifacts. The installer adds its uninstaller and installation record, which are needed to manage future upgrades and removal. Build, test and installation scripts remain in the repository.

## Tags and release notes

`version.json` defines the semantic version baseline, starting at `0.8.1`. The commit introducing that value uses the baseline exactly. Each subsequent first-parent commit on `main` adds one to its patch component: `0.8.1`, `0.8.2`, `0.8.3`, and so on. A merge counts once; a push containing several direct commits advances by their count. Documentation commits still count towards version calculation, but no longer trigger a release; the next code release may therefore skip patch numbers. Formatting the configuration without changing its version value does not reset the baseline.

For a minor or major release, deliberately change the baseline in `version.json` to the required version, such as `0.9.0` or `1.0.0`. That commit establishes a new baseline. Local builds and GitHub Actions calculate the same semantic version from the same complete Git history. A local, uncommitted baseline change uses its configured version. Shallow checkouts are refused because they cannot establish the version reliably.

The generated header and Windows manifest live under `build/native/generated/`; the resolved version and baseline commit are recorded in `build/native/version.json`. The app UI, executable version resources, installer and release title all use this resolved semantic version.

Tags retain `v<application-version>-build.<workflow-run-number>`, such as `v0.8.1-build.13`, so existing updaters continue to recognize releases. The build suffix identifies an artifact; it does not replace the advancing semantic version. The workflow run number is recorded in the validation receipt, while local builds use build number zero. Packaging refuses a `build.N` label that differs from the compiled build number.

Installer assets use `taxi-cam-<version>-windows-x64-setup.exe`, such as `taxi-cam-0.9.0-windows-x64-setup.exe`. The build number remains in the release tag, runtime ZIP name and validation receipts for traceability. Existing published downloads keep their original names.

The updater compares version components and build numbers numerically, ignores drafts and prereleases, and selects the exact installer filename for the release from the fixed project repository. It prefers `taxi-cam-<version>-windows-x64-setup.exe` and also recognizes `taxi-cam-<version>-build.<number>-windows-x64-setup.exe`. Downloads are bounded and verified before execution. Automatic checks run off the UI thread, with a manual tray action available; installing requires user confirmation and a closed simulator.

The updater uses GitHub's unauthenticated public release API. The release repository and its installer assets must be publicly readable; private repositories return HTTP 404 to this client. No GitHub credential is embedded in the application. Make the repository public and publish a release containing setup before automatic updates can be used by end users. Local installation and the isolated updater tests work independently of repository visibility.

Notes contain installation guidance, GPLv3 licence information, links to the exact corresponding source and build logs, validation scope, and commits since the most recent published ancestor build. GitHub's generated notes add pull-request and contributor information. The first release includes the available commit history.

[The publication script](../ci/publish-release.ps1) targets the exact built commit. It marks a release Latest only when that commit is still the current `main` at publication time.

## Failures and reruns

Build, validation or packaging failure prevents creation of a publishable candidate. Workflow artifacts retain logs and available package output for 30 days. The publish job depends on a successful build and only runs from `main` after environment approval.

Publication keeps the release in draft until all four assets are uploaded. Use **Re-run failed jobs** to retry a failed publication and finish an incomplete draft; approve the deployment again if prompted. If the release is already published, the script preserves its assets and returns its link. Release tags and filenames use the shared workflow run number. Rebuilding an already published build number cannot replace its release assets.

Independent pushes each receive a build; they are not cancelled by a newer push. A newer push can arrive during publication, so the Latest check is not a transactional lock.

The build job uses read-only repository permissions. Only the approval-gated publish job uses `contents: write`; it also uses `actions: read` to retrieve the candidate from the same run. Both use GitHub's built-in token. No personal access token or additional repository secret is required.

CI validates the build and isolated rendering pipeline. Hardware GPU behaviour and live MSFS operation require their own checks.
