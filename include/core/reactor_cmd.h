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

/*
 * Inbound message a reactor thread drains from its mailbox. Passed BY VALUE
 * through the lock-free ring (no per-message heap allocation), so it must stay
 * a trivially-copyable POD. The drain dispatches on `kind`:
 *   NOOP — opaque liveness token (reactor_pool_post); `payload` is only counted.
 *   EXEC — reactor_pool_exec: run fn(arg), then publish completion by storing 1
 *          through `done` (a zend_atomic_int* on the blocking caller's stack —
 *          a pointer, not an embedded atomic, precisely because the envelope is
 *          copied into the ring).
 *   POST — reactor_pool_post_exec: fire-and-forget fn(arg); no `done` ack and
 *          nothing to free (the value lived in the ring, not the heap).
 *   STOP — cooperative shutdown token (reactor_pool_destroy): the reactor sets
 *          its stopping flag and leaves its loop. Travels the same ring as work
 *          (a value queue has no out-of-band sentinel pointer).
 */
typedef enum {
    REACTOR_CMD_NOOP,
    REACTOR_CMD_EXEC,
    REACTOR_CMD_POST,
    REACTOR_CMD_STOP,
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
