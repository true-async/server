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

#ifndef PHP_TRUE_ASYNC_SERVER_H
#define PHP_TRUE_ASYNC_SERVER_H

#include <Zend/zend_modules.h>

/*
 * Public registration header for the true_async_server extension.
 *
 * It is kept flat at the extension root (not under include/) on purpose:
 * php-src's static-build codegen (build/genif.sh + build/print_include.awk)
 * discovers each bundled extension's module_entry by globbing
 * ext/<name>/*.h non-recursively and grepping the matched files for a
 * "phpext_" line — it does not follow nested #include paths. Keeping the
 * module-entry declaration here, at ext/server/php_true_async_server.h,
 * lets static (non-shared) builds such as static-php-cli find
 * phpext_true_async_server_ptr. The extension's other headers live under
 * include/.
 */

extern zend_module_entry true_async_server_module_entry;
#define phpext_true_async_server_ptr &true_async_server_module_entry

#endif /* PHP_TRUE_ASYNC_SERVER_H */
