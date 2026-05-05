cmake_minimum_required(VERSION 3.15)
project(php_http_server C)

set(CMAKE_C_STANDARD 11)

# PHP paths - adjust if needed
set(PHP_INCLUDE_DIR "e:/php/php-src" CACHE PATH "PHP source directory")
set(PHP_MAIN_INCLUDE "${PHP_INCLUDE_DIR}/main")
set(PHP_ZEND_INCLUDE "${PHP_INCLUDE_DIR}/Zend")
set(PHP_TSRM_INCLUDE "${PHP_INCLUDE_DIR}/TSRM")

# Project include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/formats
    ${CMAKE_CURRENT_SOURCE_DIR}/include/http1
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/llhttp/include
    ${PHP_INCLUDE_DIR}
    ${PHP_MAIN_INCLUDE}
    ${PHP_ZEND_INCLUDE}
    ${PHP_TSRM_INCLUDE}
)

# Windows-specific defines for PHP
if(WIN32)
    add_compile_definitions(
        ZEND_WIN32
        PHP_WIN32
        ZTS=1
        ZEND_DEBUG=0
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Source files - Core
set(CORE_SOURCES
    src/http_server.c
    src/http_server_class.c
    src/http_server_config.c
    src/http_server_exceptions.c
    src/http_request.c
    src/http_response.c
    src/uploaded_file.c
)

# Source files - Core subsystem
set(CORE_SUBSYSTEM_SOURCES
    src/core/http_connection.c
    src/core/conn_arena.c
    src/core/http_protocol_handlers.c
    src/core/http_protocol_strategy.c
    src/core/tls_layer.c
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
    message(STATUS "  OpenSSL: ${OPENSSL_VERSION} (${OPENSSL_LIBRARIES})")
else()
    message(STATUS "  OpenSSL: not found — TLS support disabled")
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
    else()
        message(WARNING "HTTP/3 disabled: pkg-config not available.")
        set(_http3_ok FALSE)
    endif()
    if(_http3_ok)
        add_compile_definitions(HAVE_HTTP_SERVER_HTTP3=1 HAVE_NGTCP2=1 HAVE_NGHTTP3=1)
        message(STATUS "  HTTP/3: ngtcp2 ${NGTCP2_VERSION}, nghttp3 ${NGHTTP3_VERSION}")
    else()
        message(STATUS "  HTTP/3: disabled (prerequisites missing)")
    endif()
endif()

# HTTP body compression (issue #8). Mirrors config.m4: prefer zlib-ng
# (faster API-compatible drop-in for zlib), fall back to system zlib.
# Fail-soft: if neither is found, HAVE_HTTP_COMPRESSION stays undefined
# and the compression sources are dropped from the build.
option(ENABLE_HTTP_COMPRESSION "Enable HTTP body compression (zlib-ng preferred, zlib fallback)" ON)
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
        )
    else()
        message(STATUS "  Compression: disabled (no zlib-ng or zlib found)")
    endif()
endif()

# Source files - HTTP/1.1 parser
set(HTTP1_SOURCES
    src/http1/http_parser.c
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

# All sources combined
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${CORE_SUBSYSTEM_SOURCES}
    ${HTTP1_SOURCES}
    ${FORMAT_SOURCES}
    ${LOG_SOURCES}
    ${LLHTTP_SOURCES}
    ${COMPRESSION_SOURCES}
)

# Create library target (not for actual building, just for CodeQL analysis)
add_library(http_server SHARED ${ALL_SOURCES})
if(OpenSSL_FOUND)
    target_link_libraries(http_server OpenSSL::SSL OpenSSL::Crypto)
endif()

# Hardening + diagnostic flags. Mirrored from config.m4. Probed
# per-flag so MSVC and older compilers stay green; CodeQL and
# any contributor IDE pick up the same warning surface.
include(CheckCCompilerFlag)
if(NOT MSVC)
    # -Wshadow excluded — Zend macros legitimately shadow names; see config.m4.
    foreach(_flag -Wall -Wextra -fstack-protector-strong -Wformat=2 -Wstrict-prototypes -Wsign-compare)
        string(MAKE_C_IDENTIFIER "HAVE_CFLAG${_flag}" _flag_var)
        check_c_compiler_flag(${_flag} ${_flag_var})
        if(${_flag_var})
            target_compile_options(http_server PRIVATE ${_flag})
        endif()
    endforeach()
else()
    target_compile_options(http_server PRIVATE /W4 /GS /sdl /guard:cf)
endif()

# Test sources (optional, for comprehensive analysis)
set(TEST_SOURCES
    tests/common/php_sapi_test.c
    tests/unit/http1/test_parser.c
    tests/unit/http1/test_parser_edge_cases.c
    tests/unit/http1/test_parser_security.c
    tests/unit/http1/test_multipart_parser.c
    tests/unit/http1/test_multipart_processor.c
    tests/unit/common/multipart_stubs.c
)

# Optionally add tests as a separate target
add_library(http_server_tests STATIC ${TEST_SOURCES})
target_link_libraries(http_server_tests http_server)

# Print configuration info
message(STATUS "PHP HTTP Server - CodeQL Analysis Configuration")
message(STATUS "  Source files: ${ALL_SOURCES}")
message(STATUS "  PHP include: ${PHP_INCLUDE_DIR}")
message(STATUS "  Project root: ${CMAKE_CURRENT_SOURCE_DIR}")
