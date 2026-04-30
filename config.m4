dnl config.m4 for extension http_server

PHP_ARG_ENABLE([http-server],
  [whether to enable http_server support],
  [AS_HELP_STRING([--enable-http-server],
    [Enable http_server support])],
  [no])

PHP_ARG_ENABLE([http2],
  [whether to enable HTTP/2 support],
  [AS_HELP_STRING([--enable-http2],
    [Enable HTTP/2 support (requires nghttp2)])],
  [no],
  [no])

PHP_ARG_ENABLE([http3],
  [whether to enable HTTP/3 support],
  [AS_HELP_STRING([--enable-http3],
    [Enable HTTP/3 support (auto-detected; requires ngtcp2, nghttp3, OpenSSL >= 3.5; use --disable-http3 to opt out)])],
  [yes],
  [no])

PHP_ARG_ENABLE([websocket],
  [whether to enable WebSocket support],
  [AS_HELP_STRING([--enable-websocket],
    [Enable WebSocket (RFC 6455) support; uses bundled wslay (default: enabled)])],
  [yes],
  [no])

PHP_ARG_WITH([openssl],
  [for OpenSSL TLS support],
  [AS_HELP_STRING([--with-openssl@<:@=DIR@:>@],
    [Enable TLS support via OpenSSL >= 3.0 (auto-detected; use --without-openssl to disable)])],
  [yes],
  [yes])

PHP_ARG_WITH([nghttp2],
  [for nghttp2 support],
  [AS_HELP_STRING([--with-nghttp2],
    [Path to nghttp2 library])],
  [no],
  [no])

PHP_ARG_WITH([ngtcp2],
  [for ngtcp2 support],
  [AS_HELP_STRING([--with-ngtcp2],
    [Path to ngtcp2 library])],
  [no],
  [no])

PHP_ARG_WITH([nghttp3],
  [for nghttp3 support],
  [AS_HELP_STRING([--with-nghttp3],
    [Path to nghttp3 library])],
  [no],
  [no])

PHP_ARG_ENABLE([tests],
  [whether to build tests],
  [AS_HELP_STRING([--enable-tests],
    [Build unit tests (requires CMocka)])],
  [no],
  [no])

PHP_ARG_ENABLE([coverage],
  [whether to enable code coverage],
  [AS_HELP_STRING([--enable-coverage],
    [Enable code coverage (requires gcov)])],
  [no],
  [no])

