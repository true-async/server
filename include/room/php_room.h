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

/* Thrown by Room::send()/trySend() and HttpServer::send() when delivery failed;
 * carries what had landed. Registered by php_room_minit(). */
extern zend_class_entry *room_delivery_exception_ce;

/* Registers the Room class, its object handlers and RoomDeliveryException, from
 * MINIT and after the server's own exceptions. Every build runs it: rooms do not
 * depend on WebSocket. */
void php_room_minit(void);

#endif /* PHP_ROOM_H */
