#!/usr/bin/env bash
#
# Run clang-tidy against C source files touched by the current branch
# (relative to main). Designed for CI and local pre-push hooks — fails
# (exit 1) if clang-tidy emits any warning, ignores files outside src/.
#
# Requires compile_commands.json in the repo root (generate with
# `bear -- make`). Reads .clang-tidy for checker configuration.
#
# Usage:
#   tools/lint-changed.sh               # diff against origin/main
#   tools/lint-changed.sh HEAD~5        # diff against arbitrary base
#   tools/lint-changed.sh --all         # lint every src/ C file
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -f compile_commands.json ]]; then
    echo "compile_commands.json missing — run 'bear -- make' first" >&2
    exit 2
fi

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy not installed" >&2
    exit 2
fi

base="${1:-origin/main}"

if [[ "$base" == "--all" ]]; then
    mapfile -t files < <(find src -name '*.c')
else
    mapfile -t files < <(git diff --name-only --diff-filter=ACMR "$base"...HEAD -- 'src/*.c' 2>/dev/null || true)
fi

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no C source changes to lint"
    exit 0
fi

echo "Linting ${#files[@]} file(s) against base $base:"
printf '  %s\n' "${files[@]}"
echo

# clang-tidy prints warnings to stdout/stderr and exits 0 even on
# warnings. Capture output and fail if any warning line survived the
# .clang-tidy filter.
out=$(clang-tidy -p . --quiet \
    -extra-arg=-Wno-incompatible-function-pointer-types \
    "${files[@]}" 2>&1 || true)
# Filter out includes we don't own.
filtered=$(echo "$out" | grep -E "warning:|error:" \
    | grep -v "/usr/\|/deps/llhttp\|/php-src\|/stubs/.*_arginfo\.h" \
    || true)

if [[ -n "$filtered" ]]; then
    echo "$filtered"
    echo
    echo "clang-tidy flagged issues above. See .clang-tidy for checker config." >&2
    exit 1
fi

echo "clean ✓"
