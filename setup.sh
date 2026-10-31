#!/usr/bin/env bash
# Set up everything needed to build webc on macOS or Linux, build the release
# preset, and run the crawler.
#
#   ./setup.sh               set up, build, run
#   ./setup.sh --build-only  set up and build, don't run
set -euo pipefail

cd "$(dirname "$0")"

run=1
if [[ "${1:-}" == "--build-only" ]]; then
  run=0
fi

# 1. build tools
missing=()
for tool in git cmake ninja clang++ pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done

if ((${#missing[@]})); then
  if [[ "$(uname)" == "Darwin" ]]; then
    if ! command -v brew >/dev/null 2>&1; then
      echo "missing: ${missing[*]}. install Homebrew (https://brew.sh) and re-run." >&2
      exit 1
    fi
    if [[ " ${missing[*]} " == *" clang++ "* ]]; then
      echo "clang++ not found, installing the Xcode command line tools..."
      xcode-select --install || true
      echo "re-run this script once the Xcode install finishes." >&2
      exit 1
    fi
    echo "installing ${missing[*]} with Homebrew..."
    brew install "${missing[@]}"
  else
    echo "missing: ${missing[*]}" >&2
    echo "on Debian/Ubuntu: sudo apt install git cmake ninja-build clang pkg-config curl zip unzip tar" >&2
    exit 1
  fi
fi

# 2. vcpkg (installs boost, sqlite3, openssl, lexbor from vcpkg.json)
if [[ -z "${VCPKG_ROOT:-}" ]]; then
  export VCPKG_ROOT="$HOME/vcpkg"
  echo "VCPKG_ROOT not set, using $VCPKG_ROOT"
  echo "  add 'export VCPKG_ROOT=\"$VCPKG_ROOT\"' to your shell profile to keep it"
fi
if [[ ! -d "$VCPKG_ROOT" ]]; then
  echo "cloning vcpkg into $VCPKG_ROOT..."
  git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi
if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

# 3. configure + build (the first configure builds the dependencies, which
#    takes a few minutes)
cmake --preset release
cmake --build --preset release

# 4. run
if ((run)); then
  ./build/release/main
fi
