# CMake configuration for HTTP Server tests
# Copyright (c) TrueAsync

cmake_minimum_required(VERSION 3.22)
# CXX is needed for the thread_queue test, which links the moodycamel-backed
# C++ TU (issue #81); every other test is pure C.
project(http_server_tests C CXX)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Compiler flags
if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
endif()
if(NOT MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")
endif()

# Find PHP
if(WIN32)
    # php-config does not exist on Windows. Locate PHP from the source tree.
    set(PHP_SRC_DIR "E:/php/php-src" CACHE PATH "PHP source root directory")

    if(EXISTS "${PHP_SRC_DIR}/x64/Debug_TS/php8ts_debug.lib")
        set(PHP_PREFIX "${PHP_SRC_DIR}/x64/Debug_TS")
        set(PHP_LIBRARY "${PHP_SRC_DIR}/x64/Debug_TS/php8ts_debug.lib")
        set(_zend_debug 1)
        message(STATUS "PHP: debug TS build (${PHP_PREFIX})")
    elseif(EXISTS "${PHP_SRC_DIR}/x64/Release_TS/php8ts.lib")
        set(PHP_PREFIX "${PHP_SRC_DIR}/x64/Release_TS")
        set(PHP_LIBRARY "${PHP_SRC_DIR}/x64/Release_TS/php8ts.lib")
        set(_zend_debug 0)
        message(STATUS "PHP: release TS build (${PHP_PREFIX})")
    else()
        message(FATAL_ERROR
            "Cannot find PHP library under ${PHP_SRC_DIR}/x64/. "
            "Set -DPHP_SRC_DIR=<path> to the PHP source root.")
    endif()

    # Do NOT add win32/ as a search dir: it contains time.h / unistd.h /
    # param.h that shadow CRT system headers when searched via angle-bracket
    # includes. PHP's own headers reference win32/* via "win32/foo.h"
    # relative to the root — that still resolves correctly through PHP_SRC_DIR.
    set(PHP_INCLUDES_LIST
        "${PHP_SRC_DIR}"
        "${PHP_SRC_DIR}/main"
        "${PHP_SRC_DIR}/Zend"
        "${PHP_SRC_DIR}/TSRM"
        "${PHP_PREFIX}"
    )
else()
    find_program(PHP_CONFIG php-config)
    if(NOT PHP_CONFIG)
        message(FATAL_ERROR "php-config not found. Please install PHP development files.")
    endif()

    execute_process(
        COMMAND ${PHP_CONFIG} --includes
        OUTPUT_VARIABLE PHP_INCLUDES
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REPLACE " -I" ";-I" PHP_INCLUDES_WITH_PREFIX ${PHP_INCLUDES})
    string(REPLACE "-I" "" PHP_INCLUDES_LIST "${PHP_INCLUDES_WITH_PREFIX}")
    message(STATUS "PHP includes: ${PHP_INCLUDES_LIST}")

    execute_process(
        COMMAND ${PHP_CONFIG} --prefix
        OUTPUT_VARIABLE PHP_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    execute_process(
        COMMAND ${PHP_CONFIG} --libs
        OUTPUT_VARIABLE PHP_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# Find CMocka
if(WIN32)
    # Prefer vcpkg install alongside PHP source tree (default location).
    set(_vcpkg_prefix "${PHP_SRC_DIR}/vcpkg_installed/x64-windows")
    list(APPEND CMAKE_PREFIX_PATH "${_vcpkg_prefix}")

    find_package(cmocka CONFIG QUIET)
    if(cmocka_FOUND)
        set(CMOCKA_LIBRARIES cmocka::cmocka)
        set(CMOCKA_INCLUDE_DIRS "")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(CMOCKA REQUIRED cmocka)
        else()
            message(FATAL_ERROR
                "cmocka not found. Pass -DCMAKE_PREFIX_PATH=<vcpkg>/x64-windows "
                "or install cmocka via vcpkg.")
        endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CMOCKA REQUIRED cmocka)
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/..  # Project root
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/http1
    ${CMAKE_SOURCE_DIR}/../deps/llhttp
    ${CMAKE_SOURCE_DIR}  # tests/ for common/
    ${PHP_INCLUDES_LIST}
    ${CMOCKA_INCLUDE_DIRS}
)

# Common test infrastructure
add_library(test_common STATIC
    common/php_sapi_test.c
)

if(WIN32)
    target_compile_definitions(test_common PUBLIC
        ZEND_WIN32
        PHP_WIN32
        ZTS=1
        ZEND_DEBUG=${_zend_debug}
        _CRT_SECURE_NO_WARNINGS
        HAVE_LLHTTP=1
        HAVE_PHP_H=1
    )
else()
    target_compile_definitions(test_common PUBLIC
        _GNU_SOURCE
        HAVE_LLHTTP=1
        HAVE_PHP_H=1
    )
endif()

target_include_directories(test_common PUBLIC
    ${CMAKE_SOURCE_DIR}/..  # Project root
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}  # tests/ for common/
    ${PHP_INCLUDES_LIST}
)

# Link PHP library
if(WIN32)
    # PHP_LIBRARY already resolved during PHP detection above.
    target_link_libraries(test_common PUBLIC ${PHP_LIBRARY})

    # Record the DLL directory so unit test targets can add it to PATH.
    get_filename_component(PHP_DLL_DIR "${PHP_LIBRARY}" DIRECTORY)

    # Resolve cmocka DLL directory for CTest PATH.
    set(CMOCKA_DLL_DIR "")
    if(TARGET cmocka::cmocka)
        foreach(_prop IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION)
            get_target_property(_cmocka_loc cmocka::cmocka ${_prop})
            if(_cmocka_loc)
                get_filename_component(CMOCKA_DLL_DIR "${_cmocka_loc}" DIRECTORY)
                break()
            endif()
        endforeach()
    elseif(CMOCKA_LIBRARY_DIRS)
        set(CMOCKA_DLL_DIR "${CMOCKA_LIBRARY_DIRS}")
    endif()

    # Combined PATH string for set_tests_properties(... ENVIRONMENT "PATH=...").
    set(PHP_TEST_WIN_PATH "${PHP_DLL_DIR}")
    if(CMOCKA_DLL_DIR)
        set(PHP_TEST_WIN_PATH "${PHP_TEST_WIN_PATH};${CMOCKA_DLL_DIR}")
    endif()
else()
    find_library(PHP_LIBRARY
        NAMES php libphp
        PATHS ${PHP_PREFIX}/lib
        NO_DEFAULT_PATH
    )
    if(NOT PHP_LIBRARY)
        message(FATAL_ERROR "PHP library not found in ${PHP_PREFIX}/lib")
    endif()
    target_link_libraries(test_common PUBLIC ${PHP_LIBRARY})
    set_target_properties(test_common PROPERTIES
        INSTALL_RPATH "${PHP_PREFIX}/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Enable testing
enable_testing()

# Add unit tests subdirectory
add_subdirectory(unit)
