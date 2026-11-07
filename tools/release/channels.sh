#!/usr/bin/env bash
# Signed channel manifest operations behind the release workflows. See
# docs/releasing.md for the routine and for local rehearsals.
#
#   channels.sh publish-beta <A.B.C.D> <entry.json>
#   channels.sh promote <A.B.C.D>
#   channels.sh resign
#   channels.sh revoke <A.B.C.D>
#
# Environment:
#   TOOL                     parties_manifest_tool.exe (required)
#   REPO                     owner/name, default $GITHUB_REPOSITORY
#   PARTIES_UPDATE_KEY       hex Ed25519 secret key that signs
#   PARTIES_UPDATE_KEY_NEXT  optional second key that also signs, during a rotation
#   CHANNELS_DIR             read and write manifests in this directory instead of the channels release
#   DRY_RUN=1                print the release edits instead of making them
#   TOOL_ARGS                extra tool arguments, such as "--keyring <dir>" with a testing build
#   SKIP_ASSET_CHECK=1       skip downloading assets, for rehearsals whose urls do not exist
#   SIGNED_MANIFEST_OUT      publish-beta also copies the manifest it published here, for setup
#
# Every command writes only under a scratch directory until its last step,
# which uploads one manifest with --clobber: the commit point. Rerunning any
# command after a failure converges, because each run starts from what is
# published and takes a new, higher serial.
set -euo pipefail

readonly KEEP=5
readonly EXPIRES_DAYS=45
readonly CHANNELS_TAG=channels

: "${TOOL:?TOOL must point at parties_manifest_tool.exe}"
REPO=${REPO:-${GITHUB_REPOSITORY:-}}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

die() { echo "::error::$*" >&2; exit 1; }
notice() { echo "::notice::$*"; }

tool() { "$TOOL" "$@" ${TOOL_ARGS:-}; }

gh_write() {
    if [ "${DRY_RUN:-0}" = 1 ]; then echo "dry run: gh $*"; else gh "$@"; fi
}

check_version() {
    [[ "$1" =~ ^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){3}$ ]] || die "version '$1' is not A.B.C.D"
}

signing_state() {
    if [ -z "$(tool pinned-keys)" ]; then
        notice "This commit pins no production update key (updater/keys/production), so no channel manifest is signed. See 'Set up the signing key the first time' in docs/releasing.md."
        return 1
    fi
    if [ -z "${PARTIES_UPDATE_KEY:-}" ]; then
        notice "PARTIES_UPDATE_KEY is not set in the update-signing environment, so no channel manifest is signed. See 'Set up the signing key the first time' in docs/releasing.md."
        return 2
    fi
    return 0
}

fetch() {
    local channel=$1 dir="$WORK/published"
    mkdir -p "$dir"
    if [ -n "${CHANNELS_DIR:-}" ]; then
        if [ -f "$CHANNELS_DIR/$channel.manifest.json" ]; then
            cp "$CHANNELS_DIR/$channel.manifest.json" "$dir/"
            echo "$dir/$channel.manifest.json"
        else
            echo none
        fi
        return
    fi
    local assets
    if ! assets=$(gh release view "$CHANNELS_TAG" -R "$REPO" --json assets --jq '.assets[].name' 2>"$WORK/gh.err"); then
        if grep -q "release not found" "$WORK/gh.err"; then echo none; return; fi
        cat "$WORK/gh.err" >&2
        die "cannot read the $CHANNELS_TAG release"
    fi
    if grep -qx "$channel.manifest.json" <<<"$assets"; then
        gh release download "$CHANNELS_TAG" -R "$REPO" -p "$channel.manifest.json" -D "$dir" --clobber >&2
        echo "$dir/$channel.manifest.json"
    else
        echo none
    fi
}

# The channels release holds only manifests and must never become the latest
# release: the legacy in-app updater and Sparkle read /releases/latest.
ensure_channels_release() {
    local prerelease
    if prerelease=$(gh release view "$CHANNELS_TAG" -R "$REPO" --json isPrerelease --jq .isPrerelease 2>/dev/null); then
        [ "$prerelease" = true ] || gh_write release edit "$CHANNELS_TAG" -R "$REPO" --prerelease
    else
        gh_write release create "$CHANNELS_TAG" -R "$REPO" --prerelease --latest=false --title "Update channels" \
            --notes "Signed channel manifests for the Windows update manager. Never mark this release as latest."
    fi
}

sign_and_check() {
    local payload=$1 envelope=$2 channel=$3
    local keys=(--key-env PARTIES_UPDATE_KEY)
    if [ -n "${PARTIES_UPDATE_KEY_NEXT:-}" ]; then keys+=(--key-env PARTIES_UPDATE_KEY_NEXT); fi
    "$TOOL" sign --payload "$payload" "${keys[@]}" --out "$envelope"
    local check=(--check-assets)
    if [ "${SKIP_ASSET_CHECK:-0}" = 1 ]; then check=(); fi
    tool verify --envelope "$envelope" --channel "$channel" "${check[@]}"
}

