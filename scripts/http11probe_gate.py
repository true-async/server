#!/usr/bin/env python3
"""Validate the complete pinned Http11Probe inventory, then its scored results.

Warn remains acceptable: the probe uses it for legal interoperability choices.
Unscored Fail is informational; Error/Skip is never a completed probe run.
"""
import json
import pathlib
import sys

POLICY = pathlib.Path(__file__).with_name("http11probe-policy.json")


def validate(data, policy):
    expected = policy["tests"]
    results = data.get("results")
    if not isinstance(results, list):
        raise ValueError("missing results array")
    seen = set()
    exceptions = []
    for result in results:
        if not isinstance(result, dict):
            raise ValueError("test result is not an object")
        test_id = result.get("id")
        if test_id not in expected or test_id in seen:
            raise ValueError(f"unexpected or duplicate test: {test_id}")
        seen.add(test_id)
        if type(result.get("scored")) is not bool or result["scored"] != expected[test_id]:
            raise ValueError(f"scored classification changed: {test_id}")
        verdict = result.get("verdict")
        if verdict not in {"Pass", "Warn", "Fail"} or result.get("error"):
            raise ValueError(f"incomplete test: {test_id}: {verdict}")
        if result["scored"] and verdict == "Fail":
            # RFC 9112 §7.1.1 recommends a bounded extension length and an
            # appropriate 4xx. Upstream accepts 400/431 but omits our 413.
            # Keep the original verdict in artifacts and the published report.
            if test_id == "MAL-CHUNK-EXT-64K" and result.get("statusCode") == 413:
                exceptions.append(test_id)
            else:
                raise ValueError(f"scored failure: {test_id}, status={result.get('statusCode')}")
    missing = expected.keys() - seen
    if missing:
        raise ValueError(f"missing tests: {', '.join(sorted(missing))}")
    return exceptions


def main():
    if len(sys.argv) != 2:
        print("usage: http11probe_gate.py results.json", file=sys.stderr)
        return 2
    try:
        policy = json.loads(POLICY.read_text())
        exceptions = validate(json.loads(pathlib.Path(sys.argv[1]).read_text()), policy)
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"Http11Probe gate failed: {error}", file=sys.stderr)
        return 1
    print(f"Http11Probe gate passed: {len(policy['tests'])} tests, upstream {policy['upstreamCommit']}")
    for test_id in exceptions:
        print(f"Accepted documented upstream mismatch: {test_id}: 413 (RFC 9112 §7.1.1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
