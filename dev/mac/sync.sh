#!/usr/bin/env bash
# Sends the working tree of the PC to the Mac's build folder (docs/Platforms.md): tracked and new files, with their
# current content (uncommitted edits included), and removes there the files that were deleted here. `git ls-files`
# alone would leave out new files that are not added yet, which is how a build once failed on a missing .qml.
#
#   RAGO_MAC_HOST=<mac-host> dev/mac/sync.sh   # default ssh host "mac", folder ~/build-ragoshell
set -euo pipefail
host="${RAGO_MAC_HOST:-mac}"
dest="${RAGO_MAC_DIR:-build-ragoshell}"
cd "$(git rev-parse --show-toplevel)"
git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - 2>/dev/null \
    | ssh "$host" "mkdir -p ~/$dest && tar xf - -C ~/$dest"
deleted=$(git ls-files --deleted | tr '\n' ' ')
if [ -n "$deleted" ]; then
    ssh "$host" "cd ~/$dest && rm -f $deleted"
fi
echo "synced to $host:~/$dest"
