#!/bin/bash
#
# Builds "Jurassic Park The Game.app" from YOUR copy of Jurassic Park: The Game (Mac, Steam).
#
# Nothing from the game is stored in this repository: the original executable is
# translated to C on this machine, compiled for Apple Silicon, and the resulting app
# reads the game data from your install at run time.
#
# Usage: scripts/build.sh [path/to/JurassicPark100.app]
#
# Requirements: Apple's Command Line Tools (xcode-select --install) and an internet
# connection the first time (two Python packages are downloaded into .venv).
#
# Progress lines start with "==> [n/6]" so the installer can follow along.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${1:-$HOME/Library/Application Support/Steam/steamapps/common/JurassicPark/JurassicPark100.app}"
BIN="${GAME}/Contents/MacOS/JurassicPark100"

# The Mac Steam release this project is written against (addresses in tools/hle.txt).
EXPECTED_SHA256="2ed2f2bb3bb1738a0f3858c53e2e9c5d7cc5a5ae5fe4fc95fd580009fa5ccfdf"

die() { echo "error: $*" >&2; exit 1; }
step() { echo "==> [$1/6] $2"; }

step 1 "Checking requirements"
for t in clang make nm python3 codesign; do
    command -v "$t" >/dev/null 2>&1 || die "'$t' was not found. Install Apple's Command Line Tools: xcode-select --install"
done
[[ "$(uname -m)" == "arm64" ]] || die "this build targets Apple Silicon Macs"

step 2 "Verifying your copy of the game"
[[ -f "$BIN" ]] || die "JurassicPark100 was not found in: $GAME"
SHA="$(shasum -a 256 "$BIN" | awk '{print $1}')"
[[ "$SHA" == "$EXPECTED_SHA256" ]] ||
    die "this copy of the game is not the supported Mac Steam release (SHA-256 $SHA)"

step 3 "Preparing the translator"
if [[ ! -x "$ROOT/.venv/bin/python" ]]; then
    python3 -m venv "$ROOT/.venv"
fi
"$ROOT/.venv/bin/python" -m pip install --quiet --disable-pip-version-check "capstone==5.0.9" "lief==1.0.0"

cd "$ROOT"
ln -sf "$BIN" orig.bin

step 4 "Translating the game executable to C"
make -s full-gen

step 5 "Compiling (this takes a few minutes)"
make -s -j"$(sysctl -n hw.ncpu)" full app

step 6 "Packaging the app"
make -s bundle GAME="$GAME"

echo "Built: $ROOT/build/Jurassic Park The Game.app"
