#!/usr/bin/env bash
# Fetch audiodsp's pinned native dependencies into .deps/ for standalone
# builds (a workspace checkout with sibling clones already in place needs
# none of this -- the build glue prefers .deps/ and falls back to the
# siblings).
#
#   ./scripts/fetch_deps.sh          # clone/checkout pins, apply patches
#   ./scripts/fetch_deps.sh --check  # verify only; nonzero on any mismatch
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$HERE/.deps"
LOCK="$HERE/DEPENDENCIES.lock"
MODE="${1:-fetch}"
rc=0
while read -r name url rev; do
    [[ "$name" =~ ^#|^$ ]] && continue
    dir="$DEPS/$name"
    if [[ "$MODE" == "--check" ]]; then
        have=$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo missing)
        if [[ "$have" != "$rev" ]]; then
            echo "MISMATCH $name: have $have, want $rev" >&2; rc=1
        else
            echo "ok $name @ ${rev:0:12}"
        fi
        continue
    fi
    if [[ ! -d "$dir/.git" ]]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$rev"
    git -C "$dir" checkout -qf "$rev"
done < "$LOCK"
if [[ "$MODE" != "--check" ]]; then
    for p in "$HERE"/patches/adafruit_mp3/*.patch; do
        if git -C "$DEPS/mp3" apply --reverse --check "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            git -C "$DEPS/mp3" apply "$p"
            echo "applied: $(basename "$p")"
        fi
    done
    echo "Dependencies ready under $DEPS"
fi
exit $rc
