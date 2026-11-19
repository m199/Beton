#!/bin/sh
# Copyright 2026, David Beckman, indispensablelabs@gmail.com.
# All rights reserved. Distributed under the terms of the MIT License.

#
# Assemble the Indispensable Labs apps repository from the .hpkg files in
# packaging/out/ PLUS whatever is already published, into packaging/out-repo/.
#
# MUST RUN ON HAIKU -- it needs `package_repo`.
#
# ---------------------------------------------------------------------------
# WHY THIS SCRIPT DOWNLOADS THE LIVE REPOSITORY FIRST
# ---------------------------------------------------------------------------
#
# `package_repo create` does not amend an index, it writes a new one. The index
# it writes lists exactly the packages handed to it -- so a repository holding
# two apps, rebuilt from one app's out/ directory, comes back holding one app.
# The other app's .hpkg stays in the bucket (publish-repo.sh copies, it does
# not sync) but nothing references it, so `pkgman install` can no longer see
# it. Nothing errors. The publish verification passes. The app just vanishes.
#
# This repository crossed that threshold on 2026-08-29, when MidiMonitor became
# the second app alongside Beton. Both apps' copies of this script therefore
# pull the published index down, enumerate it with `package_repo list -f`, and
# re-index every package they find alongside the locally built ones.
#
# The consequence worth understanding: publishing ANY app republishes ALL of
# them, byte-for-byte, from the copies in the bucket. That is why the pulled
# packages are checksum-verified against the index that named them -- we are
# putting our name on those bytes again.
#
# ---------------------------------------------------------------------------
# NO SIGNING EXISTS
# ---------------------------------------------------------------------------
#
# Haiku has no package signing. The only integrity is a checksum chain that
# never leaves the origin: repo.sha256 -> repo -> per-package checksum -> .hpkg.
# That detects a corrupted download; it cannot detect a substituted one,
# because whoever can replace a .hpkg can replace the index that names its
# checksum. So whoever can write to the bucket controls what these users run.
#
# This repository carries ordinary userland apps rather than kernel drivers, so
# the blast radius is smaller than Media-OS's -- but it is not zero, and it is
# the reason SHA256SUMS is emitted below for publication somewhere structurally
# independent of the bucket (the git repo / a GitHub release). That is the only
# out-of-band check a suspicious user can make.

set -e

PKGDIR=$(cd "$(dirname "$0")" && pwd)
INDIR="$PKGDIR/out"
OUTDIR="$PKGDIR/out-repo"

# --- configuration ---------------------------------------------------------
#
# The base URL is load-bearing and hard to change later: `pkgman add-repo`
# stores the base URL the user typed into their local repo config and keeps it.
# Moving hosts later means every existing user must drop-repo and add-repo.
#
# NO HAIKU RELEASE IN THIS PATH -- compatibility is per package, via
# `requires { haiku >= ... }`. Must stay in step with URL_PATH in
# publish-repo.sh; that script fails the publish if the two ever drift.
BASEURL_ROOT="https://hpkg.indispensablelabs.com/apps"

# --- options ---------------------------------------------------------------
PULL=1
for arg in "$@"; do
	case "$arg" in
		# Build an index from out/ ALONE. This is how you deliberately drop a
		# retired app from the repository -- and the only way to do it, since
		# the default is to preserve everything published. It is not an
		# offline convenience: using it by accident is exactly the failure
		# this script exists to prevent, so it prints a warning and the
		# resulting index is worth reading before publishing.
		--no-pull) PULL=0 ;;
		*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

if [ "$(uname -s)" != "Haiku" ]; then
	echo "ERROR: this must run on Haiku -- it needs package_repo." >&2
	exit 1
fi
command -v package_repo >/dev/null 2>&1 || {
	echo "ERROR: package_repo not found." >&2; exit 1; }

case "$(uname -m)" in
	x86_64)	ARCH=x86_64 ;;
	BePC|x86)
		if [ -d /boot/system/develop/headers/x86_gcc2 ] \
			|| gcc -dumpversion 2>/dev/null | grep -q '^2\.'; then
			ARCH=x86_gcc2
		else
			ARCH=x86
		fi
		;;
	*)	ARCH=$(uname -m) ;;
