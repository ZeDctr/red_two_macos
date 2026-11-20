#!/usr/bin/env bash
# Configure and build on macOS (Apple Clang + Ninja), like tools/build_linux.sh on Linux.
#   tools/build_macos.sh [--disc <game.rvz|.iso|main.dol>] [--build-dir <dir>] [--configure] [--target red_two]
# --disc: your own disc image (or main.dol) to recompile; CMake remembers it (R2_DOL).
# Needs: Xcode Command Line Tools (clang), cmake (3.25+), ninja, git.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/macos"
disc=""
configure=0
target=""
config="RelWithDebInfo"
cmake_args=()

# Ensure Homebrew binaries (cmake, ninja) are on PATH
for p in /opt/homebrew/bin /usr/local/bin; do
    if [ -d "$p" ]; then
        case ":$PATH:" in
            *":$p:"*) ;;
            *) export PATH="$p:$PATH" ;;
        esac
    fi
done

abspath() {
    python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$1"
}

while [ $# -gt 0 ]; do
    case "$1" in
    --disc) disc="$(abspath "$2")"; configure=1; shift 2 ;;
    --build-dir) build="$(abspath "$2")"; shift 2 ;;
    --configure) configure=1; shift ;;
    --target) target="$2"; shift 2 ;;
    --config) config="$2"; shift 2 ;;
    -D*) cmake_args+=("$1"); shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done

# submodules (a fresh clone)
if [ ! -f "$root/extern/aurora/CMakeLists.txt" ] || [ ! -f "$root/extern/rcheevos/include/rc_client.h" ]; then
    git -C "$root" submodule update --init --recursive
fi

# Our fixes to Aurora (patches/aurora/*.patch) on top of the upstream submodule, as build_linux.sh:
# all of them on a clean checkout; on a modified tree each one not applied yet.
aurora="$root/extern/aurora"
clean=0
[ -z "$(git -C "$aurora" status --porcelain --untracked-files=no)" ] && clean=1
for patch in $(ls "$root"/patches/aurora/*.patch 2>/dev/null | sort); do
    if [ $clean -eq 0 ] && git -C "$aurora" apply --reverse --check "$patch" 2>/dev/null; then continue; fi
    if git -C "$aurora" apply "$patch" 2>/dev/null; then echo "applied $(basename "$patch")"; continue; fi
    if [ $clean -eq 1 ]; then echo "could not apply $(basename "$patch") to extern/aurora" >&2; exit 1; fi
    echo "note: $(basename "$patch") neither applies nor reverses on the modified extern/aurora (assumed applied)"
done

if [ $configure -eq 1 ] || [ ! -f "$build/build.ninja" ]; then
    args=(-S "$root" -B "$build" -G Ninja "-DCMAKE_BUILD_TYPE=$config")
    [ -n "$disc" ] && args+=("-DR2_DOL=$disc")
    cmake "${args[@]}" ${cmake_args[@]+"${cmake_args[@]}"}
fi
if [ -n "$target" ]; then cmake --build "$build" --target "$target"; else cmake --build "$build"; fi
