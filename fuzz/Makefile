# libFuzzer harness build (PLAN_ATTACK_TESTING Layer 2).
#
# Two flavours:
#   * standalone harnesses (multipart): state machines that work
#     against libc malloc — no PHP runtime needed. Fast execs.
#   * embedded harnesses (http1_parser, later h2/tls): parser
#     requires emalloc / zend_string — we link libphp and bring up
#     php_embed once in LLVMFuzzerInitialize. Slower (~10–20 k exec/s
#     vs 175 k) but covers our actual production wiring.
#
# Usage:
#     make -f fuzz/Makefile fuzz_multipart
#     make -f fuzz/Makefile fuzz_http1_parser
#     ./fuzz/fuzz_multipart -max_total_time=300 fuzz/corpus/multipart/
#     ./fuzz/fuzz_http1_parser -max_total_time=300 fuzz/corpus/http1/

CC       = clang
CFLAGS   = -g -O1 -fno-omit-frame-pointer \
           -fsanitize=fuzzer,address,undefined \
           -D_GNU_SOURCE
INCLUDES = -I../include -I../include/formats -I../include/http1 \
           -I../src -I../src/core -I../src/http1 -I../src/http2 \
           -I../src/formats -I../src/log -I../deps/llhttp

PHP_CFLAGS  = $(shell php-config --includes)
PHP_LDFLAGS = -L$(shell php-config --prefix)/lib -lphp $(shell php-config --libs) \
              -Wl,-rpath,$(shell php-config --prefix)/lib

MULTIPART_SRC = ../src/formats/multipart_parser.c

HTTP1_SRC = \
    ../src/http1/http_parser.c \
    ../src/core/body_pool.c \
    ../src/formats/multipart_parser.c \
    ../src/formats/multipart_processor.c \
    ../src/http_param_parse.c \
    ../src/http_rfc5987.c \
    ../src/http_body_stream.c \
    ../deps/llhttp/api.c \
    ../deps/llhttp/http.c \
    ../deps/llhttp/llhttp.c

# Stubs for extension-level symbols that http_parser.c / http2 TUs
# reference at link time but don't need in parser-only fuzzing
# (http_exception_ce, server-telemetry hooks). Same pattern as
# tests/unit/common/multipart_stubs.c — weak no-op defaults.
STUBS_SRC = ../tests/unit/common/multipart_stubs.c fuzz_stubs.c

.PHONY: all clean
all: fuzz_multipart

fuzz_multipart: fuzz_multipart.c $(MULTIPART_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

fuzz_http1_parser: fuzz_http1_parser.c harness_common.c $(HTTP1_SRC) $(STUBS_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(PHP_CFLAGS) \
	      -DHAVE_LLHTTP=1 -DHAVE_PHP_H=1 \
	      -o $@ $^ $(PHP_LDFLAGS)

# HTTP/2 session fuzzer — same PHP embed bootstrap plus libnghttp2.
H2_SRC = \
    ../src/http2/http2_session.c \
    ../src/http2/http2_stream.c \
    ../src/http1/http_parser.c \
    ../src/core/body_pool.c \
    ../src/formats/multipart_parser.c \
    ../src/formats/multipart_processor.c \
    ../src/http_param_parse.c \
    ../src/http_rfc5987.c \
    ../src/http_body_stream.c \
    ../deps/llhttp/api.c \
    ../deps/llhttp/http.c \
    ../deps/llhttp/llhttp.c

NGHTTP2_LIBS = $(shell pkg-config --libs libnghttp2 2>/dev/null || echo -lnghttp2)

fuzz_h2_session: fuzz_h2_session.c harness_common.c $(H2_SRC) $(STUBS_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(PHP_CFLAGS) \
	      -DHAVE_LLHTTP=1 -DHAVE_PHP_H=1 -DHAVE_HTTP2=1 \
	      -o $@ $^ $(PHP_LDFLAGS) $(NGHTTP2_LIBS)

# WebSocket frame-ingress fuzzer — feeds mutated bytes through
# ws_session_feed (our wslay integration). Built WITHOUT permessage-deflate
# (no config.h → HAVE_HTTP_COMPRESSION undefined) so the core recv path is
# covered without a zlib dependency. PHP embed for emalloc / zend_string.
WS_SRC = \
    ../src/websocket/ws_session.c \
    ../deps/wslay/lib/wslay_event.c \
    ../deps/wslay/lib/wslay_frame.c \
    ../deps/wslay/lib/wslay_net.c \
    ../deps/wslay/lib/wslay_queue.c \
    ../deps/wslay/lib/wslay_stack.c

WS_INCLUDES = -I../include/websocket -I../deps/wslay/includes

fuzz_ws_frame: fuzz_ws_frame.c harness_common.c $(WS_SRC) $(STUBS_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(WS_INCLUDES) $(PHP_CFLAGS) \
	      -DHAVE_PHP_H=1 -DHAVE_ARPA_INET_H=1 -DHAVE_NETINET_IN_H=1 \
	      -o $@ $^ $(PHP_LDFLAGS)

clean:
	rm -f fuzz_multipart fuzz_http1_parser fuzz_h2_session fuzz_ws_frame *.o
