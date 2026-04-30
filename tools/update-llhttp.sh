#!/usr/bin/env bash
# Refresh the bundled llhttp under deps/llhttp from an upstream release.
#
# Usage: tools/update-llhttp.sh <version>          (e.g. 9.3.0)
#        tools/update-llhttp.sh <version> <url>    (override tarball URL)
#
# The script downloads the upstream release tarball, extracts the four
# files we ship (api.c, http.c, llhttp.c, llhttp.h), overwrites
# deps/llhttp/ in place, and updates UPSTREAM.md with the new version.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <version> [tarball-url]" >&2
    exit 2
fi

version="$1"
url="${2:-https://github.com/nodejs/llhttp/archive/refs/tags/release/v${version}.tar.gz}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$repo_root/deps/llhttp"

if [[ ! -d "$dest" ]]; then
    echo "error: $dest does not exist" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "==> downloading $url"
curl -fL --retry 3 -o "$tmp/llhttp.tar.gz" "$url"

echo "==> extracting"
tar -xzf "$tmp/llhttp.tar.gz" -C "$tmp"

# The tarball top-level dir varies (llhttp-release-vX.Y.Z); locate files by name.
find_one() {
    local name="$1"
    local hit
    hit="$(find "$tmp" -type f -name "$name" -not -path "*/test/*" | head -n 1)"
    if [[ -z "$hit" ]]; then
        echo "error: $name not found in tarball" >&2
        exit 1
    fi
    echo "$hit"
}

api_c="$(find_one api.c)"
http_c="$(find_one http.c)"
llhttp_c="$(find_one llhttp.c)"
llhttp_h="$(find_one llhttp.h)"

echo "==> staging into $dest"
install -m 0644 "$api_c"    "$dest/api.c"
install -m 0644 "$http_c"   "$dest/http.c"
install -m 0644 "$llhttp_c" "$dest/llhttp.c"
install -m 0644 "$llhttp_h" "$dest/llhttp.h"
install -m 0644 "$llhttp_h" "$dest/include/llhttp.h"

echo "==> updating UPSTREAM.md to v${version}"
sed -i.bak -E \
    -e "s|^\\| Release  \\| v[0-9.]+ \\|\$|\\| Release  \\| v${version} \\||" \
    -e "s|releases/tags/release/v[0-9.]+|releases/tags/release/v${version}|g" \
    -e "s|tags/release/v[0-9.]+\\.tar\\.gz|tags/release/v${version}.tar.gz|g" \
    "$dest/UPSTREAM.md"
rm -f "$dest/UPSTREAM.md.bak"

echo
echo "Done. Review the diff:"
echo "  git diff -- deps/llhttp"
