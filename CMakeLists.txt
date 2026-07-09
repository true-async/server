cmake_minimum_required(VERSION 3.22)
# C++ is needed for the moodycamel-backed inter-thread queue (issue #81,
# src/core/thread_queue.cc); everything else is C.
project(php_http_server C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PHP source root (used for include dirs and, on Windows, the import lib).
set(PHP_INCLUDE_DIR "E:/php/php-src" CACHE PATH "PHP source directory")

# Windows: locate the PHP import library automatically.
if(WIN32)
    set(_php_inc_extra "")
    if(EXISTS "${PHP_INCLUDE_DIR}/x64/Debug_TS/php8ts_debug.lib")
        set(PHP_LIBRARY "${PHP_INCLUDE_DIR}/x64/Debug_TS/php8ts_debug.lib")
        set(_php_build_dir "${PHP_INCLUDE_DIR}/x64/Debug_TS")
        set(_zend_debug 1)
    elseif(EXISTS "${PHP_INCLUDE_DIR}/x64/Release_TS/php8ts.lib")
        set(PHP_LIBRARY "${PHP_INCLUDE_DIR}/x64/Release_TS/php8ts.lib")
        set(_php_build_dir "${PHP_INCLUDE_DIR}/x64/Release_TS")
        set(_zend_debug 0)
    else()
        message(FATAL_ERROR
            "PHP import lib not found under ${PHP_INCLUDE_DIR}/x64/. "
            "Set -DPHP_INCLUDE_DIR=<php-src-root>.")
    endif()
    # The build dir contains the generated config.w32.h (copied from main/).
    # Do NOT add win32/ as a search dir: it contains time.h / param.h that
    # shadow the CRT system headers when searched via angle-bracket includes.
    # PHP's own headers reference win32/* via "win32/foo.h" relative to root.
    set(_php_inc_extra "${_php_build_dir}")
    message(STATUS "PHP lib: ${PHP_LIBRARY} (ZEND_DEBUG=${_zend_debug})")
endif()

# Source files - Core
set(CORE_SOURCES
    src/http_server.c
    src/http_server_class.c
    src/http_server_config.c
    src/http_request.c
    src/http_response.c
    src/grpc/grpc.c
    src/grpc/grpc_call.c
    src/http_sse.c
    src/uploaded_file.c
    src/http_mime.c
    src/http_date.c
    src/http_etag.c
    src/http_range.c
    src/http_rfc5987.c
    src/http_precompressed.c
    src/http_param_parse.c
    src/send_file.c
    src/http_send_file.c
    src/http_send_file_options.c
    src/http_body_stream.c
    src/http_response_server_api.c
    src/compression/http_compression_negotiate.c
)

# Source files - Core subsystem
set(CORE_SUBSYSTEM_SOURCES
    src/core/http_connection.c
    src/core/conn_arena.c
    src/core/body_pool.c
    src/core/http_protocol_handlers.c
    src/core/http_protocol_strategy.c
    src/core/http_known_strings.c
    src/core/async_plain_event.c
    # Inter-thread message queue (issue #81): C++ moodycamel wrapper + C reactor glue.
    src/core/thread_queue.cc
    src/core/thread_mailbox.c
    # Reactor thread pool (issue #80): pure-C transport reactors on the ThreadPool.
    src/core/reactor_pool.c
    src/core/reactor_pool_test_hooks.c
    # Flat response marshalling type (issue #80, D3): worker -> reactor response.
    src/core/response_wire.c
    # Worker-side request dispatch (issue #80, B1b/D7): request ptr -> handler -> response.
    src/core/worker_dispatch.c
    # Worker inbox (issue #80, B2): per-worker request mailbox + dispatch drain.
    src/core/worker_inbox.c
    # Worker registry (issue #80, B3): atomic table of per-worker inboxes.
    src/core/worker_registry.c
)

# TLS sources — only compiled when OpenSSL is present. Both files
# unconditionally include OpenSSL headers and must not appear in the
# source list for no-TLS builds.
set(TLS_SOURCES "")

# Source files - Static handler
set(STATIC_SOURCES
    src/static/static_handler_class.c
    src/static/http_static_path.c
    src/static/http_static_cache.c
    src/static/http_static.c
    src/static/http_static_safety.c
)

# OpenSSL detection (auto) — mirrors config.m4. Uses the system OpenSSL;
# on macOS CMake picks up Homebrew automatically when OPENSSL_ROOT_DIR
# is set or pkg-config is on PATH. On Windows, set OPENSSL_ROOT_DIR to
# the PHP SDK deps path (e.g. ...\php-sdk\deps).
find_package(OpenSSL 3.0)
if(OpenSSL_FOUND)
    add_compile_definitions(HAVE_OPENSSL=1)
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
    check_symbol_exists(BIO_get_ktls_send "openssl/bio.h" HAVE_KTLS)
    if(HAVE_KTLS)
        add_compile_definitions(HAVE_KTLS=1)
    endif()
    set(TLS_SOURCES
        src/core/http_connection_tls.c
        src/core/tls_layer.c
    )
    message(STATUS "  OpenSSL: ${OPENSSL_VERSION} (${OPENSSL_LIBRARIES})")
else()
    message(STATUS "  OpenSSL: not found — TLS support disabled")
endif()

# HTTP/2 detection (mirrors config.m4). Requires nghttp2 >= 1.57 (rapid-reset
# CVE-2023-44487 mitigation). Fail-soft: without nghttp2, HAVE_HTTP2 is not
# defined and the HTTP/2 sources are excluded; http_connection.c provides a
# static no-op stub for http2_conn_notify_emit so the build links cleanly.
set(HTTP2_SOURCES "")
find_package(PkgConfig QUIET)
set(_http2_ok FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(NGHTTP2 QUIET libnghttp2>=1.57)
    if(NGHTTP2_FOUND)
        set(_http2_ok TRUE)
        set(_nghttp2_ver "${NGHTTP2_VERSION}")
    else()
        message(STATUS "  HTTP/2: disabled (libnghttp2 >= 1.57 not found via pkg-config)")
    endif()
endif()
if(NOT _http2_ok)
    find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
    find_library(NGHTTP2_LIB NAMES nghttp2)
    if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIB)
        # Version check via header
        if(EXISTS "${NGHTTP2_INCLUDE_DIR}/nghttp2/nghttp2ver.h")
            file(STRINGS "${NGHTTP2_INCLUDE_DIR}/nghttp2/nghttp2ver.h" _l
                REGEX "NGHTTP2_VERSION \"")
            string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _nghttp2_ver "${_l}")
        else()
            set(_nghttp2_ver "0.0.0")
        endif()
        if(_nghttp2_ver VERSION_GREATER_EQUAL "1.57")
            set(_http2_ok TRUE)
        else()
            message(STATUS "  HTTP/2: disabled (nghttp2 ${_nghttp2_ver} < 1.57)")
        endif()
    else()
        message(STATUS "  HTTP/2: disabled (nghttp2 not found; set CMAKE_PREFIX_PATH)")
    endif()
