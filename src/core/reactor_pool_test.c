/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Reactor pool test hook (#80). Entirely gated behind HTTP_SERVER_TEST_HOOKS
  (--enable-http-server-test-hooks); never present in a release build.
  See include/core/reactor_pool_test.h.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/reactor_pool_test.h"

#ifdef HTTP_SERVER_TEST_HOOKS

#include "php.h"
#include "Zend/zend_API.h"
#include "core/reactor_pool.h"

#include <stdint.h>

#ifdef PHP_WIN32
# include <windows.h>
#else
# include <time.h>
#endif

/* Upper bound on how long the self-test waits for reactors to drain. */
#define REACTOR_SELFTEST_WAIT_MS 5000

static void selftest_msleep(void)
{
#ifdef PHP_WIN32
    Sleep(1);
#else
    const struct timespec ts = { 0, 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);
#endif
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_reactor_pool_selftest, 0, 2,
                                        MAY_BE_ARRAY | MAY_BE_FALSE)
    ZEND_ARG_TYPE_INFO(0, reactors, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, items_per_reactor, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Spin up `reactors` transport reactors, post `items_per_reactor` opaque tokens
 * into each reactor's #81 inbound, wait for them to drain, tear down, and return
 * the per-reactor drained counts (or false on spawn failure). Exercises spawn,
 * channel drain, per-reactor isolation, and clean shutdown. */
PHP_FUNCTION(_http_server_reactor_pool_selftest)
{
    zend_long reactors = 0;
    zend_long items = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(reactors)
        Z_PARAM_LONG(items)
    ZEND_PARSE_PARAMETERS_END();

    if (reactors <= 0 || items < 0) {
        RETURN_FALSE;
    }

    reactor_pool_t *const rp = reactor_pool_create((int)reactors);

    if (rp == NULL) {
        RETURN_FALSE;
    }

    const int count = reactor_pool_count(rp);

    for (int r = 0; r < count; r++) {
        for (zend_long k = 1; k <= items; k++) {
            void *const token = (void *)(uintptr_t)k; /* opaque, never deref'd */

            while (!reactor_pool_post(rp, r, token)) {
                selftest_msleep(); /* mailbox full: let the reactor drain */
            }
        }
    }

    /* Reactors drain on their own threads; bounded wait for completion. */
    for (int waited = 0; waited < REACTOR_SELFTEST_WAIT_MS; waited++) {
        bool all_done = true;

        for (int r = 0; r < count; r++) {
            if (reactor_pool_processed(rp, r) < (uint64_t)items) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            break;
        }

        selftest_msleep();
    }

    array_init(return_value);

    for (int r = 0; r < count; r++) {
        add_next_index_long(return_value, (zend_long)reactor_pool_processed(rp, r));
    }

    reactor_pool_destroy(rp);
}

static const zend_function_entry reactor_pool_test_functions[] = {
    ZEND_FE(_http_server_reactor_pool_selftest, arginfo_reactor_pool_selftest)
    PHP_FE_END
};

void reactor_pool_test_register(const int module_type)
{
    zend_register_functions(NULL, reactor_pool_test_functions, NULL, module_type);
}

#else /* !HTTP_SERVER_TEST_HOOKS */

void reactor_pool_test_register(const int module_type)
{
    (void)module_type;
}

#endif /* HTTP_SERVER_TEST_HOOKS */
