/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_RUNTIME_INIT_H
#define PHP_RUNTIME_INIT_H

/* Initialize PHP runtime for unit tests */
int php_test_runtime_init(void);

/* Shutdown PHP runtime */
void php_test_runtime_shutdown(void);

#endif /* PHP_RUNTIME_INIT_H */