esac
BASEURL="$BASEURL_ROOT/$ARCH/current"

if [ ! -d "$INDIR" ] || [ -z "$(ls "$INDIR"/*.hpkg 2>/dev/null)" ]; then
	echo "ERROR: no .hpkg files in $INDIR -- run build-package.sh first." >&2
	exit 1
fi

echo "Indispensable Labs apps repository build"
echo "  arch:    $ARCH"
echo "  baseurl: $BASEURL"
echo "  output:  $OUTDIR"
echo

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/packages"

# --- validate one .hpkg and stage it ---------------------------------------
#
# Every package must match the repository architecture or be `any`;
# package_repo enforces this too (RepositoryWriterImpl.cpp:415-423) but with a
# worse message. Reading the arch back also proves the file parses at all,
# which is what catches a truncated download from the pull step below.
stage_package() {
	p="$1"
	base=$(basename "$p")

	declared=$(package list -i "$p" 2>/dev/null \
		| sed -n 's/^[[:space:]]*architecture:[[:space:]]*//p' | head -1)
	case "$declared" in
		"$ARCH"|any) ;;
		"")	echo "ERROR: $base -- could not read its architecture" >&2; exit 1 ;;
		*)	echo "ERROR: $base declares arch '$declared', not $ARCH or any" >&2
			exit 1 ;;
	esac
	case "$base" in
		*-"$declared".hpkg) ;;
		*)	echo "ERROR: $base -- filename arch disagrees with declared '$declared'" >&2
			exit 1 ;;
	esac

	# The repository's vendor and every package's vendor must agree or
	# package_repo refuses the whole build. Checking here names the package.
	pvendor=$(package list -i "$p" 2>/dev/null \
		| sed -n 's/^[[:space:]]*vendor:[[:space:]]*//p' | head -1)
	if [ "$pvendor" != "Indispensable Labs" ]; then
		echo "ERROR: $base declares vendor '$pvendor', not 'Indispensable Labs'" >&2
		exit 1
	fi

	cp "$p" "$OUTDIR/packages/$base"
	printf '  %-52s %s\n' "$base" "$declared"
}

