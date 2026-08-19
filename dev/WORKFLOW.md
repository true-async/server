# Workflow

How work is organised in this repository. Code conventions live in
[docs/CODING_STANDARDS.md](../docs/CODING_STANDARDS.md) and are not repeated here.

## Issue first

A change starts as a GitHub issue in `true-async/server` that states the defect or
the feature, what the code does today with file and line, and the measurement or
reproduction behind the claim. The commit and the CHANGELOG entry cite the number.
Design questions that have more than one defensible answer are settled in the issue
before the code is written.

## Branch and commit

Work happens on a branch off `main`; `main` itself takes merges, not direct commits.
Commit subjects follow Conventional Commits with a scope taken from the source tree
(`fix(compression):`, `feat(room):`, `refactor(h3):`), one line, imperative mood,
issue number at the end where one exists.

## CHANGELOG

Every user-visible change gets an entry under `## [Unreleased]` in the Keep a Changelog
sections (`Added`, `Changed`, `Fixed`). An entry opens with a bold sentence naming what
was wrong or what is new, then says what the code did before, and carries the number
that proves it: bytes, milliseconds, allocations, runs out of runs. A comparative claim
without a number does not go in.

## Tests

- phpt for anything observable from PHP: `tests/phpt/server/<area>/`.
- cmocka unit tests for decoders and pure C surfaces: `tests/unit/<area>/`, wired into
  `tests/unit/CMakeLists.txt`. Mandatory for decoders (CODING_STANDARDS 13c.4).
- A bug fix lands with the test that fails without it. The failing run is part of the
  evidence, not a formality.

Commands, from the repository root:

```
make -j4                                   # builds modules/true_async_server.so
TEST_PHP_EXECUTABLE=/usr/local/bin/php \
  /usr/local/bin/php run-tests.php -n \
  -d extension=$(pwd)/modules/true_async_server.so -j4 tests/phpt/server/
cd tests/build && make && ctest             # unit suite
```

`-n` matters when the installed extension is also loaded from `php.ini`: without it the
tests run against the installed build rather than the one just compiled.

## TODO.md

`TODO.md` is a performance backlog with its own step numbering, kept apart from the
plan below. An item there is a candidate, not a commitment.
