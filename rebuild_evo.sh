#!/usr/bin/env bash
# Robust rebuild for evo_boids: immune to whether the tarball is nested or flat,
# always does a clean build, and fails loudly instead of silently building the
# wrong thing. Usage:  ./rebuild_evo.sh  [path/to/evo_boids.tar.gz]
#
# NOTE (v0.16.0): this is the PRE-GIT workflow. The tree is now a git repo, so
# the normal way to get a clean build is:
#     cd ~/evo_boids && rm -rf build && cmake -S . -B build && cmake --build build -j
# This script still exists for restoring from a tarball on a fresh machine, but
# it WIPES the destination (see the guard below).
set -euo pipefail

TARBALL="${1:-/mnt/c/Users/harit/Downloads/evo_boids.tar.gz}"
DEST="$HOME/evo_boids"
STAGE="$HOME/.evo_extract"

# SAFETY GUARD: the extract step below does `rm -rf "$DEST"`. Since the tree
# became a git repository that would delete the history, the remote tracking
# config, and any uncommitted work — silently, because the re-extracted tarball
# looks superficially like a working tree. Refuse by default.
if [[ -d "$DEST/.git" && "${FORCE_WIPE_GIT:-0}" != "1" ]]; then
    echo "!! $DEST is a git repository and this script would 'rm -rf' it." >&2
    echo "   For a clean build:   cd $DEST && rm -rf build && cmake -S . -B build && cmake --build build -j" >&2
    echo "   To restore a tarball anyway (DESTROYS the repo): FORCE_WIPE_GIT=1 $0 $*" >&2
    exit 1
fi

[[ -f "$TARBALL" ]] || { echo "!! tarball not found: $TARBALL" >&2; exit 1; }
echo ">> extracting $TARBALL"

rm -rf "$DEST" "$STAGE"
mkdir -p "$STAGE"
tar xzf "$TARBALL" -C "$STAGE"

# Find the real project root by locating CMakeLists.txt, wherever it sits.
# This is what makes nesting irrelevant: nested or flat, we land on the dir
# that actually contains the project and move it to a known clean path.
CML="$(find "$STAGE" -name CMakeLists.txt -print -quit)"
[[ -n "$CML" ]] || { echo "!! no CMakeLists.txt in tarball — wrong/corrupt archive" >&2; exit 1; }
mv "$(dirname "$CML")" "$DEST"
rm -rf "$STAGE"

echo ">> clean build in $DEST/build"
cd "$DEST"
rm -rf build            # never reuse a stale CMake cache or old binary
mkdir build
cd build
cmake .. >/dev/null
cmake --build . -j

echo ">> OK — built $(./evo_boids --version)"
echo ">> run it:  cd $DEST/build && ./evo_boids --headless 50000 --name evolved_50k --log-every 2000"
