# Installing zlib-ng for the gzip backend

`http_compression_gzip.c` is built against either **zlib-ng** (preferred)
or stock **zlib**. Selection is automatic at `./configure` time via
`pkg-config --exists zlib-ng`:

- zlib-ng present → `HAVE_ZLIB_NG=1` is defined, and the gzip backend
  links to `libz-ng` directly (`zng_deflate*` symbols).
- zlib-ng absent  → falls back to system `libz`.

zlib-ng is a drop-in modern zlib reimplementation with SIMD CRC32, faster
hash chains, and better small-input performance. On HTTP gzip workloads
it typically delivers **2–3× the throughput of vanilla zlib at the same
compression level**, with bit-identical output.

You can confirm which engine the running build picked up:

```c
http_compression_engine_name();   // "zlib-ng" or "zlib"
```

Or in PHP:

```php
phpinfo();   // look under "true_async_server" — engine name is in the header
```

## Verifying that detection actually fired

The `./configure` line will report:

```
checking for zlib-ng... yes, version 2.x.y
```

If it says `no`, the `pkg-config --exists zlib-ng` check failed — either
the package is missing or the development headers aren't installed.
`config.h` after `./configure` should then contain:

```c
#define HAVE_ZLIB_NG 1
```

If the line reads `/* #undef HAVE_ZLIB_NG */`, detection failed.

## Distribution packages

### Debian / Ubuntu (22.04+, Debian 12+)

```sh
sudo apt-get install libz-ng-dev
```

The package ships:
- `/usr/include/zlib-ng.h`
- `/usr/lib/x86_64-linux-gnu/libz-ng.so`
- `/usr/lib/x86_64-linux-gnu/pkgconfig/zlib-ng.pc`  ← required for autodetect

### Fedora / RHEL / CentOS Stream

```sh
sudo dnf install zlib-ng-devel
```

### Alpine

```sh
apk add zlib-ng-dev
```

### macOS (Homebrew)

```sh
brew install zlib-ng
```

Homebrew installs to `/opt/homebrew` (Apple Silicon) or `/usr/local`
(Intel). If `./configure` still doesn't detect it, point pkg-config
explicitly:

```sh
PKG_CONFIG_PATH="$(brew --prefix zlib-ng)/lib/pkgconfig:$PKG_CONFIG_PATH" \
  ./configure --enable-http-server
```

### Arch / Manjaro

```sh
sudo pacman -S zlib-ng
```

(Arch ships dev headers in the same package.)

## Building from source (any distro)

When the distro package is unavailable or too old:

```sh
git clone --depth 1 https://github.com/zlib-ng/zlib-ng /tmp/zlib-ng
cd /tmp/zlib-ng
cmake -B build -DZLIB_COMPAT=OFF -DWITH_GTEST=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
```

> `ZLIB_COMPAT=OFF` is important — we want the native API
> (`zng_*` symbols, `zlib-ng.h` header), not the zlib-compat shim. The
> compat shim would still link, but you'd lose the engine-selection
> banner and gain a confusing symbol overlap with system zlib.

After install, re-run the extension's `./configure`; the line should
flip to `checking for zlib-ng... yes`.

## Re-building the extension

zlib-ng selection is captured at `./configure` time, not at compile
time — bumping the system library after the fact is not enough. Always:

```sh
phpize --clean && phpize
./configure --with-php-config=$(which php-config) --enable-true-async-server
make -j"$(nproc)"
```

Then sanity-check:

```sh
strings modules/true_async_server.so | grep -E '^(zng_|deflateInit2)' | sort -u
```

A zlib-ng-linked build prints `zng_deflate`, `zng_deflateInit2`,
`zng_deflateReset`, etc. A vanilla-zlib build only prints `deflateInit2`
and friends.

## Expected throughput delta

Tested locally on `/json/50` (1.7 KiB body, gzip level 6, 4 workers,
c=100, h2load):

| Engine  | RPS    | Note                              |
|---------|-------:|-----------------------------------|
| zlib    | ~40k   | vanilla `libz` 1.2.13             |
| zlib-ng | ~70-90k| projected from upstream benchmarks; verify locally |

The win compounds with the per-thread encoder pool — together they
should bring on-the-fly gzip to ~70-80% of identity throughput on small
JSON bodies.
