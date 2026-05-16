# CMake configuration for unit tests
# Copyright (c) TrueAsync

# Source files to test
set(HTTP1_PARSER_SRC
    ${CMAKE_SOURCE_DIR}/../src/http1/http_parser.c
    # Streaming body queue (issue #26) — called directly from http_parser.c.
    ${CMAKE_SOURCE_DIR}/../src/http_body_stream.c
    # Body slab pool — used by http_parser.c for body allocation.
    ${CMAKE_SOURCE_DIR}/../src/core/body_pool.c
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_parser.c
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_processor.c
    # RFC 5987 / header-param parse — referenced by multipart_processor.c.
    ${CMAKE_SOURCE_DIR}/../src/http_rfc5987.c
    ${CMAKE_SOURCE_DIR}/../src/http_param_parse.c
    ${CMAKE_SOURCE_DIR}/unit/common/multipart_stubs.c
)

# llhttp library (vendored - disable warnings)
add_library(llhttp_vendor STATIC
    ${CMAKE_SOURCE_DIR}/../deps/llhttp/llhttp.c
    ${CMAKE_SOURCE_DIR}/../deps/llhttp/api.c
    ${CMAKE_SOURCE_DIR}/../deps/llhttp/http.c
)

# Disable warnings for vendored llhttp code
if(MSVC)
    target_compile_options(llhttp_vendor PRIVATE /W0)
else()
    target_compile_options(llhttp_vendor PRIVATE -w)
endif()

target_include_directories(llhttp_vendor PUBLIC
    ${CMAKE_SOURCE_DIR}/../deps/llhttp
)

# HTTP/1.1 Parser Tests
add_executable(test_http1_parser
    http1/test_parser.c
    ${HTTP1_PARSER_SRC}
)

target_include_directories(test_http1_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/formats
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/core
)

