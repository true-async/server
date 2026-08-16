/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_ROOM_H
#define PHP_ROOM_H

#include "php.h"

/* Registers the Room class and its object handlers, from MINIT. A no-op in a
 * build without WebSocket, where every room entry point throws instead — the
 * HttpServer methods that address a room are registered by the generated arginfo
 * either way, so this file compiles in every configuration. */
void php_room_minit(void);

#endif /* PHP_ROOM_H */
