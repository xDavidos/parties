# Releasing Parties

Every `vA.B.C.D` tag builds and signs the clients, publishes a GitHub prerelease, and adds the Windows build to the Beta update channel. Promotion later copies that exact build entry into the Stable channel and turns the GitHub release into the latest release. A weekly job re-signs both channels so they never expire.

The Windows launcher reads two signed manifests, `beta.manifest.json` and `stable.manifest.json`, from the GitHub release tagged `channels`. It accepts a manifest only if a key pinned in its own build signed it. Before promotion, a release is a prerelease, and `/releases/latest` never returns a prerelease. The legacy in-app updater on Windows and Sparkle on macOS both read `/releases/latest`, so they offer a version only after you promote it.

## Ship a release

1. Push the tag, for example `git tag v1.6.0.0` and then `git push upstream v1.6.0.0`.
2. Wait for the **Release** run. Its `publish-beta` job waits for a reviewer of the `update-signing` environment. Approve it.
3. Launchers that follow Beta offer the build at their next check.
4. To promote, open **Actions**, select **Promote to Stable**, select **Run workflow**, and enter the version without the `v`, for example `1.6.0.0`. Approve the `promote` job.

Promotion writes `stable.manifest.json` first and then marks the release as a full, latest release. If a newer release is already the latest one, promotion marks the version as a full release and leaves the latest release alone. If promotion fails after it wrote the manifest, run it again. A second run writes the same entry with a higher serial.

Only four-part tags get an update layout. A tag such as `v1.6.0` still publishes the zip, the server, and the macOS DMG, with a warning that the release has no update layout.

## Set up the signing key the first time

Until a production key is pinned and its secret is set, releases publish as before, `publish-beta`, `setup` and **Re-sign channels** end with a notice, and **Promote to Stable** only marks the release as latest. No release has a `PartiesSetup-A.B.C.D.exe` until then, because a setup whose manifest no launcher can verify is never shipped. A build pins the keys that its commit holds, so the first build that can receive signed updates is the first tag after the key commit.

1. Build the tool on a machine you trust: `cmake --build --preset release --target parties_manifest_tool`.
2. Generate the key: `build-release/updater/tools/parties_manifest_tool.exe keygen --out-secret update-key.secret --out-public update-key.pub`. The tool prints the key id.
3. In the repository settings, create the environment `update-signing`. Add required reviewers. Under deployment branches and tags, allow only `main` and tags that match `v*`.
4. Add the environment secret `PARTIES_UPDATE_KEY` with the contents of `update-key.secret`.
5. Store `update-key.secret` in your password manager, then delete the file. The repository must never hold it.
6. Move `update-key.pub` to `updater/keys/production/<key id>.pub`, commit it, and merge it to `main`. A configure step now prints `Pinned production update keys: 1` instead of a warning.
7. Tag the next release. Its `publish-beta` job creates the `channels` prerelease and its first `beta.manifest.json`.

The secret file holds the 32-byte Ed25519 private key (the RFC 8032 seed) as 64 lowercase hex characters. The public file holds the 32-byte public key the same way. The key id is the first 16 characters of the public key's hex.

## Pin the Authenticode publisher

The signed manifest is what makes a launcher trust a download. As a second, independent check, a launcher can also require that every executable it downloads carries a valid Authenticode signature from your publisher. Revocation is checked along the whole certificate chain. A launcher built without a publisher skips this check.

1. Read the signer's name from a signed release asset: `(Get-AuthenticodeSignature parties_client.exe).SignerCertificate.Subject`. The publisher is the value after `CN=`, for example `Parties Project`.
2. In the repository settings, under **Secrets and variables**, **Actions**, **Variables**, add the repository variable `PARTIES_AUTHENTICODE_PUBLISHER` with that exact value. The **Release** workflow passes it to CMake as `-DPARTIES_AUTHENTICODE_PUBLISHER=...`.
3. Tag the next release. Its launcher refuses any downloaded executable that is unsigned, has a revoked or untrusted certificate, or was signed by anyone else, and it keeps running the installed build.

