#!/usr/bin/env bash
# Upload files directly to a GitHub release via the REST API (curl + jq only,
# no gh CLI — the Linux builds run inside a bare ubuntu:22.04 container and
# the Windows build inside MSYS2, neither of which ships gh).
#
# Exists because actions/upload-artifact is subject to the account-wide
# artifact *storage quota*, which blocked every build in Jul 2026 even after
# all artifacts were purged (recalc stuck — see run 81). Release assets are
# not metered by that quota, so each platform job publishes its binaries
# straight to the release and no artifact hand-off is needed.
#
# Clobbers same-name assets (delete + re-upload; the API rejects duplicates).
#
# Usage: GH_TOKEN=... upload-release-assets.sh <owner/repo> <tag> <file...>
set -euo pipefail

REPO="$1"; TAG="$2"; shift 2
API="https://api.github.com/repos/$REPO"
AUTH=(-H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json")

REL_ID=$(curl -sf --retry 3 "${AUTH[@]}" "$API/releases/tags/$TAG" | jq -r .id)
ASSETS=$(curl -sf --retry 3 "${AUTH[@]}" "$API/releases/$REL_ID/assets?per_page=100")

for f in "$@"; do
  name=$(basename "$f")
  old_id=$(printf '%s' "$ASSETS" | jq -r ".[] | select(.name==\"$name\") | .id")
  if [ -n "$old_id" ]; then
    echo "clobber: deleting existing asset $name (id $old_id)"
    curl -sf --retry 3 -X DELETE "${AUTH[@]}" "$API/releases/assets/$old_id" > /dev/null
  fi
  echo "uploading $name ($(du -h "$f" | cut -f1))"
  curl -sf --retry 3 -X POST "${AUTH[@]}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @"$f" \
    "https://uploads.github.com/repos/$REPO/releases/$REL_ID/assets?name=$name" > /dev/null
done

echo "done: https://github.com/$REPO/releases/tag/$TAG"
