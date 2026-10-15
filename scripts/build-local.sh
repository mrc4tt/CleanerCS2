#!/usr/bin/env bash
# Build CleanerCS2 locally inside the same container CI uses, against the same SDK/Metamod branches.
# Usage: scripts/build-local.sh [steamrt3|steamrt4]   (default: steamrt3)
# Dependencies are cloned into .deps/ on first run; pass UPDATE=1 to pull them again.
set -euo pipefail

RUNTIME="${1:-steamrt3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps"
IMAGE="ghcr.io/source2ze/build-containers:$RUNTIME"

clone_or_update() {
	local repo="$1" ref="$2" dir="$3"
	if [ ! -d "$dir" ]; then
		git clone --depth 1 --recurse-submodules --shallow-submodules -b "$ref" "$repo" "$dir"
	elif [ "${UPDATE:-0}" = "1" ]; then
		git -C "$dir" fetch --depth 1 origin "$ref"
		git -C "$dir" reset --hard FETCH_HEAD
		git -C "$dir" submodule update --init --recursive --depth 1
	fi
}

mkdir -p "$DEPS"
clone_or_update https://github.com/alliedmodders/s2sdk.git cs2 "$DEPS/hl2sdk-cs2"
clone_or_update https://github.com/alliedmodders/metamod-source.git master "$DEPS/mmsource-2.0"

docker run --rm \
	-v "$ROOT:/src" \
	-v "cleanercs2-xmake-$RUNTIME:/root/.xmake" \
	-e HL2SDKCS2=/src/.deps/hl2sdk-cs2 \
	-e MMSOURCE_DEV=/src/.deps/mmsource-2.0 \
	-e XMAKE_ROOT=y \
	-w /src \
	"$IMAGE" \
	bash -c "rm -rf build/.objs .xmake && xmake config --mode=release --yes && xmake --yes && xmake pack --yes CleanerCS2; status=\$?; chown -R $(id -u):$(id -g) build .xmake; exit \$status"

echo "Build OK ($RUNTIME): $ROOT/build/xpack/CleanerCS2"
