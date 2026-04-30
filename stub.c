/*
 * Bootstrap source for the Windows EXTENSION() call.
 *
 * PHP-SDK's confutils.js EXTENSION() requires a non-empty source list
 * with at least one real file at the extension root. All actual sources
 * live under src/ and are pulled in via ADD_SOURCES, so this file is
 * intentionally empty.
 *
 * Linux config.m4 builds via PHP_NEW_EXTENSION which has no equivalent
 * requirement — this stub is Windows-specific scaffolding.
 */
