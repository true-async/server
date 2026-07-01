#!/usr/bin/env bash
# Refresh the bundled wslay under deps/wslay from an upstream release.
#
# Usage: tools/update-wslay.sh <version>          (e.g. 1.1.1)
#        tools/update-wslay.sh <version> <url>    (override tarball URL)
#
# The script downloads the upstream release tarball, extracts the ten
# library sources (lib/wslay_*.c, lib/wslay_*.h) plus the public
# header (lib/includes/wslay/wslay.h) and the COPYING license file,
# overwriting deps/wslay/ in place.
#
# wslayver.h is hand-written (upstream generates it from
# wslayver.h.in via autoconf, which we deliberately do not pull in).
# After running this script, manually bump WSLAY_VERSION in
# deps/wslay/includes/wslay/wslayver.h and the version recorded in
# deps/wslay/UPSTREAM.md.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <version> [tarball-url]" >&2
    exit 2
fi

version="$1"
url="${2:-https://github.com/tatsuhiro-t/wslay/archive/refs/tags/release-${version}.tar.gz}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$repo_root/deps/wslay"

if [[ ! -d "$dest" ]]; then
    echo "error: $dest does not exist" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "==> downloading $url"
curl -fL --retry 3 -o "$tmp/wslay.tar.gz" "$url"

echo "==> extracting"
tar -xzf "$tmp/wslay.tar.gz" -C "$tmp"

# Locate files by name; tarball top-level dir is wslay-release-X.Y.Z.
find_one() {
    local name="$1"
    local hit
    hit="$(find "$tmp" -type f -name "$name" -not -path "*/tests/*" -not -path "*/examples/*" | head -n 1)"
    if [[ -z "$hit" ]]; then
        echo "error: $name not found in tarball" >&2
        exit 1
    fi
    echo "$hit"
}

lib_files=(wslay_event.c wslay_event.h wslay_frame.c wslay_frame.h
           wslay_net.c   wslay_net.h   wslay_queue.c wslay_queue.h
           wslay_stack.c wslay_stack.h)

echo "==> staging into $dest"
for f in "${lib_files[@]}"; do
    install -m 0644 "$(find_one "$f")" "$dest/lib/$f"
done
install -m 0644 "$(find_one wslay.h)" "$dest/includes/wslay/wslay.h"
install -m 0644 "$(find_one COPYING)" "$dest/LICENSE"

echo "==> done"
echo
echo "Next steps (manual):"
echo "  1. Bump WSLAY_VERSION in $dest/includes/wslay/wslayver.h to \"$version\""
echo "  2. Bump the Release line in $dest/UPSTREAM.md to release-$version"
echo "  3. Review the diff (git diff -- deps/wslay) and commit"
