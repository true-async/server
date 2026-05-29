# Http11Probe conformance report

HTTP/1.1 RFC 9110/9112 compliance + request-smuggling + malformed-input + header-normalization + caching/cookie probe ([MDA2AV/Http11Probe](https://github.com/MDA2AV/Http11Probe)).

_Generated: 2026-05-29 16:41 UTC — refreshed weekly by `.github/workflows/chaos.yml`._

## Summary

| Total | Scored | Passed | Warnings | Failed | Errors |
|------:|-------:|-------:|---------:|-------:|-------:|
| 215 | 161 | 137 | 18 | 6 | 0 |

## Failures (10)

| Category | Check | RFC level | Expected | Got |
|----------|-------|-----------|----------|-----|
| Compliance | `COMP-CHUNKED-BODY` | Must | 2xx + echo | status=200 conn=Open |
| Compliance | `COMP-CHUNKED-EXTENSION` | Must | 2xx preferred; 400 warns | status=200 conn=Open |
| Compliance | `COMP-CHUNKED-HEX-UPPERCASE` | Must | 2xx + echo | status=200 conn=Open |
| Compliance | `COMP-CHUNKED-MULTI` | Must | 2xx + echo | status=200 conn=Open |
| Compliance | `COMP-CHUNKED-TRAILER-VALID` | Must | 2xx + echo | status=200 conn=Open |
| Compliance | `COMP-HTTP12-VERSION` | May | 200 or 505 | status=400 conn=ClosedByServer |
| Compliance | `COMP-POST-CL-BODY` | Must | 2xx + echo | status=200 conn=Open |
| Cookies | `COOK-ECHO` | NotApplicable | 2xx with Cookie in body | status=200 conn=Open |
| Cookies | `COOK-PARSED-BASIC` | NotApplicable | 2xx with foo=bar in body | status=200 conn=Open |
| Cookies | `COOK-PARSED-MULTI` | NotApplicable | 2xx with a=1, b=2, c=3 in body | status=200 conn=Open |

## Warnings (39)

| Category | Check | RFC level | Expected | Got |
|----------|-------|-----------|----------|-----|
| Capabilities | `CAP-ETAG-304` | Should | 304 | status=200 conn=Open |
| Capabilities | `CAP-ETAG-IN-304` | Should | 304 with ETag | status=200 conn=Open |
| Capabilities | `CAP-ETAG-WEAK` | Should | 304 | status=200 conn=Open |
| Capabilities | `CAP-INM-PRECEDENCE` | Should | 304 | status=200 conn=Open |
| Capabilities | `CAP-INM-WILDCARD` | Should | 304 | status=200 conn=Open |
| Capabilities | `CAP-LAST-MODIFIED-304` | Should | 304 | status=200 conn=Open |
| Compliance | `COMP-405-ALLOW` | Must | 405 + Allow header | status=200 conn=Open |
| Compliance | `COMP-ACCEPT-NONSENSE` | Should | 406 or 2xx | status=200 conn=Open |
| Compliance | `COMP-DUPLICATE-CT` | Should | 400 or 2xx | status=200 conn=Open |
| Compliance | `COMP-EXPECT-UNKNOWN` | May | 417 or 2xx | status=200 conn=Open |
| Compliance | `COMP-GET-WITH-CL-BODY` | May | 400 or 2xx | status=200 conn=Open |
| Compliance | `COMP-HTTP10-NO-HOST` | May | 200 or 400 | status=200 conn=ClosedByServer |
| Compliance | `COMP-LEADING-CRLF` | Should | 400 or 2xx; close/timeout = warn | status=200 conn=Open |
| Compliance | `COMP-METHOD-TRACE` | Should | 405/501 or 2xx | status=200 conn=Open |
| Compliance | `COMP-NO-CL-IN-204` | Must | 204 without CL, or 405 | status=200 conn=Open |
| Compliance | `COMP-OPTIONS-ALLOW` | Should | 2xx with Allow header, or 405 | status=200 conn=Open |
| Compliance | `COMP-TRACE-WITH-BODY` | Should | 400/405 or 200 | status=200 conn=Open |
| Compliance | `RFC9112-3-MULTI-SP-REQUEST-LINE` | Should | 400 or 2xx; close/timeout = warn | status=200 conn=Open |
| Compliance | `RFC9112-3.2-FRAGMENT-IN-TARGET` | Should | 400 or 2xx; 404 = warn | status=200 conn=Open |
| Cookies | `COOK-MULTI-HEADER` | NotApplicable | 2xx with both cookies | status=200 conn=Open |
| MalformedInput | `MAL-CHUNK-EXT-64K` | NotApplicable | 400 or 2xx | status=200 conn=Open |
| MalformedInput | `MAL-CL-TAB-BEFORE-VALUE` | May | 400 or 2xx | status=200 conn=Open |
| MalformedInput | `MAL-RANGE-OVERLAPPING` | NotApplicable | 200/206/400/416 | status=200 conn=Open |
| MalformedInput | `MAL-URL-BACKSLASH` | Should | 400 or 2xx/404 | status=200 conn=Open |
| MalformedInput | `MAL-URL-PERCENT-CRLF` | NotApplicable | 400 or 2xx/404 | status=200 conn=Open |
| MalformedInput | `MAL-URL-PERCENT-NULL` | NotApplicable | 400 or 2xx/404 | status=200 conn=Open |
| Smuggling | `SMUG-ABSOLUTE-URI-HOST-MISMATCH` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-DOUBLE-ZERO` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-EXTRA-LEADING-SP` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-LEADING-ZEROS` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-CL-LEADING-ZEROS-OCTAL` | Should | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-EXPECT-100-CL` | Must | 100, 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-HEAD-CL-BODY` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-OPTIONS-CL-BODY` | Must | 400/405 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRAILER-AUTH` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRAILER-CONTENT-TYPE` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRAILER-HOST` | Must | 400 or 2xx | status=200 conn=Open |
| Smuggling | `SMUG-TRANSFER_ENCODING` | Must | 400 or 2xx | status=200 conn=Open |
| WebSockets | `WS-UPGRADE-INVALID-VER` | Must | non-101 (426 preferred) | status=200 conn=ClosedByServer |

