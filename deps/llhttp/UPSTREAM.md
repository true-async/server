# llhttp — bundled dependency

This directory contains a vendored copy of [llhttp](https://github.com/nodejs/llhttp),
the HTTP/1.x parser used by Node.js. TrueAsync Server uses it to parse HTTP/1.1
requests on the server side.

| Field | Value |
|---|---|
| Upstream | https://github.com/nodejs/llhttp |
| Release  | v9.3.0 |
| Tarball  | https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.3.0.tar.gz |
| License  | MIT (see `LICENSE`) |

## Layout

```
deps/llhttp/
├── LICENSE          MIT license text from upstream
├── UPSTREAM.md      this file
├── api.c            from upstream src/
├── http.c           from upstream src/
├── llhttp.c         pre-generated parser (release artifact)
├── llhttp.h         public header (release artifact)
└── include/
    └── llhttp.h     same header, exposed on the include path
```

`llhttp.c` and `llhttp.h` are pre-generated artifacts shipped in the upstream
release tarball under `release/`; they are not produced from this tree.

## Updating

Run `tools/update-llhttp.sh <version>` from the repository root, e.g.

```bash
tools/update-llhttp.sh 9.3.0
```

The script downloads the upstream release, extracts the four required files,
and overwrites this directory in place. Review the diff and commit the result
together with a version bump in this file.
