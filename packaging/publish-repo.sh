#!/bin/sh
# Copyright 2026, David Beckman.
# All rights reserved. Distributed under the terms of the MIT License.

#
# Publish packaging/out-repo/ to the Indispensable Labs apps repository on
# Cloudflare R2.
#
# Runs wherever rclone is configured (WSL Ubuntu-24.04), NOT on Haiku. Copy
# out-repo/ off the Haiku box first.
#
#   ./packaging/publish-repo.sh          # dry run -- shows every object
#   ./packaging/publish-repo.sh --go     # actually upload
#   ./packaging/publish-repo.sh --go --prune   # also delete unreferenced pkgs
#
# ---------------------------------------------------------------------------
# THE UPLOAD ORDER IS LOAD-BEARING
# ---------------------------------------------------------------------------
#
# packages -> repo.info -> repo -> repo.sha256 LAST. repo.sha256 is what tells
# a client its cached index is stale, so it must not appear until everything it
# points at is already fetchable. Do NOT replace this with `rclone sync`.
#
# ---------------------------------------------------------------------------
# THE BUCKET NAME IS NOT PART OF THE URL
# ---------------------------------------------------------------------------
#
# rclone's S3 backend reads the FIRST path component after the colon as the
# BUCKET, not as a key prefix: `remote:a/b/c` means bucket `a`, key `b/c`.
# The bucket is `hpkg`, the custom domain is bound to that bucket, and
# `apps/...` is a key prefix inside it -- exactly as `media-os/...` is for the
# Media-OS repository. The two repositories share the bucket, the domain and
# the R2 token, and share nothing else.
#
# ---------------------------------------------------------------------------
# THIS PUBLISHES THE WHOLE REPOSITORY, NOT JUST BETON
# ---------------------------------------------------------------------------
#
# The apps repository holds more than one app (Beton and MidiMonitor as of
# 2026-08-29) and there is one shared index. build-repo.sh assembles that index
# from packaging/out/ plus everything already published, so out-repo/ is the
# complete repository and this script replaces the live one with it. Do not run
# this against an out-repo/ built with --no-pull unless dropping the other apps
# is what you actually mean to do. The `--prune` step below is what makes such
# a mistake irreversible.

set -e

PKGDIR=$(cd "$(dirname "$0")" && pwd)
SRC="$PKGDIR/out-repo"

# The remote is named for its first user (Media-OS) but is really "the hpkg
# bucket" -- an Object Read & Write token scoped to the whole bucket, so it can
# write this prefix too. No second token is needed.
# NOTE: it must have no_check_bucket set, or rclone tries to CreateBucket first
# and the bucket-scoped token cannot do that; the error names CreateBucket, not
# PutObject, which makes it look like a permissions bug.
RCLONE_REMOTE="${RCLONE_REMOTE:-mediaos-hpkg}"

BUCKET="hpkg"
# Must stay in step with BASEURL_ROOT in build-repo.sh. The INFO_URL gate below
# fails the publish if these two ever drift apart.
URL_PATH="apps/@ARCH@/current"
BASEURL_HOST="https://hpkg.indispensablelabs.com"

GO=0
PRUNE=0
for arg in "$@"; do
	case "$arg" in
		--go)    GO=1 ;;
		--prune) PRUNE=1 ;;
		*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

[ -d "$SRC" ] || { echo "ERROR: $SRC missing -- run build-repo.sh first." >&2; exit 1; }
for f in repo repo.info repo.sha256; do
	[ -f "$SRC/$f" ] || { echo "ERROR: $SRC/$f missing." >&2; exit 1; }
done
command -v rclone >/dev/null 2>&1 || { echo "ERROR: rclone not found." >&2; exit 1; }

# Take the architecture from the built repo.info rather than from this host --
# this script runs on Linux, where uname -m says nothing about what was built.
ARCH=$(awk '$1 == "architecture" { print $2 }' "$SRC/repo.info" | tr -d '"')
[ -n "$ARCH" ] || { echo "ERROR: no architecture in $SRC/repo.info" >&2; exit 1; }
URL_PATH=$(echo "$URL_PATH" | sed "s|@ARCH@|$ARCH|")
BASEURL="$BASEURL_HOST/$URL_PATH"
DEST="$RCLONE_REMOTE:$BUCKET/$URL_PATH"

