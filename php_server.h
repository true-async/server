/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | https://www.php.net/license/3_01.txt                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_SERVER_STATIC_H
#define PHP_SERVER_STATIC_H

#include <Zend/zend_modules.h>

/*
 * php-src's static-build codegen (build/genif.sh + build/print_include.awk)
 * discovers each bundled extension's module_entry by globbing
 * ext/<name>/*.h directly and grepping matched files for a "phpext_" line
 * — it does not follow nested #include paths. This extension keeps its
 * real header under include/php_http_server.h, so this flat file exists
 * purely so static (non-shared, e.g. static-php-cli) builds can find
 * phpext_true_async_server_ptr. Shared/dynamic builds never read this file.
 */

extern zend_module_entry true_async_server_module_entry;
#define phpext_true_async_server_ptr &true_async_server_module_entry

#endif