publish() {
    local envelope=$1
    if [ -n "${CHANNELS_DIR:-}" ]; then
        cp "$envelope" "$CHANNELS_DIR/"
        echo "wrote $CHANNELS_DIR/$(basename "$envelope")"
        return
    fi
    ensure_channels_release
    gh_write release upload "$CHANNELS_TAG" -R "$REPO" "$envelope" --clobber
}

newer() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$1" ]
}

cmd_publish_beta() {
    local version=$1 entry=$2
    check_version "$version"
    grep -q "^{\"version\":\"$version\"," "$entry" || die "$entry is not the entry for $version"
    signing_state || return 0
    local base
    base=$(fetch beta)
    tool merge --base "$base" --entry "$entry" --channel beta --serial now --expires-days "$EXPIRES_DAYS" \
        --keep "$KEEP" --out "$WORK/beta.payload.json"
    sign_and_check "$WORK/beta.payload.json" "$WORK/beta.manifest.json" beta
    publish "$WORK/beta.manifest.json"
    if [ -n "${SIGNED_MANIFEST_OUT:-}" ]; then cp "$WORK/beta.manifest.json" "$SIGNED_MANIFEST_OUT"; fi
}

cmd_promote() {
    local version=$1 tag="v$1" state=0
    check_version "$version"
    gh release view "$tag" -R "$REPO" --json tagName >/dev/null || die "there is no release $tag"
    signing_state || state=$?
    if [ "$state" = 2 ]; then die "set PARTIES_UPDATE_KEY before promoting; production keys are pinned"; fi
    if [ "$state" = 0 ]; then
        local beta stable
        beta=$(fetch beta)
        [ "$beta" != none ] || die "there is no beta manifest; publish $version to Beta first"
        stable=$(fetch stable)
        tool promote --from "$beta" --to "$stable" --version "$version" --serial now --expires-days "$EXPIRES_DAYS" \
            --keep "$KEEP" --out "$WORK/stable.payload.json"
        sign_and_check "$WORK/stable.payload.json" "$WORK/stable.manifest.json" stable
        publish "$WORK/stable.manifest.json"
    else
        notice "The stable manifest is not updated; $tag still becomes a full release for the legacy updater."
    fi
    local latest
    latest=$(gh release view -R "$REPO" --json tagName --jq .tagName 2>/dev/null || true)
    if [ -n "$latest" ] && newer "${latest#v}" "$version"; then
        notice "$latest is newer than $tag and stays the latest release."
        gh_write release edit "$tag" -R "$REPO" --prerelease=false --latest=false
    else
        gh_write release edit "$tag" -R "$REPO" --prerelease=false --latest
    fi
}

cmd_resign() {
    local state=0
    signing_state || state=$?
    if [ "$state" = 1 ]; then return 0; fi
    if [ "$state" = 2 ]; then die "set PARTIES_UPDATE_KEY; published manifests expire $EXPIRES_DAYS days after their last signature"; fi
    local channel base
    for channel in beta stable; do
        base=$(fetch "$channel")
        if [ "$base" = none ]; then
            notice "There is no $channel manifest yet; nothing to re-sign."
            continue
        fi
        tool resign --base "$base" --channel "$channel" --serial now --expires-days "$EXPIRES_DAYS" \
            --out "$WORK/$channel.payload.json"
        sign_and_check "$WORK/$channel.payload.json" "$WORK/$channel.manifest.json" "$channel"
        publish "$WORK/$channel.manifest.json"
    done
}

# Always both channels: a setup install follows Stable even when its build is
# only on Beta, so a Stable manifest that does not exist yet is created.
cmd_revoke() {
    local version=$1 state=0
    check_version "$version"
    signing_state || state=$?
    if [ "$state" = 2 ]; then die "set PARTIES_UPDATE_KEY before revoking; production keys are pinned"; fi
    if [ "$state" = 0 ]; then
        local channel beta stable
        beta=$(fetch beta)
        stable=$(fetch stable)
        tool revoke --beta "$beta" --stable "$stable" --version "$version" --serial now \
            --expires-days "$EXPIRES_DAYS" --out-dir "$WORK"
        for channel in beta stable; do
            sign_and_check "$WORK/$channel.payload.json" "$WORK/$channel.manifest.json" "$channel"
        done
        for channel in beta stable; do publish "$WORK/$channel.manifest.json"; done
    fi
    # A prerelease is never /releases/latest, so the legacy updater and
    # Sparkle stop offering the version too.
    if gh release view "v$version" -R "$REPO" --json tagName >/dev/null 2>&1; then
        gh_write release edit "v$version" -R "$REPO" --prerelease
    fi
}

command=${1:-}
shift || true
case "$command" in
    publish-beta) [ $# = 2 ] || die "usage: channels.sh publish-beta <A.B.C.D> <entry.json>"; cmd_publish_beta "$@" ;;
    promote) [ $# = 1 ] || die "usage: channels.sh promote <A.B.C.D>"; cmd_promote "$@" ;;
    resign) [ $# = 0 ] || die "usage: channels.sh resign"; cmd_resign ;;
    revoke) [ $# = 1 ] || die "usage: channels.sh revoke <A.B.C.D>"; cmd_revoke "$@" ;;
    *) die "usage: channels.sh publish-beta|promote|resign|revoke ..." ;;
esac