Before you move to a certificate with a different common name, clear the variable and ship one release that is still signed with the old certificate. Launchers that update to it no longer pin a publisher. Once users run it, switch the certificate and set the variable to the new name.

## Rotate the key

A launcher verifies a manifest if at least one signature comes from a key its build pins, and every signature from a pinned key verifies. A build pins at most two keys. To move to a new key without stranding any installed build:

1. Generate the new key with `keygen`, as in the first-time setup.
2. Commit its public file next to the current one in `updater/keys/production/`. Configure refuses a third key.
3. Add the new secret to `update-signing` as `PARTIES_UPDATE_KEY_NEXT`. From now on every signing job signs with both keys.
4. Keep releasing until no supported build pins only the old key. Promote at least one build that pins both keys to Stable.
5. Replace `PARTIES_UPDATE_KEY` with the new secret, delete `PARTIES_UPDATE_KEY_NEXT`, delete the old public file, and run **Re-sign channels**.

If the old key leaked, do the same steps without waiting in step 4 for longer than it takes users to update. A build that pins only the leaked key trusts it until that build updates.

## Revoke a build

Revoke a build that passed its health check but is still bad, for example one that damages data or breaks calls. The launcher already rolls back a build that crashes or hangs before it reports healthy, so a crash on startup needs no revocation.

1. Open **Actions**, select **Revoke a build**, and select **Run workflow**.
2. Enter the version. Approve the `revoke` job.

The workflow always revokes from both channels. It removes the build from Beta and Stable and lists its version under `revoked` in each, creating the Stable manifest if none exists yet. A revocation from one channel only would miss installs that follow the other one, and every `PartiesSetup-A.B.C.D.exe` installs a Beta build on the Stable channel. A launcher that runs a revoked build moves to the newest build it can run, even if that build is older. A revoked version can never return to either channel, so ship the fix under a new version. The workflow also turns the version's GitHub release back into a prerelease, so the legacy updater and Sparkle stop offering it. After that, check which release GitHub shows as **Latest**, and mark the right one if needed.

## When the weekly re-sign fails

Each manifest expires 45 days after it was signed. **Re-sign channels** runs every Monday and gives both channels a new serial and expiry without changing their builds. Each run waits for a reviewer of `update-signing`, so someone has to approve it every week.

If no re-sign succeeds for 45 days, launchers treat the channel as stale. They stop offering updates and report `feed_stale`. Installed builds keep running, and rollback to the previous build keeps working, because neither needs the network. To recover, run **Re-sign channels** by hand. The next valid manifest restores update offers.

The re-sign also downloads every asset that a manifest names and checks its size and SHA-256. A failure there means a release asset changed or disappeared. Revoke the affected build, or restore the asset from the Release run's artifacts.

GitHub disables scheduled workflows in a repository with no activity for 60 days. If **Re-sign channels** shows as disabled, enable it on its **Actions** page.

## Retry a Beta publish

If `publish-beta` failed or ended with the notice because the key was not ready, run **Publish to Beta** by hand. Enter the version and the ID of the Release run that built it, which is the number at the end of the run's URL. The job uses that run's artifacts, which GitHub keeps for 90 days.

A hand-run **Publish to Beta** does not build `PartiesSetup-A.B.C.D.exe`. Only the `setup` job of the Release run does, right after its own `publish-beta` job. Afterwards, rerun only the failed `setup` job of that Release run. Do not rerun all of its jobs.

Do not rerun all jobs of a Release run for a version that is already in a channel. The rebuild produces new bytes and replaces the release assets, so the published entry no longer matches them. Launchers then refuse the download, and the next re-sign fails its asset check. Tag a new version instead.

## Rehearse locally

