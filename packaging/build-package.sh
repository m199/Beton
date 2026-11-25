#!/bin/sh
# Copyright 2026, David Beckman, indispensablelabs@gmail.com.
# All rights reserved. Distributed under the terms of the MIT License.

#
# Build Beton from a source tree and produce a .hpkg in packaging/out/.
#
# MUST RUN ON HAIKU -- it needs `package`, and it links against the Haiku
# libraries the resulting package will declare requirements on.
#
# Usage:  packaging/build-package.sh [<source-dir>]
#
# <source-dir> defaults to the repository root, i.e. building the working tree.
# Pass an unpacked release tarball instead to build exactly what the HaikuPorts
# recipe would build:
#     packaging/build-package.sh /boot/home/build/Beton-1.3.0

set -e

PKGDIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$PKGDIR/.." && pwd)
SRCDIR=${1:-$REPO}
OUTDIR="$PKGDIR/out"

# --- version ---------------------------------------------------------------
#
# VERSION_BASE is the upstream version; VERSION appends OUR package revision.
# Bump the revision (not the base) when the packaging changes but the source
# does not -- a rebuilt package with identical contents and the same version
# is indistinguishable to pkgman, so clients never pick it up.
VERSION_BASE="1.4.1"
# Revision resets to 1 on an upstream version bump -- it counts OUR packaging
# iterations of a given upstream version, not builds overall.
#
# 1.4.1-1 is built from main at the v1.4.1 tag. 1.4.1 is 1.4.0 plus PR #36
# (FLAC ratings kept on Properties saves, 978666d; locale-aware sorting of the
# filter lists, 11e63a7), which missed upstream's v1.4.0 tag (79185a0). Build
# from the repository working tree at that tag.
#
# History worth keeping: 1.4.0-1 was built from main at the v1.4.0 tag,
# including the About-box fix 1f6944b that made 1.3.1-2 differ from
# upstream's tarball.
#
# History worth keeping: 1.3.1-2 carried 1f6944b, which was NOT in upstream's
# v1.3.1 tag, so it deliberately did not match packaging/beton-1.3.1.recipe.
#
# History worth keeping: 1.3.0-1 was linked against ffmpeg6, purely because
# ffmpeg6_devel happened to be the devel package installed on the build box.
# Upstream has declared ffmpeg8 since the 1.2.0 recipe and has never used
# ffmpeg6. 1.3.0-2 corrected that; see the clean-build note below for why the
# correction needed a from-scratch rebuild to take effect.
REVISION="1"
VERSION="$VERSION_BASE-$REVISION"

APPNAME="Beton"

if [ "$(uname -s)" != "Haiku" ]; then
	echo "ERROR: this must run on Haiku -- it needs the 'package' tool." >&2
	exit 1
fi
command -v package >/dev/null 2>&1 || {
	echo "ERROR: 'package' not found." >&2; exit 1; }
[ -f "$SRCDIR/Makefile" ] || {
	echo "ERROR: no Makefile in $SRCDIR" >&2; exit 1; }

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

# Beton does not build with gcc2 -- the HaikuPorts recipe says
# ARCHITECTURES="all !x86_gcc2". On a gcc2 system it must be built for the
# `x86` secondary architecture instead, which is a different package name and
# a different repository. Refuse rather than emit something mislabelled.
if [ "$ARCH" = "x86_gcc2" ]; then
	echo "ERROR: Beton does not build for x86_gcc2 (C++17)." >&2
	echo "Build it for the x86 secondary architecture instead." >&2
	exit 1
fi

echo "Beton package build"
echo "  source:  $SRCDIR"
echo "  version: $VERSION"
echo "  arch:    $ARCH"
echo

# --- compile ---------------------------------------------------------------
#
# Same two steps the HaikuPorts recipe's BUILD() runs. bindcatalogs is not
# optional: without it the app ships with no translations bound in, which is a
# silent, shippable defect -- the app just runs in English.
# ⚠️ ALWAYS FROM CLEAN. Beton's Makefile links ffmpeg by bare name (avcodec,
# avformat, avutil, swresample) and resolves whatever
# /boot/system/develop/lib/libavcodec.so points at -- a symlink owned by
# whichever single ffmpeg*_devel is installed (they conflict; only one can be).
# Swapping ffmpeg6_devel for ffmpeg8_devel therefore changes what a build links
# against while touching NO source file, so make would keep every stale .o and
# emit a binary linked half against each. Nothing in the build would complain.
# That is how 1.3.0-1 shipped ffmpeg6-linked in the first place.
echo "=== clean ==="
( cd "$SRCDIR" && rm -rf objects )
echo "=== make ==="
( cd "$SRCDIR" && make OBJ_DIR=objects -j"$(nproc)" )
echo "=== bindcatalogs ==="
( cd "$SRCDIR" && make OBJ_DIR=objects bindcatalogs )

BIN="$SRCDIR/objects/$APPNAME"
[ -f "$BIN" ] || { echo "ERROR: $BIN not produced" >&2; exit 1; }

# --- assemble the package tree ---------------------------------------------
#
# Layout copied from an existing HaikuPorts GUI app (wonderbrush): the binary
# at apps/<Name>, and a Deskbar entry as a RELATIVE symlink from
# data/deskbar/menu/Applications/<Name>. It must be relative -- packagefs
# mounts the package contents under a prefix, so an absolute link would point
# outside the package.
STAGE="$PKGDIR/.stage-$ARCH"
rm -rf "$STAGE"
mkdir -p "$STAGE/apps" "$STAGE/data/deskbar/menu/Applications"
cp "$BIN" "$STAGE/apps/$APPNAME"
ln -s "../../../../apps/$APPNAME" "$STAGE/data/deskbar/menu/Applications/$APPNAME"

sed -e "s|@VERSION@|$VERSION|g" \
    -e "s|@VERSION_BASE@|$VERSION_BASE|g" \
    -e "s|@ARCH@|$ARCH|g" \
    "$PKGDIR/$(echo $APPNAME | tr 'A-Z' 'a-z').PackageInfo.in" \
    > "$STAGE/.PackageInfo"

# --- create ----------------------------------------------------------------
mkdir -p "$OUTDIR"
PKGFILE="$OUTDIR/beton-$VERSION-$ARCH.hpkg"
rm -f "$PKGFILE"
( cd "$STAGE" && package create -q "$PKGFILE" )
[ -f "$PKGFILE" ] || { echo "ERROR: package create produced nothing" >&2; exit 1; }

# The filename arch and the DECLARED arch must agree -- build-repo.sh enforces
# this too, but catching it here points at the actual cause.
declared=$(package list -i "$PKGFILE" | sed -n 's/^[[:space:]]*architecture:[[:space:]]*//p' | head -1)
[ "$declared" = "$ARCH" ] || {
	echo "ERROR: package declares arch '$declared', expected '$ARCH'" >&2; exit 1; }

rm -rf "$STAGE"

echo
echo "=== result ==="
ls -l "$PKGFILE"
echo
package list -i "$PKGFILE" | grep -iE 'name|version|architecture|vendor|provides|requires'
echo
echo "Next: packaging/build-repo.sh"