endif()
if(_http2_ok)
    add_compile_definitions(HAVE_HTTP2=1)
    set(HTTP2_SOURCES
        src/http2/http2_strategy.c
        src/http2/http2_session.c
        src/http2/http2_stream.c
        src/http2/http2_static_response.c
    )
    message(STATUS "  HTTP/2: nghttp2 ${_nghttp2_ver}")
endif()

# HTTP/3 detection (optional; mirrors config.m4). Fail-soft: if any
# prerequisite is missing, disable HAVE_HTTP_SERVER_HTTP3 and keep building.
# Version floors per docs/PLAN_HTTP3.md §2.
option(ENABLE_HTTP3 "Enable HTTP/3 support (requires ngtcp2, nghttp3, OpenSSL >= 3.5)" OFF)
if(ENABLE_HTTP3)
    find_package(PkgConfig QUIET)
    set(_http3_ok TRUE)
    if(NOT OpenSSL_FOUND OR OPENSSL_VERSION VERSION_LESS "3.5.0")
        message(WARNING "HTTP/3 disabled: OpenSSL >= 3.5 required (found ${OPENSSL_VERSION}).")
        set(_http3_ok FALSE)
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(NGTCP2 QUIET libngtcp2>=1.6)
        pkg_check_modules(NGTCP2_OSSL QUIET libngtcp2_crypto_ossl)
        pkg_check_modules(NGHTTP3 QUIET libnghttp3>=1.4)
        if(NOT NGTCP2_FOUND)
            message(WARNING "HTTP/3 disabled: libngtcp2 >= 1.6 not found.")
            set(_http3_ok FALSE)
        endif()
        if(NOT NGTCP2_OSSL_FOUND)
            message(WARNING "HTTP/3 disabled: libngtcp2_crypto_ossl not found.")
            set(_http3_ok FALSE)
        endif()
        if(NOT NGHTTP3_FOUND)
            message(WARNING "HTTP/3 disabled: libnghttp3 >= 1.4 not found.")
            set(_http3_ok FALSE)
        endif()
        set(_ngtcp2_ver  "${NGTCP2_VERSION}")
        set(_nghttp3_ver "${NGHTTP3_VERSION}")
    elseif(_http3_ok)
        # No pkg-config (typical on Windows): probe via find_library + find_path.
        find_path(NGTCP2_INCLUDE_DIR  ngtcp2/ngtcp2.h)
        find_path(NGHTTP3_INCLUDE_DIR nghttp3/nghttp3.h)
        find_library(NGTCP2_LIB      NAMES ngtcp2)
        find_library(NGTCP2_OSSL_LIB NAMES ngtcp2_crypto_ossl)
        find_library(NGHTTP3_LIB     NAMES nghttp3)
        if(NOT NGTCP2_INCLUDE_DIR OR NOT NGTCP2_LIB)
            message(WARNING "HTTP/3 disabled: ngtcp2 not found (set CMAKE_PREFIX_PATH).")
            set(_http3_ok FALSE)
        endif()
        if(NOT NGTCP2_OSSL_LIB)
            message(WARNING "HTTP/3 disabled: ngtcp2_crypto_ossl not found.")
            set(_http3_ok FALSE)
        endif()
        if(NOT NGHTTP3_INCLUDE_DIR OR NOT NGHTTP3_LIB)
            message(WARNING "HTTP/3 disabled: nghttp3 not found (set CMAKE_PREFIX_PATH).")
            set(_http3_ok FALSE)
        endif()
        if(_http3_ok)
            file(STRINGS "${NGTCP2_INCLUDE_DIR}/ngtcp2/version.h"  _l REGEX "NGTCP2_VERSION \"")
            string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _ngtcp2_ver "${_l}")
            file(STRINGS "${NGHTTP3_INCLUDE_DIR}/nghttp3/version.h" _l REGEX "NGHTTP3_VERSION \"")
            string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _nghttp3_ver "${_l}")
            if(_ngtcp2_ver VERSION_LESS "1.6")
                message(WARNING "HTTP/3 disabled: ngtcp2 ${_ngtcp2_ver} < 1.6.")
                set(_http3_ok FALSE)
            endif()
            if(_nghttp3_ver VERSION_LESS "1.4")
                message(WARNING "HTTP/3 disabled: nghttp3 ${_nghttp3_ver} < 1.4.")
                set(_http3_ok FALSE)
            endif()
        endif()
    endif()
    if(_http3_ok)
        add_compile_definitions(HAVE_HTTP_SERVER_HTTP3=1 HAVE_NGTCP2=1 HAVE_NGHTTP3=1
            NGTCP2_STATICLIB NGHTTP3_STATICLIB)
        set(HTTP3_SOURCES
            src/http3/http3_listener.c
            src/http3/http3_connection.c
            src/http3/http3_dispatch.c
            src/http3/http3_callbacks.c
            src/http3/http3_io.c
            src/http3/http3_packet.c
            src/http3/http3_steer.c
            src/http3/http3_stream.c
            src/http3/http3_stream_pool.c
            src/http3/http3_static_response.c
        )
        message(STATUS "  HTTP/3: ngtcp2 ${_ngtcp2_ver}, nghttp3 ${_nghttp3_ver}")
    else()
        set(HTTP3_SOURCES "")
        message(STATUS "  HTTP/3: disabled (prerequisites missing)")
    endif()