`tools/release/channels.sh` runs every channel operation, and two variables keep it away from real releases. `CHANNELS_DIR` reads and writes manifests in a local directory instead of the `channels` release. `DRY_RUN=1` prints each release edit instead of making it. A Debug build configured with `-DPARTIES_UPDATE_TESTING=ON` adds the tool's `--keyring <dir>` option, which replaces the pinned keys with the `*.pub` files in a directory.

```bash
tool=build/updater/tools/parties_manifest_tool.exe
mkdir -p rehearsal/ring rehearsal/channels
$tool keygen --out-secret rehearsal/secret --out-public rehearsal/ring/test.pub
pwsh -File tools/release/stage-update-layout.ps1 -Tag v1.6.0.0 -Repository emcifuntik/parties \
	-BuildDir build -OutDir rehearsal/dist -Launcher <any exe that stands in for the launcher>
export TOOL=$tool REPO=emcifuntik/parties CHANNELS_DIR=$PWD/rehearsal/channels DRY_RUN=1 \
	TOOL_ARGS="--keyring $PWD/rehearsal/ring" SKIP_ASSET_CHECK=1 PARTIES_UPDATE_KEY=$(cat rehearsal/secret)
bash tools/release/channels.sh publish-beta 1.6.0.0 rehearsal/dist/entry.json
bash tools/release/channels.sh promote 1.6.0.0
bash tools/release/channels.sh resign
bash tools/release/channels.sh revoke 1.6.0.0
```

The tag must match the version the build reports, which comes from `git describe`. `promote` reads the real release named by `REPO`, so that release must exist. `SKIP_ASSET_CHECK=1` skips the downloads, because the rehearsal's URLs point at assets that were never uploaded.

To build a setup locally, run `cmake --build build --target PartiesSetup` in a build configured with `-DPARTIES_UPDATE_TESTING=ON`. It packs that build's stub, launcher and client with a manifest signed by the test harness key, which only testing launchers trust. To pack other files, set `-DPARTIES_SETUP_PAYLOAD_DIR=<dir>`. The directory holds `manifest.json`, `Parties.exe`, and every file of one build that the manifest lists. The `setup` job runs the same packing, `PartiesLauncher.exe --pack=<dir> --out=<exe>`, directly on the release's signed files.

## Reference

### Release assets

Each `vA.B.C.D` release holds these Windows assets. The server and debug symbol archives are unchanged from before the update channels.

| Asset | Contents |
| --- | --- |
| `parties-client-windows-vA.B.C.D.zip` | `parties_client.exe`, which is `PartiesSetup-A.B.C.D.exe` under the name the legacy in-app updater installs. A release without a signed setup keeps the client itself here. |
| `parties-server-windows-vA.B.C.D.zip` | `parties_server.exe` |
| `parties-pdb-windows-vA.B.C.D.zip` | Debug symbols |
| `PartiesLauncher-A.B.C.D.exe` | The launcher of the build, named `PartiesLauncher.exe` in the build directory |
| `parties_client-A.B.C.D.exe` | The app of the build, named `parties_client.exe` in the build directory |
| `Parties-A.B.C.D.exe` | The boot stub, for setup. Updates never replace it. |
| `PartiesSetup-A.B.C.D.exe` | The installer. It installs or repairs per user, with no administrator rights, in `%LOCALAPPDATA%\Programs\Parties`. |

`PartiesSetup-A.B.C.D.exe` is the launcher with the build, the stub and the signed Beta manifest embedded as resources. Before it writes anything, it verifies that manifest against its pinned keys and every embedded file against its digest. The `setup` job packs it after `publish-beta` has signed the manifest, then signs it and verifies its Authenticode signature. `--silent` installs without any dialog, and `--no-launch` skips starting Parties afterwards. Windows lists Parties under **Settings > Apps > Installed apps**. Uninstalling it from there keeps `%LOCALAPPDATA%\Parties` unless the user chooses to move it to the Recycle Bin.

