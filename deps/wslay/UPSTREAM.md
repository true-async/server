# wslay — bundled dependency

This directory contains a vendored copy of [wslay](https://github.com/tatsuhiro-t/wslay),
a C implementation of the WebSocket protocol (RFC 6455). TrueAsync Server uses
it for WebSocket frame parsing, masking, fragmentation, UTF-8 validation, and
control-frame handling.

| Field | Value |
|---|---|
| Upstream | https://github.com/tatsuhiro-t/wslay |
| Release  | release-1.1.1 |
| Tarball  | https://github.com/tatsuhiro-t/wslay/archive/refs/tags/release-1.1.1.tar.gz |
| License  | MIT (see `LICENSE`) |

## Layout

```
deps/wslay/
├── LICENSE                      MIT license text from upstream COPYING
├── UPSTREAM.md                  this file
├── lib/                         from upstream lib/
│   ├── wslay_event.{c,h}        event-driven API (recv/send callbacks)
│   ├── wslay_frame.{c,h}        low-level frame parser + builder
│   ├── wslay_net.{c,h}          host/network 64-bit byteswap helper
│   ├── wslay_queue.{c,h}        outbound message FIFO
│   └── wslay_stack.{c,h}        small fixed-depth stack helper
└── includes/wslay/
    ├── wslay.h                  public API header
    └── wslayver.h               version macro (hand-written, see below)
```

`wslayver.h` is hand-written for this vendoring (upstream generates it from
`wslayver.h.in` via autoconf). Keep its `WSLAY_VERSION` literal in sync with
the version recorded in this file when bumping.

The upstream `lib/config.h.in` is **not** vendored. wslay's only build-time
configuration sentinels are `HAVE_NETINET_IN_H`, `HAVE_ARPA_INET_H`,
`HAVE_WINSOCK2_H`, and `WORDS_BIGENDIAN` — the first three already come from
the project's top-level `config.h` (probed by `config.m4`), and the fourth is
left undefined so wslay falls back to a portable manual byteswap on every
host.

## Updating

Run `tools/update-wslay.sh <version>` from the repository root, e.g.

```bash
tools/update-wslay.sh 1.1.1
```

The script downloads the upstream release tarball, extracts the ten library
sources plus the public header, and overwrites this directory in place.
After running, manually update `WSLAY_VERSION` in
`includes/wslay/wslayver.h` and the version line above. Review the diff and
commit the result.