# Refuse to publish an index that disagrees with its own checksum -- that would
# wedge every client until the next release.
HAVE=$(cd "$SRC" && sha256sum repo | awk '{print $1}')
WANT=$(cat "$SRC/repo.sha256")
if [ "$HAVE" != "$WANT" ]; then
	echo "ERROR: repo.sha256 does not match repo." >&2
	echo "  repo hashes to: $HAVE" >&2
	echo "  repo.sha256:    $WANT" >&2
	echo "Re-run build-repo.sh." >&2
	exit 1
fi

# Refuse to publish to a URL other than the one baked into repo.info. That
# value is what `pkgman add-repo` stores and every later refresh fetches; if it
# disagrees with where the bytes actually land, the repository works exactly
# once (at add-repo time, from the URL the user typed) and then 404s forever.
INFO_URL=$(awk '$1 == "baseurl" { print $2 }' "$SRC/repo.info" | tr -d '"')
if [ -z "$INFO_URL" ]; then
	echo "ERROR: no baseurl found in $SRC/repo.info." >&2
	exit 1
fi
if [ "$INFO_URL" != "$BASEURL" ]; then
	echo "ERROR: repo.info baseurl does not match the publish destination." >&2
	echo "  repo.info says:   $INFO_URL" >&2
	echo "  publishing to:    $BASEURL" >&2
	echo "Fix BASEURL_ROOT in build-repo.sh or BUCKET/URL_PATH here, then rebuild." >&2
	exit 1
fi

if [ "$GO" = "0" ]; then
	FLAGS="--dry-run"
	echo "*** DRY RUN -- nothing will be uploaded. Pass --go to publish. ***"
else
	FLAGS=""
fi
echo "  source: $SRC"
echo "  dest:   $DEST"
echo "  url:    $BASEURL"
echo

echo "=== 1/4  packages (add only) ==="
rclone copy $FLAGS "$SRC/packages" "$DEST/packages" --progress
echo "=== 2/4  repo.info ==="
rclone copyto $FLAGS "$SRC/repo.info" "$DEST/repo.info"
echo "=== 3/4  repo (the index) ==="
rclone copyto $FLAGS "$SRC/repo" "$DEST/repo"
echo "=== 4/4  repo.sha256 (LAST -- this is the trigger) ==="
rclone copyto $FLAGS "$SRC/repo.sha256" "$DEST/repo.sha256"

if [ "$PRUNE" = "1" ]; then
	# Opt-in for a reason: a client that fetched the old index seconds ago is
	# still entitled to the packages it names.
	echo "=== 5  pruning packages the new index does not reference ==="
	rclone sync $FLAGS "$SRC/packages" "$DEST/packages" --progress
fi

if [ "$GO" = "0" ]; then
	echo
	echo "Dry run complete. Re-run with --go to publish."
	exit 0
fi

echo
echo "=== verifying what the world now sees ==="
FAIL=0
for f in repo.info repo repo.sha256; do
	code=$(curl -sL -o /dev/null -w '%{http_code}' "$BASEURL/$f")
	printf '  %-12s %s\n' "$f" "$code"
	[ "$code" = "200" ] || FAIL=1
done

REMOTE_SUM=$(curl -sL "$BASEURL/repo.sha256" | tr -d '\r\n')
if [ "$REMOTE_SUM" = "$WANT" ]; then
	echo "  repo.sha256 served matches what we built"
else
	echo "  !! served repo.sha256 ($REMOTE_SUM) != built ($WANT)"
	FAIL=1
fi

# Negative control: a path that must NOT exist. Without this, a host that
# returns 200 for everything (a catch-all error page) reads as a clean pass.
code=$(curl -sL -o /dev/null -w '%{http_code}' "$BASEURL/repo.NOSUCHFILE")
printf '  %-12s %s   (control -- must NOT be 200)\n' "negative" "$code"
[ "$code" = "200" ] && FAIL=1

echo
if [ "$FAIL" = "0" ]; then
	echo "Published. Users add it once with:"
	echo "    pkgman add-repo $BASEURL"
	echo "    pkgman install beton"
	echo
	echo "Everything the index now carries:"
	awk '{ printf "    %s\n", $2 }' "$SRC/SHA256SUMS"
else
	echo "PUBLISH VERIFICATION FAILED -- see above." >&2
	exit 1
fi