Portable users reach the installer through the legacy in-app updater once a release with a signed setup becomes **Latest**. The updater swaps the zip's `parties_client.exe`, which is the setup, in for the running app, keeps the old exe as `parties_client.old.exe`, and starts the setup with `--update-cleanup`. The setup waits for the old app to exit, then copies the portable `parties_client.db` into `%LOCALAPPDATA%\Parties\data` before it writes any shortcut, and keeps another copy in `backups\legacy-*`. If `data` already has a database, only the copy in `backups` is made, and the setup says where it is. The setup then deletes `parties_client.old.exe` and starts Parties. The portable folder keeps the setup as `parties_client.exe`, so old shortcuts start the installed Parties. A setup run by hand also looks for portable data, in its working folder, its own folder, the folders that the Start menu and Desktop `Parties.lnk` start from, and the folders of running portable copies.

The launcher, app and stub assets appear once the launcher target builds `build-retail/updater/launcher/PartiesLauncher.exe`. The stub comes from `build-retail/updater/boot/Parties.exe`. Both paths are set once, in the `windows` job of `.github/workflows/release.yml`. All Windows executables carry the Authenticode signature from the signing service.

### Channel manifests

The `channels` release is a prerelease that holds `beta.manifest.json` and `stable.manifest.json` and nothing else. Uploading a manifest is the last step of every job, and it replaces the previous file. A launcher that downloads during the replacement sees no manifest and retries at its next check.

- The serial is the signing time in Unix seconds. The tool refuses a serial that does not exceed the published one.
- Each channel keeps its five newest builds. For every launcher link protocol, it also keeps the newest build that a launcher speaking that protocol can stage, so no installed launcher loses its way forward.
- An entry records the version, data schema, data compatibility floor, link protocols, and each file's size, SHA-256, and URL. The data schema and link protocols come from `parties_client.exe --write-build-info <path>`.

### Workflows

| Workflow | Trigger | Signs | Other effect |
| --- | --- | --- | --- |
| **Release** (`release.yml`) | `v*` tag | Beta, through `publish-beta.yml` | Creates the prerelease and its assets, then `PartiesSetup-A.B.C.D.exe` |
| **Publish to Beta** (`publish-beta.yml`) | Called by Release, or by hand with a run ID | Beta | None |
| **Promote to Stable** (`promote.yml`) | By hand | Stable | Marks the release as a full, latest release |
| **Re-sign channels** (`resign.yml`) | Mondays 06:17 UTC, or by hand | Beta and Stable | None |
| **Revoke a build** (`revoke.yml`) | By hand | Beta and Stable | Makes the release a prerelease again |

Every job that signs runs in the `update-signing` environment, one at a time across all workflows (concurrency group `update-channels`). Promote, re-sign, and revoke build the tool in a separate job without secrets. Publish to Beta uses the tool that the Release run built, which pins the same keys as the launcher it publishes.

### `parties_manifest_tool`

`parties_manifest_tool` with no arguments prints its usage. Every command that reads a published manifest first verifies it against the pinned keys and refuses to continue if it does not verify.

| Command | Effect |
| --- | --- |
| `keygen` | Writes a new secret key and public key. Refuses to overwrite a file. |
| `entry` | Writes one build entry from `build-info.json` and the files of the build. |
| `merge` | Adds or replaces an entry in a channel payload. |
| `promote` | Copies the Beta entry for a version into the Stable payload. |
| `revoke` | Removes a version from the Beta and Stable payloads and lists it as revoked in both. `none` stands for a channel with no manifest yet. |
| `resign` | Writes the same payload with a new serial and expiry. |
| `sign` | Signs a payload with the keys in the named environment variables. |
| `verify` | Verifies an envelope and its expiry. `--check-assets` also downloads every URL and compares size and SHA-256. |
| `pinned-keys` | Prints the id of each pinned key. |
