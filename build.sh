#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
CMAKE_FALLBACK_DIR="${CMAKE_FALLBACK_DIR:-$ROOT_DIR/build-cmake}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "Error: cmake is not installed or not in PATH." >&2
  exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
  echo "Error: ninja is not installed or not in PATH." >&2
  exit 1
fi

# Initialize/update submodules only if dependency sources are missing.
if [[ ! -f "$ROOT_DIR/Loiacono/CMakeLists.txt" || ! -f "$ROOT_DIR/Loiacono/rtaudio/CMakeLists.txt" ]]; then
  echo "Dependency sources missing; initializing submodules..."
  git -C "$ROOT_DIR" submodule update --init --recursive
fi

# Keep Ninja as the default in build/. If build/ currently contains a
# non-Ninja CMake tree, move it aside to build-cmake/.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  current_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | head -n1)"
  if [[ "$current_generator" != "Ninja" ]]; then
    target="$CMAKE_FALLBACK_DIR"
    if [[ -e "$target" ]]; then
      target="${CMAKE_FALLBACK_DIR}-$(date +%Y%m%d-%H%M%S)"
    fi
    echo "Found non-Ninja build tree in '$BUILD_DIR' ($current_generator). Moving it to '$target'."
    mv "$BUILD_DIR" "$target"
  fi
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo "Build complete: $BUILD_DIR/jtune_autotune"
