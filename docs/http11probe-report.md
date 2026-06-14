# Http11Probe conformance report

HTTP/1.1 RFC 9110/9112 compliance + request-smuggling + malformed-input + header-normalization + caching/cookie probe ([MDA2AV/Http11Probe](https://github.com/MDA2AV/Http11Probe)).

_Generated: 2026-06-14 08:52 UTC — refreshed weekly by `.github/workflows/chaos.yml`._

## Summary

| Total | Scored | Passed | Warnings | Failed | Errors |
|------:|-------:|-------:|---------:|-------:|-------:|
| 215 | 161 | 148 | 13 | 0 | 0 |

## Failures (1)

| Category | Check | RFC level | Expected | Got |
|----------|-------|-----------|----------|-----|
| Compliance | `COMP-HTTP12-VERSION` | May | 200 or 505 | status=400 conn=ClosedByServer |

## Warnings (27)

| Category | Check | RFC level | Expected | Got |
|----------|-------|-----------|----------|-----|
| Capabilities | `CAP-IMS-FUTURE` | Should | 200 | status=304 conn=Open |
| Compliance | `COMP-ACCEPT-NONSENSE` | Should | 406 or 2xx | status=200 conn=Open |
| Compliance | `COMP-EXPECT-UNKNOWN` | May | 417 or 2xx | status=200 conn=Open |
| Compliance | `COMP-GET-WITH-CL-BODY` | May | 400 or 2xx | status=200 conn=Open |
| Compliance | `COMP-HTTP10-NO-HOST` | May | 200 or 400 | status=200 conn=ClosedByServer |
| Compliance | `COMP-LEADING-CRLF` | Should | 400 or 2xx; close/timeout = warn | status=200 conn=Open |
| Compliance | `COMP-NO-CL-IN-204` | Must | 204 without CL, or 405 | status=200 conn=Open |
| Compliance | `RFC9112-3-MULTI-SP-REQUEST-LINE` | Should | 400 or 2xx; close/timeout = warn | status=200 conn=Open |
| MalformedInput | `MAL-CHUNK-EXT-64K` | NotApplicable | 400 or 2xx | status=200 conn=Open |
| MalformedInput | `MAL-CL-TAB-BEFORE-VALUE` | May | 400 or 2xx | status=200 conn=Open |
| MalformedInput | `MAL-RANGE-OVERLAPPING` | NotApplicable | 200/206/400/416 | status=200 conn=Open |
| MalformedInput | `MAL-URL-PERCENT-CRLF` | NotApplicable | 400 or 2xx/404 | status=200 conn=Open |
| MalformedInput | `MAL-URL-PERCENT-NULL` | NotApplicable | 400 or 2xx/404 | status=200 conn=Open |
| Smuggling | `SMUG-ABSOLUTE-URI-HOST-MISMATCH` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-DOUBLE-ZERO` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-EXTRA-LEADING-SP` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-LEADING-ZEROS` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-LEADING-ZEROS-OCTAL` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL0-BODY-POISON` | NotApplicable | 400/close preferred; poisoned follow-up = warn | status=200 conn=TimedOut |
| Smuggling | `SMUG-EXPECT-100-CL` | Must | 100, 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-HEAD-CL-BODY` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-OPTIONS-CL-BODY` | Must | 400/405 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRAILER-AUTH` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRAILER-CONTENT-TYPE` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRAILER-HOST` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRANSFER_ENCODING` | Must | 400 or 2xx | status=200 conn=Open |
| WebSockets | `WS-UPGRADE-INVALID-VER` | Must | non-101 (426 preferred) | status=200 conn=ClosedByServer |