# --- select locally built packages -----------------------------------------
#
# out/ accumulates across versions and across experiments. Selecting by name
# alone is ambiguous the moment a second version of the same package exists,
# and `ls | head -1` picks ALPHABETICALLY -- which on the Media-OS repo once
# silently assembled an entire release from a stale build while every check
# passed. So: group by package name, and refuse outright if any name has more
# than one file present.
echo "=== local packages (packaging/out) ==="
LOCAL_NAMES=""
NAMES=$(ls "$INDIR"/*.hpkg | xargs -n1 basename | sed 's/-[0-9].*//' | sort -u)
for name in $NAMES; do
	matches=$(ls "$INDIR/$name-"*.hpkg 2>/dev/null || true)
	nmatch=$(printf '%s\n' "$matches" | grep -c . || true)
	if [ "$nmatch" -gt 1 ]; then
		echo "ERROR: '$name' is ambiguous -- $nmatch files match:" >&2
		printf '  %s\n' $matches >&2
		echo "Remove the ones you are not publishing and re-run." >&2
		exit 1
	fi

	stage_package "$matches"
	LOCAL_NAMES="$LOCAL_NAMES $name"
done

# --- pull in what is already published --------------------------------------
PULLED=0
if [ "$PULL" = "0" ]; then
	echo
	echo "*** --no-pull: indexing packaging/out ALONE. Any app already in the"
	echo "*** repository and not rebuilt here will DISAPPEAR from the index"
	echo "*** when this is published. Read the listing below before you do."
else
	echo
	echo "=== already published (re-indexed from $BASEURL) ==="
	TMP="$OUTDIR/.pull"
	mkdir -p "$TMP"

	# A missing index is the legitimate first-publish case for a new
	# architecture, not an error -- but distinguish it from a network failure,
	# because "the repo looks empty" is exactly how the clobber this script
	# prevents would look if we guessed wrong. curl -f makes 404 a failure
	# exit; we then probe once more to tell 404 apart from everything else.
	if curl -fsL --max-time 60 -o "$TMP/repo" "$BASEURL/repo"; then
		# filename -> checksum, from the index that already vouches for these
		# bytes. Attribute lines are tab-indented; description continuation
		# lines are not, which is what keeps this from matching prose.
		package_repo list -v "$TMP/repo" | awk '
			/^\tname: /         { n = $2 }
			/^\tversion: /      { v = $2 }
			/^\tarchitecture: / { a = $2 }
			/^\tchecksum: /     { print n "-" v "-" a ".hpkg", $2 }
		' > "$TMP/published"

		while read -r base sum; do
			[ -n "$base" ] || continue

			# A locally built package SUPERSEDES the published one of the same
			# name -- otherwise every release would accumulate its own
			# predecessors in the index forever.
			pname=$(echo "$base" | sed 's/-[0-9].*//')
			skip=0
			for l in $LOCAL_NAMES; do
				[ "$l" = "$pname" ] && skip=1
			done
			if [ "$skip" = "1" ]; then
				printf '  %-52s (superseded by local build)\n' "$base"
				continue
			fi

			curl -fsL --max-time 300 -o "$TMP/$base" "$BASEURL/packages/$base" || {
				echo "ERROR: $base is in the published index but could not be" >&2
				echo "downloaded from $BASEURL/packages/$base" >&2
				echo "Publishing now would drop it from the repository." >&2
				exit 1; }

			# Verify against the checksum the OLD index declared. This is the
			# one link in the chain we can actually check: it proves the bytes
			# we are about to re-index are the bytes that index vouched for.
			got=$(sha256sum "$TMP/$base" | awk '{print $1}')
			if [ "$got" != "$sum" ]; then
				echo "ERROR: $base failed its checksum from the published index." >&2
				echo "  index says: $sum" >&2
				echo "  downloaded: $got" >&2
				exit 1
			fi

			stage_package "$TMP/$base"
			PULLED=$((PULLED + 1))
		done < "$TMP/published"

		[ "$PULLED" = "0" ] && [ ! -s "$TMP/published" ] \
			&& echo "  (published index is empty)"
	else
		code=$(curl -sL -o /dev/null -w '%{http_code}' --max-time 60 "$BASEURL/repo")
		if [ "$code" = "404" ]; then
			echo "  no index published yet at this URL -- first publish for $ARCH"
		else
			echo "ERROR: could not fetch $BASEURL/repo (HTTP $code)." >&2
			echo "Refusing to build an index that might silently omit already-" >&2
			echo "published packages. Fix the network, or pass --no-pull if you" >&2
			echo "really do intend to publish packaging/out alone." >&2
			exit 1
		fi
	fi
	rm -rf "$TMP"
fi

COUNT=$(ls "$OUTDIR/packages"/*.hpkg | grep -c .)
echo
echo "  $COUNT package(s) total ($PULLED carried over from the live repository)"
echo

# --- index -----------------------------------------------------------------
sed -e "s|@BASEURL@|$BASEURL|g" \
    -e "s|@ARCH@|$ARCH|g" \
    "$PKGDIR/repo-info.in" > "$OUTDIR/repo.info"

( cd "$OUTDIR" && package_repo create -q repo.info packages/*.hpkg )
[ -f "$OUTDIR/repo" ] || { echo "ERROR: package_repo produced no 'repo'" >&2; exit 1; }

( cd "$OUTDIR" && sha256sum repo | awk '{print $1}' > repo.sha256 )
( cd "$OUTDIR/packages" && sha256sum *.hpkg > ../SHA256SUMS )

echo "=== verifying the index reads back ==="
package_repo list -f "$OUTDIR/repo"
echo
echo "=== result ==="
find "$OUTDIR" -type f | sort | while read -r f; do
	printf '  %8s  %s\n' "$(stat -c %s "$f")" "${f#$OUTDIR/}"
done
echo
echo "Next: packaging/publish-repo.sh   (do NOT just rsync the directory --"
echo "      the upload ORDER matters; see that script's header)"