endif()

# HTTP body compression (issue #8). Mirrors config.m4: prefer zlib-ng
# (faster API-compatible drop-in for zlib), fall back to system zlib.
# Fail-soft: if neither is found, HAVE_HTTP_COMPRESSION stays undefined
# and the compression sources are dropped from the build.
option(ENABLE_HTTP_COMPRESSION "Enable HTTP body compression (zlib-ng preferred, zlib fallback)" ON)
option(ENABLE_BROTLI            "Enable Brotli compression backend (issue #9)" ON)
option(ENABLE_ZSTD              "Enable zstd compression backend (issue #9)"   ON)
set(COMPRESSION_SOURCES "")
if(ENABLE_HTTP_COMPRESSION)
    find_package(PkgConfig QUIET)
    set(_compression_ok FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZLIB_NG QUIET zlib-ng)
        if(ZLIB_NG_FOUND)
            add_compile_definitions(HAVE_ZLIB_NG=1 HAVE_HTTP_COMPRESSION=1)
            message(STATUS "  Compression: zlib-ng ${ZLIB_NG_VERSION}")
            set(_compression_ok TRUE)
        else()
            pkg_check_modules(ZLIB_PC QUIET zlib)
            if(ZLIB_PC_FOUND)
                add_compile_definitions(HAVE_HTTP_COMPRESSION=1)
                message(STATUS "  Compression: zlib ${ZLIB_PC_VERSION} (zlib-ng not found)")
                set(_compression_ok TRUE)
            endif()
        endif()
    endif()
    if(NOT _compression_ok)
        find_package(ZLIB QUIET)
        if(ZLIB_FOUND)
            add_compile_definitions(HAVE_HTTP_COMPRESSION=1)
            message(STATUS "  Compression: zlib ${ZLIB_VERSION_STRING} (system)")
            set(_compression_ok TRUE)
        endif()
    endif()
    if(_compression_ok)
        set(COMPRESSION_SOURCES
            src/compression/http_compression.c
            src/compression/http_compression_gzip.c
            src/compression/http_compression_defaults.c
            src/compression/http_compression_negotiate.c
            src/compression/http_compression_response.c
            src/compression/http_compression_request.c
            src/compression/http_compression_pool.c
        )
        # Brotli + zstd plug into the existing http_encoder vtable —
        # gated by HAVE_HTTP_COMPRESSION (no point without the base layer).
        if(ENABLE_BROTLI AND PkgConfig_FOUND)
            pkg_check_modules(BROTLI QUIET libbrotlienc libbrotlidec)
            if(BROTLI_FOUND)
                add_compile_definitions(HAVE_HTTP_BROTLI=1)
                list(APPEND COMPRESSION_SOURCES src/compression/http_compression_brotli.c)
                message(STATUS "  Compression: brotli ${BROTLI_VERSION}")
            else()
                message(STATUS "  Compression: brotli — not found, skipping")
            endif()
        endif()
        if(ENABLE_ZSTD AND PkgConfig_FOUND)
            pkg_check_modules(ZSTD QUIET libzstd)
            if(ZSTD_FOUND)
                add_compile_definitions(HAVE_HTTP_ZSTD=1)
                list(APPEND COMPRESSION_SOURCES src/compression/http_compression_zstd.c)
                message(STATUS "  Compression: zstd ${ZSTD_VERSION}")
            else()
                message(STATUS "  Compression: zstd — not found, skipping")
            endif()
        endif()
    else()
        message(STATUS "  Compression: disabled (no zlib-ng or zlib found)")
    endif()
