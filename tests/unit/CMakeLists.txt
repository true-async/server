# CMake configuration for unit tests
# Copyright (c) TrueAsync

# Source files to test
set(HTTP1_PARSER_SRC
    ${CMAKE_SOURCE_DIR}/../src/http1/http_parser.c
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_parser.c
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_processor.c
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

# Set environment for test
if(UNIX AND NOT APPLE)
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

# Set environment for test
if(UNIX AND NOT APPLE)
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

# Multipart Processor Tests
add_executable(test_multipart_processor
    http1/test_multipart_processor.c
    ${MULTIPART_PARSER_SRC}
    ${CMAKE_SOURCE_DIR}/../src/formats/multipart_processor.c
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

# Set environment for test
if(UNIX AND NOT APPLE)
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

# Install test binaries (optional)
# install(TARGETS test_http1_parser test_http1_parser_edge_cases test_multipart_parser DESTINATION bin/tests)
