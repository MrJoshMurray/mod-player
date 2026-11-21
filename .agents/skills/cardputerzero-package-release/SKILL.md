---
name: cardputerzero-package-release
description: Configure, build, verify, package, deploy, or prepare upload metadata for the CardputerZero Template application. Use for CMake presets, aarch64 Debian artifacts, app-builder.json matching, and release diagnostics; do not use for ordinary source edits.
---

# CardputerZero Package Release

Produce a release artifact that matches the final upload contract. Read
[references/release-contract.md](references/release-contract.md) before packaging or upload work.

## Workflow

1. Inspect `git status` and preserve existing changes. Never clean caches or build directories as
   a first response to a build failure.
2. Select an existing configure/build preset for the current host. Use
   `cmake --workflow --preset cp0-cross-package` for the complete aarch64 Debian flow.
3. Treat configure-time downloads of LVGL or the BSP as network operations and surface them when
   the environment requires authorization.
4. Confirm the output is an ARM64 Debian package, not a desktop executable labeled as ARM64.
5. Run the manifest validator before upload:

   ```sh
   cmake -P .agents/skills/cardputerzero-package-release/scripts/validate_app_builder.cmake
   ```

   When a package exists, also pass its path:

   ```sh
   cmake -DPACKAGE_FILE=dist/<package>.deb \
     -P .agents/skills/cardputerzero-package-release/scripts/validate_app_builder.cmake
   ```

    When invoking the validator from another checkout, use an absolute package path. The
    validator rejects a package path that is relative to the wrong working directory.

6. Inspect the package with `dpkg-deb -f` and `dpkg-deb -c` when available. Verify executable,
   config, assets, APPLaunch entry, optional service, version, and architecture.
7. Update `app-builder.json` only when the feature set and release artifact are final. Its paths
   must resolve from the repository root and its permissions must describe shipped behavior.
8. Stop after producing and validating artifacts unless the user explicitly requested deployment
   or upload. Use the provided uploader and credentials only when they are actually available; do
   not invent an upload command or destination.

## Lessons From A Real App Build

- Replace all template `TODO` metadata before considering a project store-ready. Set `source_repo`,
   author, SPDX license, summary, description, permissions, categories, and localized store text.
   Keep the store share code stable; do not invent or rotate one after publication. A new project
   still needs a real share-code allocation before store upload.
- Store screenshots must be actual application content at exactly 320x170. A desktop simulator
   capture includes the Cardputer shell and window chrome, so crop the LVGL display before adding it
   to `store.screenshots`. Capture current receiver and Settings views in both dark and light themes
   when those themes are shipped. Do not retain stale template screenshots in the repository.
- The local package can keep the same version and revision during iterative device testing, but
   device installation must use `apt install --reinstall`; otherwise apt may report the package as
   current while leaving the previous binary in place. Verify the installed `/usr/bin/<app>` hash
   against the hash of the packaged executable.
- Preserve user-edited conffiles during deployment and inspect them before packaging. In particular,
   do not overwrite a user's theme preference merely to restore a repository default.
- For a direct radio/Ethernet test, inspect the active link-local interface first, then use the
   repository discovery and stream probes to confirm the endpoint and valid protocol packets before
   launching the full application. On desktop, `SQUARE_SDR_LIVE_RADIO=1` enables live IQ mode; the
   desktop audio sink is simulated, but device deployments still require hearing-safety precautions.
- Keep store preparation separate from publication. Build, test, validate metadata, and push source
   to the user's GitHub repository when requested, but do not upload to the Cardputer store until the
   user explicitly authorizes publication after hardware testing.
- When converting the template into a standalone app, replace the template Git remote with the new
   project remote after the initial commit, and remove generated CPack staging plus obsolete template
   screenshots before publishing the source repository.

## Failure Handling

- Report the failing phase: configure, compile, link, package, manifest validation, deploy, or
  upload.
- Do not delete `.cache/`, `build/`, or `dist/` without a demonstrated need and explicit approval.
- Do not deploy to a device until the remote user, host, directory, and exact package are known.