endif()

# Source files - HTTP/1.1 parser
set(HTTP1_SOURCES
    src/http1/http_parser.c
    src/http1/http1_stream.c
    src/http1/http1_sendfile.c
    src/http1/http1_format.c
)

# Source files - Multipart format handling
set(FORMAT_SOURCES
    src/formats/multipart_parser.c
    src/formats/multipart_processor.c
)

# Source files - Logging subsystem (PLAN_LOG.md)
set(LOG_SOURCES
    src/log/http_log.c
    src/log/trace_context.c
)

# Source files - Dependencies (llhttp)
set(LLHTTP_SOURCES
    deps/llhttp/api.c
    deps/llhttp/http.c
    deps/llhttp/llhttp.c
)

# WebSocket (RFC 6455) — bundled wslay frame parser + our strategy,
# session, handshake, dispatch and PHP object layer. Mirrors config.m4:
# wslay is always bundled (never system), gated by ENABLE_WEBSOCKET.
# Defines are GLOBAL (add_compile_definitions) so every TU sees a
# consistent HAVE_HTTP_SERVER_WEBSOCKET — per-target risks ODR/inline skew.
option(ENABLE_WEBSOCKET "Enable WebSocket (RFC 6455) support (bundled wslay)" ON)
if(ENABLE_WEBSOCKET)
    add_compile_definitions(HAVE_WSLAY=1 HAVE_HTTP_SERVER_WEBSOCKET=1)

    # wslay's header/byteswap selection keys off these (config.m4 probes
    # them for the autotools build); probe them here too so wslay compiles
    # cleanly under CMake without implicit htons/htonl declarations.
    include(CheckIncludeFile)
    check_include_file(netinet/in.h HAVE_NETINET_IN_H)
    check_include_file(arpa/inet.h  HAVE_ARPA_INET_H)
    check_include_file(winsock2.h   HAVE_WINSOCK2_H)
    if(HAVE_NETINET_IN_H)
        add_compile_definitions(HAVE_NETINET_IN_H=1)
    endif()
    if(HAVE_ARPA_INET_H)
        add_compile_definitions(HAVE_ARPA_INET_H=1)
    endif()
    if(HAVE_WINSOCK2_H)
        add_compile_definitions(HAVE_WINSOCK2_H=1)
    endif()

    set(WEBSOCKET_SOURCES
        deps/wslay/lib/wslay_event.c
        deps/wslay/lib/wslay_frame.c
        deps/wslay/lib/wslay_net.c
        deps/wslay/lib/wslay_queue.c
        deps/wslay/lib/wslay_stack.c
        src/websocket/websocket_strategy.c
        src/websocket/ws_session.c
        src/websocket/ws_handshake.c
        src/websocket/ws_dispatch.c
        src/websocket/php_websocket.c
    )
    message(STATUS "  WebSocket: enabled (bundled wslay)")
