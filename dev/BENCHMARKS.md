# Benchmarks

One entry per measurement, newest first. An entry names the machine, the build and
the scenario, because a number without them cannot be compared with the next one.

## 2026-08-19 — cost of the per-chunk flush on a streamed response (#170)

Base commit 22a8d37 plus the #170 working tree. Machine: WSL2, Linux 6.6.114.1,
PHP 8.6.0-dev ZTS DEBUG. Both runs on the same machine and the same PHP; the two
extension builds differ only in the flush. Byte counts do not depend on the build
mode, so DEBUG is not a caveat here.

Scenario: one streamed response of 4080 bytes (80 repeats of a 51-byte log line),
delivered as 1, 8 or 80 `send()` calls. The number is the encoded body on the wire,
de-chunked. Identity is 4080 bytes in every case.

| codec | chunks | before | after | per flush |
|---|---|---|---|---|
| gzip | 1 | 97 | 104 | 7.0 |
| gzip | 8 | 97 | 158 | 7.6 |
| gzip | 80 | 97 | 709 | 7.7 |
| br | 1 | 57 | 58 | 1.0 |
| br | 8 | 57 | 133 | 9.5 |
| br | 80 | 57 | 845 | 9.9 |
| zstd | 1 | 69 | 72 | 3.0 |
| zstd | 8 | 69 | 149 | 10.0 |
| zstd | 80 | 69 | 853 | 9.8 |

Taken. The worst case measured — 80 chunks of 51 bytes, the row-by-row export the
issue is about — costs 8 to 10 bytes per chunk and still sends 4.8 times less than
identity. A handler that wants the old ratio buffers its response instead of
streaming it, which is the same choice it always had.

Script: `tests/perf` has no case for this; the measurement was driven by an ad-hoc
client that reads the chunked body and reports its length, run once per extension
build. Reproduce by streaming a compressible body in N `send()` calls and comparing
the de-chunked body length across builds.