if test "$PHP_HTTP_SERVER" != "no"; then
  dnl Check for required headers
  AC_CHECK_HEADERS([sys/socket.h netinet/in.h arpa/inet.h])

  dnl Use bundled llhttp
  AC_MSG_CHECKING([for llhttp])
  AC_MSG_RESULT([using bundled llhttp])
  PHP_ADD_INCLUDE([$ext_srcdir/deps/llhttp/include])
  PHP_ADD_INCLUDE([$ext_builddir/deps/llhttp/include])
  AC_DEFINE([HAVE_LLHTTP], [1], [Whether llhttp is available])

  dnl Bundled wslay (RFC 6455 frame parser). Gated by --enable-websocket.
  dnl PHP_ADD_INCLUDE for wslay is intentionally deferred until after
  dnl PHP_NEW_EXTENSION runs (see the include-path block at the bottom
  dnl of this file) — $ext_srcdir is empty before that macro fires.
  if test "$PHP_WEBSOCKET" = "yes"; then
    AC_MSG_CHECKING([for wslay])
    AC_MSG_RESULT([using bundled wslay])
    AC_DEFINE([HAVE_WSLAY], [1], [Whether bundled wslay is available])
    AC_DEFINE([HAVE_HTTP_SERVER_WEBSOCKET], [1], [Whether WebSocket support is enabled])
  fi

  dnl Macro for checking library with pkg-config
  AC_DEFUN([PHP_CHECK_LIBRARY_PKG_CONFIG], [
    AC_PATH_PROG(PKG_CONFIG, pkg-config, no)

    if test -x "$PKG_CONFIG" && $PKG_CONFIG --exists $1; then
      $2_CFLAGS=`$PKG_CONFIG $1 --cflags`
      $2_LIBS=`$PKG_CONFIG $1 --libs`
      $2_VERSION=`$PKG_CONFIG $1 --modversion`

      AC_MSG_CHECKING([for $1])
      AC_MSG_RESULT([yes (version $$2_VERSION)])

      $3  dnl Success action
    else
      AC_MSG_CHECKING([for $1])
      AC_MSG_RESULT([no])

      $4  dnl Failure action
    fi
  ])

  dnl OpenSSL / TLS support (auto-detected — uses the same OpenSSL that PHP
  dnl itself is built against, so symbol/ABI stays consistent).
  dnl
  dnl --with-openssl      → auto-detect via pkg-config (default).
  dnl --with-openssl=DIR  → look under DIR/lib/pkgconfig first.
  dnl --without-openssl   → disable TLS support entirely.
  if test "$PHP_OPENSSL" != "no"; then
    dnl If a prefix was given, prepend its pkgconfig directory.
    if test "$PHP_OPENSSL" != "yes"; then
      if test -d "$PHP_OPENSSL/lib/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_OPENSSL/lib/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      elif test -d "$PHP_OPENSSL/lib64/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_OPENSSL/lib64/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      fi
    fi

    dnl pkg-config with a version constraint — call directly because the
    dnl generic PHP_CHECK_LIBRARY_PKG_CONFIG macro fails to quote $1 and
    dnl shell treats ">=" as a redirection.
    AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
    AC_MSG_CHECKING([for OpenSSL >= 3.0])
    _http_server_openssl_ok=no
    if test -x "$PKG_CONFIG"; then
      if "$PKG_CONFIG" --atleast-version=3.0 openssl 2>/dev/null; then
        OPENSSL_CFLAGS=`"$PKG_CONFIG" --cflags openssl`
        OPENSSL_LIBS=`"$PKG_CONFIG" --libs openssl`
        OPENSSL_VERSION=`"$PKG_CONFIG" --modversion openssl`
        _http_server_openssl_ok=yes
      fi
    fi

    if test "$_http_server_openssl_ok" = "yes"; then
      AC_MSG_RESULT([yes (version $OPENSSL_VERSION)])
      PHP_EVAL_LIBLINE($OPENSSL_LIBS, TRUE_ASYNC_SERVER_SHARED_LIBADD)
      PHP_EVAL_INCLINE($OPENSSL_CFLAGS)
      AC_DEFINE([HAVE_OPENSSL], [1], [Whether OpenSSL TLS support is available])

      dnl Linux kTLS opportunistic optimization — detect at configure time,
      dnl engage at runtime per connection (see docs/PLAN_TLS.md §7).
      _http_server_saved_cflags="$CFLAGS"
      CFLAGS="$CFLAGS $OPENSSL_CFLAGS"
      AC_CHECK_DECL([BIO_get_ktls_send], [
        AC_DEFINE([HAVE_KTLS], [1], [Whether OpenSSL exposes kTLS BIO controls])
      ], [], [[#include <openssl/bio.h>]])
      CFLAGS="$_http_server_saved_cflags"
    else
      AC_MSG_RESULT([no])
      if test "$PHP_OPENSSL" != "yes"; then
        AC_MSG_ERROR([OpenSSL >= 3.0 not found at $PHP_OPENSSL. Install it or use --without-openssl.])
      else
        AC_MSG_WARN([OpenSSL >= 3.0 not found via pkg-config; TLS support disabled. Install libssl-dev (or openssl@3 on macOS) to enable.])
      fi
    fi
  fi

  dnl HTTP/2 support. Version floor is 1.57.0 — earlier nghttp2 lacks the
  dnl built-in rapid-reset mitigation (CVE-2023-44487). See docs/PLAN_HTTP2.md §2.2.
  if test "$PHP_HTTP2" = "yes"; then
    if test "$PHP_NGHTTP2" != "no" -a "$PHP_NGHTTP2" != "yes"; then
      if test -d "$PHP_NGHTTP2/lib/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_NGHTTP2/lib/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      elif test -d "$PHP_NGHTTP2/lib64/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_NGHTTP2/lib64/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      fi
    fi

    AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
    AC_MSG_CHECKING([for libnghttp2 >= 1.57])
    _http_server_nghttp2_ok=no
    if test -x "$PKG_CONFIG"; then
      if "$PKG_CONFIG" --atleast-version=1.57 libnghttp2 2>/dev/null; then
        NGHTTP2_CFLAGS=`"$PKG_CONFIG" --cflags libnghttp2`
        NGHTTP2_LIBS=`"$PKG_CONFIG" --libs libnghttp2`
        NGHTTP2_VERSION=`"$PKG_CONFIG" --modversion libnghttp2`
        _http_server_nghttp2_ok=yes
      fi
    fi

    if test "$_http_server_nghttp2_ok" = "yes"; then
      AC_MSG_RESULT([yes (version $NGHTTP2_VERSION)])
      PHP_EVAL_LIBLINE($NGHTTP2_LIBS, TRUE_ASYNC_SERVER_SHARED_LIBADD)
      PHP_EVAL_INCLINE($NGHTTP2_CFLAGS)
      AC_DEFINE([HAVE_HTTP2], [1], [Whether HTTP/2 support is enabled])
    else
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([libnghttp2 >= 1.57 not found. Install libnghttp2-dev (>= 1.57) or use --with-nghttp2=/path to a newer build. Version floor required for rapid-reset (CVE-2023-44487) mitigation.])
    fi
  fi

  dnl HTTP/3 support. Version floors per docs/PLAN_HTTP3.md §2:
  dnl   ngtcp2 >= 1.6.0, nghttp3 >= 1.4.0, OpenSSL >= 3.5.0 (QUIC API).
  dnl   libngtcp2_crypto_ossl is the OpenSSL glue that binds SSL_CTX* to
  dnl   ngtcp2_conn — required, not a nice-to-have.
  dnl Default: auto-detect — H3 is enabled if all prerequisites are present.
  dnl Use --disable-http3 to opt out.
  dnl Fail-soft policy: if any prerequisite is missing, emit a warning and
  dnl leave HAVE_HTTP_SERVER_HTTP3 undefined. The build completes and the
  dnl .so loads; H3 simply is not compiled in.
  if test "$PHP_HTTP3" = "yes"; then
    _http_server_http3_ok=yes

    if test "$PHP_NGTCP2" != "no" -a "$PHP_NGTCP2" != "yes"; then
      if test -d "$PHP_NGTCP2/lib/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_NGTCP2/lib/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      elif test -d "$PHP_NGTCP2/lib64/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_NGTCP2/lib64/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      fi
    fi
    if test "$PHP_NGHTTP3" != "no" -a "$PHP_NGHTTP3" != "yes"; then
      if test -d "$PHP_NGHTTP3/lib/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_NGHTTP3/lib/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      elif test -d "$PHP_NGHTTP3/lib64/pkgconfig"; then
        PKG_CONFIG_PATH="$PHP_NGHTTP3/lib64/pkgconfig:$PKG_CONFIG_PATH"
        export PKG_CONFIG_PATH
      fi
    fi

    AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])

    dnl OpenSSL >= 3.5.0 — QUIC crypto callbacks (SSL_set_quic_tls_cbs)
    dnl arrived there. Reuse the earlier detection's OPENSSL_VERSION when
    dnl present rather than re-probing.
    AC_MSG_CHECKING([for OpenSSL >= 3.5 (HTTP/3)])
    _http_server_openssl35_ok=no
    if test -x "$PKG_CONFIG"; then
      if "$PKG_CONFIG" --atleast-version=3.5 openssl 2>/dev/null; then
        _http_server_openssl35_ok=yes
      fi
    fi
    if test "$_http_server_openssl35_ok" = "yes"; then
      AC_MSG_RESULT([yes])
    else
      AC_MSG_RESULT([no])
      AC_MSG_WARN([HTTP/3 disabled: OpenSSL >= 3.5 is required for the QUIC TLS API (ngtcp2_crypto_ossl).])
      _http_server_http3_ok=no
    fi

    AC_MSG_CHECKING([for libngtcp2 >= 1.6])
    _http_server_ngtcp2_ok=no
    if test -x "$PKG_CONFIG"; then
      if "$PKG_CONFIG" --atleast-version=1.6 libngtcp2 2>/dev/null; then
        NGTCP2_CFLAGS=`"$PKG_CONFIG" --cflags libngtcp2`
        NGTCP2_LIBS=`"$PKG_CONFIG" --libs libngtcp2`
        NGTCP2_VERSION=`"$PKG_CONFIG" --modversion libngtcp2`
        _http_server_ngtcp2_ok=yes
      fi
    fi
    if test "$_http_server_ngtcp2_ok" = "yes"; then
      AC_MSG_RESULT([yes (version $NGTCP2_VERSION)])
    else
      AC_MSG_RESULT([no])
      AC_MSG_WARN([HTTP/3 disabled: libngtcp2 >= 1.6 not found. Install libngtcp2-dev or use --with-ngtcp2=/path.])
      _http_server_http3_ok=no
    fi

    AC_MSG_CHECKING([for libngtcp2_crypto_ossl])
    _http_server_ngtcp2_ossl_ok=no
    if test -x "$PKG_CONFIG"; then
      if "$PKG_CONFIG" --exists libngtcp2_crypto_ossl 2>/dev/null; then
        NGTCP2_OSSL_CFLAGS=`"$PKG_CONFIG" --cflags libngtcp2_crypto_ossl`
        NGTCP2_OSSL_LIBS=`"$PKG_CONFIG" --libs libngtcp2_crypto_ossl`
        _http_server_ngtcp2_ossl_ok=yes
      fi
    fi
    if test "$_http_server_ngtcp2_ossl_ok" = "yes"; then
      AC_MSG_RESULT([yes])
    else
      AC_MSG_RESULT([no])
      AC_MSG_WARN([HTTP/3 disabled: libngtcp2_crypto_ossl not found. Build ngtcp2 with --with-openssl.])
      _http_server_http3_ok=no
    fi

    AC_MSG_CHECKING([for libnghttp3 >= 1.4])
    _http_server_nghttp3_ok=no
    if test -x "$PKG_CONFIG"; then
      if "$PKG_CONFIG" --atleast-version=1.4 libnghttp3 2>/dev/null; then
        NGHTTP3_CFLAGS=`"$PKG_CONFIG" --cflags libnghttp3`
        NGHTTP3_LIBS=`"$PKG_CONFIG" --libs libnghttp3`
        NGHTTP3_VERSION=`"$PKG_CONFIG" --modversion libnghttp3`
        _http_server_nghttp3_ok=yes
      fi
    fi
    if test "$_http_server_nghttp3_ok" = "yes"; then
      AC_MSG_RESULT([yes (version $NGHTTP3_VERSION)])
    else
      AC_MSG_RESULT([no])
      AC_MSG_WARN([HTTP/3 disabled: libnghttp3 >= 1.4 not found. Install libnghttp3-dev or use --with-nghttp3=/path.])
      _http_server_http3_ok=no
    fi

    if test "$_http_server_http3_ok" = "yes"; then
      PHP_EVAL_LIBLINE($NGTCP2_LIBS, TRUE_ASYNC_SERVER_SHARED_LIBADD)
      PHP_EVAL_INCLINE($NGTCP2_CFLAGS)
      PHP_EVAL_LIBLINE($NGTCP2_OSSL_LIBS, TRUE_ASYNC_SERVER_SHARED_LIBADD)
      PHP_EVAL_INCLINE($NGTCP2_OSSL_CFLAGS)
      PHP_EVAL_LIBLINE($NGHTTP3_LIBS, TRUE_ASYNC_SERVER_SHARED_LIBADD)
      PHP_EVAL_INCLINE($NGHTTP3_CFLAGS)
      AC_DEFINE([HAVE_NGTCP2], [1], [Whether ngtcp2 is available])
      AC_DEFINE([HAVE_NGHTTP3], [1], [Whether nghttp3 is available])
      AC_DEFINE([HAVE_HTTP_SERVER_HTTP3], [1], [Whether HTTP/3 support is enabled])
    else
      dnl Record the user asked for H3 but we could not satisfy it — downstream
      dnl Makefile sources are still gated on $PHP_HTTP3; flip it to "no" so
      dnl build directories are not emitted for an empty build.
      PHP_HTTP3=no
    fi
  fi

  dnl Unit tests support (CMocka)
  if test "$PHP_TESTS" = "yes"; then
    AC_CHECK_LIB(cmocka, _cmocka_run_group_tests, [
      PHP_ADD_LIBRARY(cmocka, 1, TRUE_ASYNC_SERVER_SHARED_LIBADD)
      AC_DEFINE(HAVE_CMOCKA, 1, [Whether CMocka is available])
    ], [
      AC_MSG_ERROR([CMocka library not found. Install libcmocka-dev])
    ])
  fi

  dnl Code coverage support
  if test "$PHP_COVERAGE" = "yes"; then
    CFLAGS="$CFLAGS -fprofile-arcs -ftest-coverage"
    LDFLAGS="$LDFLAGS -lgcov"
    AC_DEFINE(HAVE_COVERAGE, 1, [Whether code coverage is enabled])
  fi

  dnl Define source files
  dnl Phase 1: HTTP/1.1 Parser
  dnl Phase 2: Server classes
  http_server_sources="
    deps/llhttp/llhttp.c
    deps/llhttp/api.c
    deps/llhttp/http.c
    src/http_server.c
    src/http_server_exceptions.c
    src/http_server_config.c
    src/http_server_class.c
    src/core/http_connection.c
    src/http1/http_parser.c
    src/http1/http1_stream.c
    src/formats/multipart_parser.c
    src/formats/multipart_processor.c
    src/http_request.c
    src/http_response.c
    src/uploaded_file.c
    src/core/http_protocol_handlers.c
    src/core/http_protocol_strategy.c
    src/core/tls_layer.c
    src/core/http_known_strings.c
    src/log/http_log.c
    src/log/trace_context.c
  "

  dnl TLS-only TU split out of http_connection.c (BIO ring writer +
  dnl handshake/decrypt coroutine). Compiled only when OpenSSL is present
  dnl so non-TLS builds stay smaller.
  if test "$_http_server_openssl_ok" = "yes"; then
    http_server_sources="$http_server_sources src/core/http_connection_tls.c"
  fi

  dnl HTTP/2 sources compile only when --enable-http2 succeeded and
  dnl HAVE_HTTP2 is defined. The build system is the single source of
  dnl truth — files are simply absent from the source list when disabled.
  if test "$PHP_HTTP2" = "yes"; then
    http_server_sources="$http_server_sources
      src/http2/http2_strategy.c
      src/http2/http2_session.c
      src/http2/http2_stream.c
    "
  fi

  dnl WebSocket — bundled wslay + our strategy. Both gated on
  dnl --enable-websocket. Frame I/O, handshake, and the public PHP
  dnl API land in follow-up commits; the strategy file is a scaffold
  dnl at this stage (see src/websocket/websocket_strategy.c).
  if test "$PHP_WEBSOCKET" = "yes"; then
    http_server_sources="$http_server_sources
      deps/wslay/lib/wslay_event.c
      deps/wslay/lib/wslay_frame.c
      deps/wslay/lib/wslay_net.c
      deps/wslay/lib/wslay_queue.c
      deps/wslay/lib/wslay_stack.c
      src/websocket/websocket_strategy.c
      src/websocket/ws_session.c
      src/websocket/ws_handshake.c
    "
  fi

  dnl HTTP/3 sources — gated by the same PHP_HTTP3=yes set by the detection
  dnl block above. Files appear in the build only when H3 detection
  dnl succeeded; no internal #ifdef wrap is needed.
  if test "$PHP_HTTP3" = "yes"; then
    http_server_sources="$http_server_sources
      src/http3/http3_listener.c
      src/http3/http3_packet.c
      src/http3/http3_connection.c
      src/http3/http3_io.c
      src/http3/http3_callbacks.c
      src/http3/http3_dispatch.c
      src/http3/http3_stream.c
    "
  fi

  dnl Hardening + diagnostic flags. Probed individually so old/non-GCC
  dnl toolchains don't break — flags that aren't accepted are dropped
  dnl silently. -fstack-protector-strong and -Wformat=2 are universally
  dnl supported on any Clang/GCC produced this decade; the rest are
  dnl belt-and-braces. _FORTIFY_SOURCE intentionally NOT added here
  dnl because (a) PHP debug builds compile -O0 and the macro warns,
  dnl (b) distros already pass it via system CFLAGS for release builds.
  HTTP_SERVER_HARDENING=""
  SAVE_CFLAGS="$CFLAGS"
  dnl -Wshadow excluded — Zend hash/zval helper macros legitimately
  dnl shadow names by design (`_z`, `__ht`, `_count`); enabling it
  dnl drowns the build log without surfacing any project bug.
  for flag in -fstack-protector-strong -Wformat=2 -Wstrict-prototypes -Wsign-compare; do
    AC_MSG_CHECKING([whether $CC accepts $flag])
    CFLAGS="$SAVE_CFLAGS $flag -Werror"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
      [AC_MSG_RESULT([yes])
       HTTP_SERVER_HARDENING="$HTTP_SERVER_HARDENING $flag"],
      [AC_MSG_RESULT([no])])
  done
  CFLAGS="$SAVE_CFLAGS"

  dnl Create extension
  PHP_NEW_EXTENSION(true_async_server, $http_server_sources, $ext_shared,, -Wall -Wextra -std=c11 $HTTP_SERVER_HARDENING)
  PHP_SUBST(TRUE_ASYNC_SERVER_SHARED_LIBADD)

  dnl Add include paths
  PHP_ADD_INCLUDE([$ext_srcdir/include])
  PHP_ADD_INCLUDE([$ext_srcdir/src])
  PHP_ADD_INCLUDE([$ext_srcdir/src/core])
  PHP_ADD_BUILD_DIR([$ext_builddir/src])
  PHP_ADD_BUILD_DIR([$ext_builddir/src/core])
  PHP_ADD_BUILD_DIR([$ext_builddir/src/http1])
  PHP_ADD_BUILD_DIR([$ext_builddir/src/formats])
  PHP_ADD_BUILD_DIR([$ext_builddir/src/log])
  PHP_ADD_BUILD_DIR([$ext_builddir/deps/llhttp])

  if test "$PHP_HTTP2" = "yes"; then
    PHP_ADD_BUILD_DIR([$ext_builddir/src/http2])
  fi

  if test "$PHP_WEBSOCKET" = "yes"; then
    PHP_ADD_BUILD_DIR([$ext_builddir/deps/wslay/lib])
    PHP_ADD_BUILD_DIR([$ext_builddir/src/websocket])
    dnl wslay's own headers do #include <wslay/wslay.h>, so the include
    dnl path is mandatory (unlike llhttp, which is only ever included
    dnl via the relative "../deps/llhttp/..." path from src/).
    PHP_ADD_INCLUDE([$ext_srcdir/deps/wslay/includes])
    PHP_ADD_INCLUDE([$ext_srcdir/include/websocket])
  fi

  if test "$PHP_HTTP3" = "yes"; then
    PHP_ADD_BUILD_DIR([$ext_builddir/src/http3])
    PHP_ADD_INCLUDE([$ext_srcdir/src/http3])
  fi
fi
