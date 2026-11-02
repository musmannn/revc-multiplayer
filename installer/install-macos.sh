#!/usr/bin/env bash
# reVC multiplayer installer for macOS.
#
# Builds reVC from this checkout, then sets up a playable game folder:
#   1. copies your GTA Vice City files (found in ~/Downloads by default),
#   2. adds reVC's own game files (gamefiles/) on top,
#   3. puts reVC.app, revc-relay and revc-fakeclient in that folder.
# Your original GTA folder is never changed.
#
# Usage: installer/install-macos.sh [--source DIR] [--dest DIR] [--skip-build]

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO/build/macos"
SOURCE=""
DEST="$HOME/Games/reVC-Multiplayer"
SKIP_BUILD=0

usage() {
	cat <<EOF
Usage: $0 [options]

  --source DIR   Your GTA Vice City folder (the one with data/ and models/).
                 Default: searched for in ~/Downloads.
  --dest DIR     Where to install. Default: $DEST
  --skip-build   Reuse the last build in build/macos instead of rebuilding.
  -h, --help     Show this help.
EOF
}

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
fail() { printf '\n\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--source) [ $# -ge 2 ] || fail "--source needs a folder"; SOURCE="$2"; shift 2 ;;
		--dest) [ $# -ge 2 ] || fail "--dest needs a folder"; DEST="$2"; shift 2 ;;
		--skip-build) SKIP_BUILD=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) usage; fail "unknown option: $1" ;;
	esac
done

[ "$(uname -s)" = "Darwin" ] || fail "this installer is for macOS. On Windows use installer\\install-windows.bat."

# A folder is a GTA Vice City install if it has these two files.
is_game_dir() {
	[ -f "$1/data/gta_vc.dat" ] && [ -f "$1/models/gta3.img" ]
}

# --- Find the game files --------------------------------------------------

step "Looking for your GTA Vice City files"
if [ -z "$SOURCE" ]; then
	candidates=()
	while IFS= read -r dat; do
		dir="$(cd "$(dirname "$dat")/.." && pwd)"
		is_game_dir "$dir" && candidates+=("$dir")
	done < <(find "$HOME/Downloads" -maxdepth 5 -type f -iname gta_vc.dat -ipath '*/data/*' 2>/dev/null)

	[ ${#candidates[@]} -gt 0 ] || fail "no GTA Vice City folder found in ~/Downloads.
Put your game folder (the one containing data/ and models/) in Downloads,
or point at it with: $0 --source \"/path/to/GTA Vice City\""

	SOURCE="${candidates[0]}"
	if [ ${#candidates[@]} -gt 1 ]; then
		info "Found more than one game folder:"
		for c in "${candidates[@]}"; do info "  $c"; done
		info "Using the first. Pass --source to pick another."
	fi
fi
SOURCE="$(cd "$SOURCE" 2>/dev/null && pwd)" || fail "folder not found: $SOURCE"
is_game_dir "$SOURCE" || fail "$SOURCE doesn't look like GTA Vice City (no data/gta_vc.dat or models/gta3.img)."
info "Game files: $SOURCE"
info "Install to: $DEST"

# --- Build ----------------------------------------------------------------

if [ "$SKIP_BUILD" -eq 0 ]; then
	step "Checking build tools"
	if ! xcode-select -p >/dev/null 2>&1; then
		xcode-select --install || true
		fail "the Xcode Command Line Tools are needed. Finish the install window that just opened, then run this again."
	fi

	if ! command -v brew >/dev/null 2>&1; then
		for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do
			[ -x "$b" ] && eval "$("$b" shellenv)" && break
		done
	fi
	command -v brew >/dev/null 2>&1 || fail "Homebrew is needed. Install it from https://brew.sh, then run this again."

	for formula in cmake git glfw openal-soft mpg123; do
		if brew list --versions "$formula" >/dev/null 2>&1; then
			info "$formula: installed"
		else
			info "$formula: installing"
			brew install "$formula"
		fi
	done

	step "Fetching submodules"
	if [ -d "$REPO/.git" ]; then
		git -C "$REPO" submodule update --init --recursive
	fi
	[ -f "$REPO/vendor/librw/CMakeLists.txt" ] || fail "vendor/librw is empty. Clone the repo with: git clone --recursive https://github.com/musmannn/revc-multiplayer.git"

	step "Building reVC (this takes a few minutes)"
	cmake -S "$REPO" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
		-DREVC_VENDORED_LIBRW=ON -DLIBRW_PLATFORM=GL3 -DLIBRW_GL3_GFXLIB=GLFW -DREVC_AUDIO=OAL
	cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)"
fi

APP="$BUILD_DIR/src/reVC.app"
[ -d "$APP" ] || fail "no build found at $APP. Run without --skip-build."

# --- Install --------------------------------------------------------------

step "Copying game files"
mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"
if [ "$SOURCE" = "$DEST" ]; then
	info "Source and destination are the same folder; installing in place."
elif is_game_dir "$DEST"; then
	info "Already there, skipping. (Delete $DEST for a fresh copy.)"
else
	info "From $SOURCE (a few GB, give it a minute)"
	rsync -a "$SOURCE/" "$DEST/"
fi

step "Adding reVC files"
rsync -a "$REPO/gamefiles/" "$DEST/"
rm -rf "$DEST/reVC.app"
cp -R "$APP" "$DEST/"
cp "$BUILD_DIR/mp/revc-relay" "$BUILD_DIR/mp/revc-fakeclient" "$DEST/"
cp "$REPO/INSTALLATION.md" "$DEST/INSTALLATION.md"
xattr -dr com.apple.quarantine "$DEST/reVC.app" 2>/dev/null || true

step "Done"
info "Play:          open \"$DEST/reVC.app\""
info "Host a relay:  \"$DEST/revc-relay\" --port 7777"
info "Setup guide:   $DEST/INSTALLATION.md"