target_link_libraries(test_http1_parser
    test_common
    llhttp_vendor
    ${CMOCKA_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

if(UNIX)
    target_link_libraries(test_http1_parser
        m
        pthread
        rt
    )
endif()

# Add test to CTest
add_test(NAME HTTP1Parser COMMAND test_http1_parser)

if(WIN32)
    set_tests_properties(HTTP1Parser PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
elseif(UNIX AND NOT APPLE)
    set_tests_properties(HTTP1Parser PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endif()

# HTTP/1.1 Parser Edge Cases Tests
add_executable(test_http1_parser_edge_cases
    http1/test_parser_edge_cases.c
    ${HTTP1_PARSER_SRC}
)

target_include_directories(test_http1_parser_edge_cases PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/formats
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/core
)

target_link_libraries(test_http1_parser_edge_cases
    test_common
    llhttp_vendor
    ${CMOCKA_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

if(UNIX)
    target_link_libraries(test_http1_parser_edge_cases
        m
        pthread
        rt
    )
endif()

# Add edge cases test to CTest
add_test(NAME HTTP1ParserEdgeCases COMMAND test_http1_parser_edge_cases)

if(WIN32)
    set_tests_properties(HTTP1ParserEdgeCases PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
elseif(UNIX AND NOT APPLE)
    set_tests_properties(HTTP1ParserEdgeCases PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endif()

# Multipart Parser Source
set(MULTIPART_PARSER_SRC
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_parser.c
)

# Multipart Parser Tests (standalone, no PHP dependency)
add_executable(test_multipart_parser
    http1/test_multipart_parser.c
    ${MULTIPART_PARSER_SRC}
)

target_include_directories(test_multipart_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/formats
)

target_link_libraries(test_multipart_parser
    test_common
    ${CMOCKA_LIBRARIES}
)

if(UNIX)
    target_link_libraries(test_multipart_parser
        m
        pthread
    )
endif()

# Add multipart parser test to CTest
add_test(NAME MultipartParser COMMAND test_multipart_parser)

if(WIN32)
    set_tests_properties(MultipartParser PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
endif()

# Multipart Processor Tests
add_executable(test_multipart_processor
    http1/test_multipart_processor.c
    ${MULTIPART_PARSER_SRC}
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_processor.c
    # multipart_processor.c references these via http_param_parse.h.
    ${CMAKE_SOURCE_DIR}/../src/http_rfc5987.c
    ${CMAKE_SOURCE_DIR}/../src/http_param_parse.c
    ${CMAKE_SOURCE_DIR}/unit/common/multipart_stubs.c
)

target_include_directories(test_multipart_processor PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/formats
    ${CMAKE_SOURCE_DIR}/../src
)


target_link_libraries(test_multipart_processor
    test_common
    ${CMOCKA_LIBRARIES}
)

if(UNIX)
    target_link_libraries(test_multipart_processor
        m
        pthread
    )
endif()

# Add multipart processor test to CTest
add_test(NAME MultipartProcessor COMMAND test_multipart_processor)

if(WIN32)
    set_tests_properties(MultipartProcessor PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
endif()

# HTTP/1.1 Parser Security Tests
add_executable(test_http1_parser_security
    http1/test_parser_security.c
    ${HTTP1_PARSER_SRC}
)

target_include_directories(test_http1_parser_security PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/formats
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/core
)

target_link_libraries(test_http1_parser_security
    test_common
    llhttp_vendor
    ${CMOCKA_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

if(UNIX)
    target_link_libraries(test_http1_parser_security
        m
        pthread
        rt
    )
endif()

# Add security test to CTest
add_test(NAME HTTP1ParserSecurity COMMAND test_http1_parser_security)

if(WIN32)
    set_tests_properties(HTTP1ParserSecurity PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
elseif(UNIX AND NOT APPLE)
    set_tests_properties(HTTP1ParserSecurity PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endif()

# TLS session state-machine tests — exercise tls_session_t end-to-end
# by wiring our server-side session against a raw client SSL. Built only
# when OpenSSL is available; plain-TLS builds silently skip it.
find_package(OpenSSL 3.0 QUIET)
if(OpenSSL_FOUND)
    add_executable(test_tls_session
        core/test_tls_session.c
        ${CMAKE_SOURCE_DIR}/../src/core/tls_layer.c
    )

    target_compile_definitions(test_tls_session PRIVATE HAVE_OPENSSL=1)

    target_include_directories(test_tls_session PRIVATE
        ${CMAKE_SOURCE_DIR}/../include
        ${CMAKE_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/../src/core
    )

    target_link_libraries(test_tls_session
        test_common
        OpenSSL::SSL
        OpenSSL::Crypto
        ${CMOCKA_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )

    if(UNIX)
        target_link_libraries(test_tls_session m pthread rt)
    endif()

    add_test(NAME TLSSession COMMAND test_tls_session)

    if(UNIX AND NOT APPLE)
        set_tests_properties(TLSSession PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
        )
    endif()
endif()

# HTTP/2 strategy skeleton test (PLAN_HTTP2 Step 1). Only built when
# libnghttp2 >= 1.57 is present on the system; otherwise silently
# skipped — matches the --enable-http2 optionality of the main build.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(NGHTTP2 QUIET libnghttp2>=1.57)
endif()
if(NGHTTP2_FOUND)
    add_executable(test_http2_strategy
        http2/test_http2_strategy.c
        ${CMAKE_SOURCE_DIR}/../src/core/http_protocol_strategy.c
        ${CMAKE_SOURCE_DIR}/../src/http2/http2_strategy.c
        ${CMAKE_SOURCE_DIR}/../src/http2/http2_session.c
        ${CMAKE_SOURCE_DIR}/../src/http2/http2_stream.c
        # HTTP/1 parser is referenced by http_protocol_strategy.c (http1_feed);
        # link the same source set the HTTP/1 unit tests use.
        ${HTTP1_PARSER_SRC}
    )

    target_compile_definitions(test_http2_strategy PRIVATE HAVE_HTTP2=1)

    target_include_directories(test_http2_strategy PRIVATE
        ${CMAKE_SOURCE_DIR}/../include
        ${CMAKE_SOURCE_DIR}/../include/formats
        ${CMAKE_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/../src/core
        ${NGHTTP2_INCLUDE_DIRS}
    )

    target_link_libraries(test_http2_strategy
        test_common
        llhttp_vendor
        ${NGHTTP2_LIBRARIES}
        ${CMOCKA_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )

    if(UNIX)
        target_link_libraries(test_http2_strategy m pthread rt)
    endif()

    add_test(NAME HTTP2Strategy COMMAND test_http2_strategy)

    if(UNIX AND NOT APPLE)
        set_tests_properties(HTTP2Strategy PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
        )
    endif()

    # HTTP/2 session lifecycle + SETTINGS exchange (Step 2) +
    # HEADERS decode + stream dispatch (Step 3).
    add_executable(test_http2_session
        http2/test_http2_session.c
        ${CMAKE_SOURCE_DIR}/../src/http2/http2_session.c
        ${CMAKE_SOURCE_DIR}/../src/http2/http2_stream.c
        # http2_stream references http_request_destroy from the HTTP/1
        # parser, and that TU in turn pulls in smart_str / llhttp usage
        # — so link the same source set the HTTP/1 unit tests use.
        ${HTTP1_PARSER_SRC}
    )

    target_compile_definitions(test_http2_session PRIVATE HAVE_HTTP2=1)

    target_include_directories(test_http2_session PRIVATE
        ${CMAKE_SOURCE_DIR}/../include
        ${CMAKE_SOURCE_DIR}/../include/formats
        ${CMAKE_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/../src/core
        ${NGHTTP2_INCLUDE_DIRS}
    )

    target_link_libraries(test_http2_session
        test_common
        llhttp_vendor
        ${NGHTTP2_LIBRARIES}
        ${CMOCKA_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )

    if(UNIX)
        target_link_libraries(test_http2_session m pthread rt)
    endif()

    add_test(NAME HTTP2Session COMMAND test_http2_session)

    if(UNIX AND NOT APPLE)
        set_tests_properties(HTTP2Session PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
        )
    endif()
endif()

# Compression negotiation tests (issue #8). The negotiate TU is pure
# C with no Zend deps so the unit test compiles standalone — no PHP
# headers, no llhttp. Other compression sources (registry, gzip backend,
# defaults) require HAVE_HTTP_COMPRESSION at compile time; we set it
# directly here rather than re-running the pkg-config probe so the test
# build stays decoupled from the main feature flag.
add_executable(test_compression_negotiate
    compression/test_negotiate.c
    ${CMAKE_SOURCE_DIR}/../src/compression/http_compression_negotiate.c
    # Stub for http_compression_lookup — avoids pulling in gzip/brotli/zstd
    # backends (and their zlib/libbrotli/libzstd deps) into the unit test.
    compression/compression_stubs.c
)

target_compile_definitions(test_compression_negotiate PRIVATE HAVE_HTTP_COMPRESSION=1)

target_include_directories(test_compression_negotiate PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../src
)

target_link_libraries(test_compression_negotiate
    test_common
    ${CMOCKA_LIBRARIES}
)

if(UNIX)
    target_link_libraries(test_compression_negotiate m pthread)
endif()

add_test(NAME CompressionNegotiate COMMAND test_compression_negotiate)

if(WIN32)
    set_tests_properties(CompressionNegotiate PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
endif()

# Compression gzip-encoder round-trip tests. Pulls in real zlib(-ng)
# at link time and depends on the PHP runtime (the encoder uses
# emalloc/efree). pkg-config probe mirrors the main build's preference
# for zlib-ng with zlib fallback.
find_package(PkgConfig QUIET)
set(_compr_test_ok FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(ZLIB_NG_TEST QUIET zlib-ng)
    if(ZLIB_NG_TEST_FOUND)
        set(_compr_test_libs ${ZLIB_NG_TEST_LIBRARIES})
        set(_compr_test_incs ${ZLIB_NG_TEST_INCLUDE_DIRS})
        set(_compr_test_def  HAVE_ZLIB_NG=1)
        set(_compr_test_ok TRUE)
    else()
        pkg_check_modules(ZLIB_TEST QUIET zlib)
        if(ZLIB_TEST_FOUND)
            set(_compr_test_libs ${ZLIB_TEST_LIBRARIES})
            set(_compr_test_incs ${ZLIB_TEST_INCLUDE_DIRS})
            set(_compr_test_def  "")
            set(_compr_test_ok TRUE)
        endif()
    endif()
endif()
if(NOT _compr_test_ok)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        set(_compr_test_libs ZLIB::ZLIB)
        set(_compr_test_incs "")
        set(_compr_test_def  "")
        set(_compr_test_ok TRUE)
    endif()
endif()
if(_compr_test_ok)
    add_executable(test_compression_gzip
        compression/test_gzip_encoder.c
        ${CMAKE_SOURCE_DIR}/../src/compression/http_compression.c
        ${CMAKE_SOURCE_DIR}/../src/compression/http_compression_gzip.c
    )
    target_compile_definitions(test_compression_gzip PRIVATE
        HAVE_HTTP_COMPRESSION=1 ${_compr_test_def})
    target_include_directories(test_compression_gzip PRIVATE
        ${CMAKE_SOURCE_DIR}/../include
        ${CMAKE_SOURCE_DIR}/../src
        ${_compr_test_incs}
    )
    target_link_libraries(test_compression_gzip
        test_common
        ${CMOCKA_LIBRARIES}
        ${_compr_test_libs}
        ${CMAKE_DL_LIBS}
    )
    if(UNIX)
        target_link_libraries(test_compression_gzip m pthread rt)
    endif()
    add_test(NAME CompressionGzip COMMAND test_compression_gzip)
    if(UNIX AND NOT APPLE)
        set_tests_properties(CompressionGzip PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
        )
    endif()
endif()

# Static-handler decoder tests (issue #13 §13c.4). Exercises the three
# parsing surfaces — MIME extension lookup, If-None-Match / If-Modified-
# Since matching, and percent-decode + segment validation. Each decoder
# TU is compiled directly into the test binary; no llhttp / zlib needed.
# Guard with existence check: mime/etag sources are added in a later PR.
if(EXISTS "${CMAKE_SOURCE_DIR}/../src/static/http_static_mime.c" AND
   EXISTS "${CMAKE_SOURCE_DIR}/../src/static/http_static_etag.c")
add_executable(test_static_decoders
    static/test_static_decoders.c
    ${CMAKE_SOURCE_DIR}/../src/static/http_static_mime.c
    ${CMAKE_SOURCE_DIR}/../src/static/http_static_etag.c
    ${CMAKE_SOURCE_DIR}/../src/static/http_static_path.c
)

target_include_directories(test_static_decoders PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../src
)

target_link_libraries(test_static_decoders
    test_common
    ${CMOCKA_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

if(UNIX)
    target_link_libraries(test_static_decoders m pthread rt)
endif()

add_test(NAME StaticDecoders COMMAND test_static_decoders)

if(WIN32)
    set_tests_properties(StaticDecoders PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
elseif(UNIX AND NOT APPLE)
    set_tests_properties(StaticDecoders PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endif()
else()
    message(STATUS "Skipping test_static_decoders: http_static_mime.c / http_static_etag.c not yet present")
endif()

# Windows/MSVC portability tests (issue #15).
# Exercises win32_compat.h macro values, path-separator handling in
# http_static_resolved_under_root / verify_path_owner_chain, and
# the open/read/close/unlink compat aliases.  Runs on all platforms;
# Windows-specific assertions are guarded with #ifdef PHP_WIN32.
add_executable(test_win32_compat
    static/test_win32_compat.c
    ${CMAKE_SOURCE_DIR}/../src/static/http_static_safety.c
)

target_include_directories(test_win32_compat PRIVATE
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/static
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}           # tests/ → resolves "common/php_sapi_test.h"
)

target_link_libraries(test_win32_compat
    test_common
    ${CMOCKA_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

if(UNIX)
    target_link_libraries(test_win32_compat m pthread rt)
endif()

if(MSVC)
    target_compile_options(test_win32_compat PRIVATE /wd4701 /wd4703)
endif()

add_test(NAME Win32Compat COMMAND test_win32_compat)

if(WIN32)
    set_tests_properties(Win32Compat PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PHP_DLL_DIR};PATH=path_list_prepend:${CMOCKA_DLL_DIR}"
    )
elseif(UNIX AND NOT APPLE)
    set_tests_properties(Win32Compat PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${PHP_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
    )
endif()

# Install test binaries (optional)
# install(TARGETS test_http1_parser test_http1_parser_edge_cases test_multipart_parser DESTINATION bin/tests)
