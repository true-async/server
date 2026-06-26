# QUIC interop-runner endpoint (HTTP/3 conformance, #80 / P0)

Wires the TrueAsync HTTP/3 server into the
[quic-interop-runner](https://github.com/quic-interop/quic-interop-runner) — the
IETF cross-implementation test harness. It pairs our server against third-party
QUIC clients (quic-go, ngtcp2, mvfst, quiche, picoquic, …) over a simulated
network (ns-3), giving conformance coverage of **RFC 9000** (QUIC transport),
**9001** (QUIC-TLS), **9002** (recovery), **9114** (HTTP/3) and **9204** (QPACK)
that the in-tree phpt suite (own client only) can't.

## Files

| File | Role |
|------|------|
| `entry.php` | Server: serves `/www` over H3 (UDP) + H2/H1 (TLS, TCP) on one port, with `/certs/{cert.pem,priv.key}`. Validated locally — see below. |
| `run_endpoint.sh` | Endpoint hook: role + test-case gating (`exit 127` for unsupported), launches the server on 443. |
| `Dockerfile` | Published TrueAsync runtime layered onto the simulator endpoint base. |
| `implementations.snippet.json` | Entry to add to the runner's `implementations_quic.json`. |

## Running (needs a Docker host)

The runner is Docker- + ns-3-only; it cannot run in a plain WSL distro without
Docker. On a Docker host:

```bash
# 1. Build the endpoint image (pin TAS_IMAGE to the matching release tag).
docker build -t trueasync/quic-interop-endpoint:latest \
  --build-arg TAS_IMAGE=trueasync/php-true-async:0.7.2-php8.6 \
  -f tests/interop/quic/Dockerfile tests/interop/quic

# 2. In a quic-interop-runner checkout, merge implementations.snippet.json into
#    implementations_quic.json, then run our server against all clients:
python run.py -s trueasync -t handshake,transfer,http3,multiplexing
```

## Supported test cases (this iteration)

Cases a correct H3 file server passes via **downloaded-file integrity** (the
runner compares the bytes the client pulled):

`handshake`, `transfer`, `http3`, `multiplexing`, `longrtt`, `goodput`,
`crosstraffic`, `transferloss`, `transfercorruption`, `blackhole`,
`handshakeloss`.

Everything else returns `exit 127` (skipped) for now — see gaps.

## Known gaps / follow-ups

1. **Server-side `SSLKEYLOGFILE` + `QLOGDIR` export is not wired** (TLS-layer
   follow-up). The runner verifies the cases above by downloaded-file integrity,
   which works without it; cases it verifies by inspecting *server* packet traces
   (`amplificationlimit`, `ecn`, …) need it and are screened out.
2. **Special-config cases not yet enabled:** `retry` (force address validation),
   `resumption` (TLS tickets), `zerortt` (0-RTT early data — code exists, needs
   wiring), `chacha20` (cipher pin), `keyupdate`, `v2` (QUIC v2), `multiconnect`.
3. **Not run here:** this WSL distro has no Docker, so the matrix above is the
   set expected to pass on a correct stack — confirm actual pass/fail on a Docker
   host / CI.

## Local validation (no Docker)

`entry.php`'s serving logic is validated against our own `tests/h3client`:
start it on a high port with `examples/certs`, `GET` a file, and the body matches
byte-for-byte (`STATUS=200`, exact SHA-256). Only the Docker/network wrapping is
unexercised in this environment.

```bash
INTEROP_WWW=/path/to/www INTEROP_CERT=examples/certs/server.crt \
INTEROP_KEY=examples/certs/server.key INTEROP_PORT=8543 WORKERS=1 \
  php -d extension=./modules/true_async_server.so tests/interop/quic/entry.php &
tests/h3client/h3client 127.0.0.1 8543 /yourfile
```
