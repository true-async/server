# CMake configuration for HTTP Server tests
# Copyright (c) TrueAsync

cmake_minimum_required(VERSION 3.10)
project(http_server_tests C)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")

# Find PHP
find_program(PHP_CONFIG php-config)
if(NOT PHP_CONFIG)
    message(FATAL_ERROR "php-config not found. Please install PHP development files.")
endif()

# Get PHP include directories
execute_process(
    COMMAND ${PHP_CONFIG} --includes
    OUTPUT_VARIABLE PHP_INCLUDES
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Remove -I prefix from each include
string(REPLACE " -I" ";-I" PHP_INCLUDES_WITH_PREFIX ${PHP_INCLUDES})
string(REPLACE "-I" "" PHP_INCLUDES_LIST "${PHP_INCLUDES_WITH_PREFIX}")
message(STATUS "PHP includes: ${PHP_INCLUDES_LIST}")

# Get PHP library directory
execute_process(
    COMMAND ${PHP_CONFIG} --prefix
    OUTPUT_VARIABLE PHP_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get PHP libraries
execute_process(
    COMMAND ${PHP_CONFIG} --libs
    OUTPUT_VARIABLE PHP_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Find CMocka
find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA REQUIRED cmocka)

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

target_compile_definitions(test_common PUBLIC
    _GNU_SOURCE
    HAVE_LLHTTP=1
    HAVE_PHP_H=1
)

target_include_directories(test_common PUBLIC
    ${CMAKE_SOURCE_DIR}/..  # Project root
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}  # tests/ for common/
    ${PHP_INCLUDES_LIST}
)

# Link PHP library
if(WIN32)
    # Windows: link with php8.lib (or similar)
    find_library(PHP_LIBRARY
        NAMES php8 php libphp
        PATHS ${PHP_PREFIX}/lib ${PHP_PREFIX}/bin
        NO_DEFAULT_PATH
    )
    if(NOT PHP_LIBRARY)
        message(FATAL_ERROR "PHP library not found. Looked in ${PHP_PREFIX}/lib and ${PHP_PREFIX}/bin")
    endif()
    target_link_libraries(test_common ${PHP_LIBRARY})
else()
    # Unix: link with libphp.so
    find_library(PHP_LIBRARY
        NAMES php libphp
        PATHS ${PHP_PREFIX}/lib
        NO_DEFAULT_PATH
    )
    if(NOT PHP_LIBRARY)
        message(FATAL_ERROR "PHP library not found in ${PHP_PREFIX}/lib")
    endif()
    target_link_libraries(test_common ${PHP_LIBRARY})
    # Add rpath for libphp.so
    set_target_properties(test_common PROPERTIES
        INSTALL_RPATH "${PHP_PREFIX}/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Enable testing
enable_testing()

# Add unit tests subdirectory
add_subdirectory(unit)
