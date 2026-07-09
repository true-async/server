/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef REACTOR_CMD_H
#define REACTOR_CMD_H

#include "core/reactor_pool.h"   /* reactor_exec_fn */

#ifdef __cplusplus
extern "C" {
#endif

/* Mailbox message, passed BY VALUE through the lock-free ring — must stay a
 * trivially-copyable POD. */
typedef enum {
    REACTOR_CMD_NOOP,   /* liveness token; payload only counted */
    REACTOR_CMD_EXEC,   /* run fn(arg), then store 1 through `done` */
    REACTOR_CMD_POST,   /* fire-and-forget fn(arg) */
    REACTOR_CMD_STOP,   /* cooperative shutdown token */
} reactor_cmd_kind_t;

typedef struct reactor_cmd_s {
    reactor_cmd_kind_t kind;
    void              *payload;  /* NOOP */
    reactor_exec_fn    fn;       /* EXEC / POST */
    void              *arg;      /* EXEC / POST */
    void              *done;     /* EXEC: zend_atomic_int* on caller stack; else NULL */
} reactor_cmd_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REACTOR_CMD_H */