else()
    set(WEBSOCKET_SOURCES "")
    message(STATUS "  WebSocket: disabled")
endif()

# All sources combined
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${CORE_SUBSYSTEM_SOURCES}
    ${TLS_SOURCES}
    ${HTTP2_SOURCES}
    ${HTTP1_SOURCES}
    ${FORMAT_SOURCES}
    ${LOG_SOURCES}
    ${LLHTTP_SOURCES}
    ${STATIC_SOURCES}
    ${COMPRESSION_SOURCES}
    ${HTTP3_SOURCES}
    ${WEBSOCKET_SOURCES}
)

# Extension DLL / shared library
add_library(php_true_async_server SHARED ${ALL_SOURCES})

set_target_properties(php_true_async_server PROPERTIES
    OUTPUT_NAME "php_true_async_server"
    PREFIX ""
)

# Project include dirs (our own code)
target_include_directories(php_true_async_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/formats
    ${CMAKE_CURRENT_SOURCE_DIR}/include/http1
    ${CMAKE_CURRENT_SOURCE_DIR}/include/static
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/llhttp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/concurrentqueue
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/wslay/includes
    ${CMAKE_CURRENT_SOURCE_DIR}/include/websocket
)

# PHP headers — added as SYSTEM so the compiler suppresses their warnings.
target_include_directories(php_true_async_server SYSTEM PRIVATE
    ${PHP_INCLUDE_DIR}
    ${PHP_INCLUDE_DIR}/main
    ${PHP_INCLUDE_DIR}/Zend
    ${PHP_INCLUDE_DIR}/TSRM
    ${_php_inc_extra}
)

