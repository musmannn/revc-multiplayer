#!/bin/sh
# Double-click in Finder (or run in a terminal) to open the reVC installer window.
# Options are passed on, e.g. ./install-gui.command --update
cd "$(dirname "$0")" || exit 1
exec python3 revc_installer.py "$@"
