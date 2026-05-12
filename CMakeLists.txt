cmake_minimum_required(VERSION 3.22)
project(php_http_server C)

set(CMAKE_C_STANDARD 11)

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
    src/http_server_exceptions.c
    src/http_request.c
    src/http_response.c
    src/uploaded_file.c
    src/http_mime.c
    src/http_date.c
    src/http_etag.c
    src/http_conditional.c
    src/http_range.c
    src/fs_util.c
    src/http_rfc5987.c
    src/http_precompressed.c
    src/http_param_parse.c
    src/send_file.c
    src/http_send_file.c
    src/http_send_file_options.c
    src/compression/http_compression_negotiate.c
)

# Source files - Core subsystem
set(CORE_SUBSYSTEM_SOURCES
    src/core/http_connection.c
    src/core/http_connection_tls.c
    src/core/conn_arena.c
    src/core/body_pool.c
    src/core/http_protocol_handlers.c
    src/core/http_protocol_strategy.c
    src/core/tls_layer.c
    src/core/http_known_strings.c
)

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
    ${STATIC_SOURCES}
    ${COMPRESSION_SOURCES}
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