target_compile_definitions(php_true_async_server PRIVATE
    HTTP_SERVER_EXPORTS
    HAVE_LLHTTP=1
)

if(WIN32)
    target_compile_definitions(php_true_async_server PRIVATE
        ZEND_WIN32 PHP_WIN32 _WINDOWS WINDOWS=1 WIN32
        ZTS=1 ZEND_DEBUG=${_zend_debug}
        _CRT_SECURE_NO_WARNINGS FD_SETSIZE=256
    )
    target_link_libraries(php_true_async_server PRIVATE
        ${PHP_LIBRARY} ws2_32 Mswsock
    )
    # /external:W0 suppresses warnings from SYSTEM includes on MSVC.
    target_compile_options(php_true_async_server PRIVATE
        /external:W0
    )
endif()

if(OpenSSL_FOUND)
    target_compile_definitions(php_true_async_server PRIVATE HAVE_OPENSSL=1)
    target_link_libraries(php_true_async_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(_http2_ok)
    if(PkgConfig_FOUND AND NGHTTP2_FOUND)
        target_include_directories(php_true_async_server SYSTEM PRIVATE ${NGHTTP2_INCLUDE_DIRS})
        target_link_libraries(php_true_async_server PRIVATE ${NGHTTP2_LIBRARIES})
    else()
        target_include_directories(php_true_async_server SYSTEM PRIVATE ${NGHTTP2_INCLUDE_DIR})
        target_link_libraries(php_true_async_server PRIVATE ${NGHTTP2_LIB})
    endif()
endif()

if(_http3_ok)
    if(PkgConfig_FOUND)
        target_include_directories(php_true_async_server SYSTEM PRIVATE
            ${NGTCP2_INCLUDE_DIRS} ${NGTCP2_OSSL_INCLUDE_DIRS} ${NGHTTP3_INCLUDE_DIRS})
        target_link_libraries(php_true_async_server PRIVATE
            ${NGTCP2_LIBRARIES} ${NGTCP2_OSSL_LIBRARIES} ${NGHTTP3_LIBRARIES})
    else()
        target_include_directories(php_true_async_server SYSTEM PRIVATE
            ${NGTCP2_INCLUDE_DIR} ${NGHTTP3_INCLUDE_DIR})
        target_link_libraries(php_true_async_server PRIVATE
            ${NGTCP2_LIB} ${NGTCP2_OSSL_LIB} ${NGHTTP3_LIB})
    endif()
endif()

# Diagnostic flags — our code only (SYSTEM headers are already silenced above).
include(CheckCCompilerFlag)
if(MSVC)
    target_compile_options(php_true_async_server PRIVATE
        /W4 /WX /GS /sdl /guard:cf
        /wd4100  # unreferenced formal parameter — return_value in void ZEND_METHOD + llhttp callbacks
        /wd4127  # constant conditional — PHP do{}while(0) macros
        /wd4456  # declaration hides local — nested ZEND_HASH_FOREACH _count/_size/_z vars
        /wd4701 /wd4703  # uninitialized false-positives in Z_PARAM_* macros
    )
else()
    foreach(_flag -Wall -Wextra -Werror -fstack-protector-strong
                  -Wformat=2 -Wstrict-prototypes -Wsign-compare)
        string(MAKE_C_IDENTIFIER "HAVE_CFLAG${_flag}" _flag_var)
        check_c_compiler_flag(${_flag} ${_flag_var})
        if(${_flag_var})
            target_compile_options(php_true_async_server PRIVATE ${_flag})
        endif()
    endforeach()
endif()

# llhttp is a generated third-party parser — it has intentional
# int→uint8_t narrowing and function/data pointer casts. Relax warnings
# only for those source files so our own code stays under /WX.
if(MSVC)
    foreach(_f ${LLHTTP_SOURCES})
        set_source_files_properties(${_f} PROPERTIES
            COMPILE_FLAGS "/wd4244 /wd4152")
    endforeach()
endif()

message(STATUS "PHP HTTP Server")
message(STATUS "  PHP include: ${PHP_INCLUDE_DIR}")
message(STATUS "  Sources: ${ALL_SOURCES}")
