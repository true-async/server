/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  Fault injection through libfiu. A fault point in C is
  `fiu_do_on("area/what", action)` or `fiu_return_on(...)` from
  include/fiu-local.h: without FIU_ENABLE the macros expand to nothing, so a
  release build carries neither the checks nor the library. A phpt enables a
  point by name, drives the server into it, reads what the peer sees, and
  reads the point's hit count to prove the fault was injected at all: a
  misspelt or removed point otherwise leaves the test on the healthy path.

  Points are enabled through fiu_enable_external, so this module decides each
  check itself and counts it; libfiu's own FIU_ONETIME does not count.
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "core/fault_hooks.h"

#ifdef FIU_ENABLE

#include "php.h"
#include "Zend/zend_API.h"
#include "Zend/zend_atomic.h"
#include "zend_exceptions.h"

#include <fiu.h>
#include <fiu-control.h>

#define FAULT_POINTS_MAX   16
#define FAULT_NAME_MAX     64
#define FAULT_UNLIMITED    INT_MAX

/* One enabled point. Written by the enabling thread before
 * fiu_enable_external publishes the point; afterwards only the two atomics
 * change, from whichever reactor thread reaches the point. */
typedef struct {
    char            name[FAULT_NAME_MAX];
    zend_atomic_int remaining; /* failures still to inject; FAULT_UNLIMITED: every check */
    zend_atomic_int hits;      /* failures injected since the last enable */
} fault_point_t;

static fault_point_t   fault_points[FAULT_POINTS_MAX];

/* Stored after the new entry's name is written, so a reactor thread checking
 * another point never scans a half-written name. */
static zend_atomic_int fault_points_used;

static fault_point_t *fault_point_find(const char *name)
{
    const int used = zend_atomic_int_load(&fault_points_used);

    for (int i = 0; i < used; i++) {
        if (strcmp(fault_points[i].name, name) == 0) {
            return &fault_points[i];
        }
    }

    return NULL;
}

/* libfiu calls this under its read lock on every check of an enabled point;
 * a non-zero answer makes fiu_fail() return failnum. */
static int fault_point_decide(const char *name, int *failnum, void **failinfo, unsigned int *flags)
{
    (void)failnum;
    (void)failinfo;
    (void)flags;
    fault_point_t *const point = fault_point_find(name);

    if (point == NULL) {
        return 0;
    }

    int left = zend_atomic_int_load(&point->remaining);

    do {
        if (left == 0) {
            return 0;
        }
    } while (left != FAULT_UNLIMITED
             && !zend_atomic_int_compare_exchange(&point->remaining, &left, left - 1));

    zend_atomic_int_inc(&point->hits);
    return 1;
}

/* Enable a fault point. With $once the point fails on its next check only;
 * without it, on every check until _http_fault_disable(). Enabling a point
 * again resets its hit count. Throws on an empty or over-long name, on more
 * than FAULT_POINTS_MAX distinct names, and on a libfiu refusal. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fault_enable, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, once, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()

PHP_FUNCTION(_http_fault_enable)
{
    zend_string *name;
    bool         once = true;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(name)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(once)
    ZEND_PARSE_PARAMETERS_END();

    if (ZSTR_LEN(name) == 0 || ZSTR_LEN(name) >= FAULT_NAME_MAX) {
        zend_argument_value_error(1, "must be 1 to %d bytes long", FAULT_NAME_MAX - 1);
        RETURN_THROWS();
    }

    fault_point_t *point = fault_point_find(ZSTR_VAL(name));

    if (point == NULL) {
        const int used = zend_atomic_int_load(&fault_points_used);

        if (used == FAULT_POINTS_MAX) {
            zend_throw_error(NULL, "more than %d fault points enabled", FAULT_POINTS_MAX);
            RETURN_THROWS();
        }

        point = &fault_points[used];
        memcpy(point->name, ZSTR_VAL(name), ZSTR_LEN(name) + 1);
        zend_atomic_int_store(&fault_points_used, used + 1);
    }

    zend_atomic_int_store(&point->remaining, once ? 1 : FAULT_UNLIMITED);
    zend_atomic_int_store(&point->hits, 0);

    if (fiu_enable_external(ZSTR_VAL(name), 1, NULL, 0, fault_point_decide) != 0) {
        zend_throw_error(NULL, "fiu_enable_external(\"%s\") failed", ZSTR_VAL(name));
        RETURN_THROWS();
    }
}

/* Disable a fault point; its hit count stays readable. Disabling a point
 * that is not enabled is not an error. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fault_disable, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(_http_fault_disable)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    (void)fiu_disable(ZSTR_VAL(name));
}

/* How many failures the point injected since it was last enabled; 0 for a
 * name never enabled. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fault_hits, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(_http_fault_hits)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    fault_point_t *const point = fault_point_find(ZSTR_VAL(name));
    RETURN_LONG(point != NULL ? zend_atomic_int_load(&point->hits) : 0);
}

/* Check a point the way a fault point in C does. Exists so a test can check
 * the injection itself on a known answer. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fault_fail, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(_http_fault_fail)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(fiu_fail(ZSTR_VAL(name)) != 0);
}

static const zend_function_entry fault_hook_functions[] = {
    ZEND_FE(_http_fault_enable, arginfo_fault_enable)
    ZEND_FE(_http_fault_disable, arginfo_fault_disable)
    ZEND_FE(_http_fault_hits, arginfo_fault_hits)
    ZEND_FE(_http_fault_fail, arginfo_fault_fail)
    PHP_FE_END
};

void fault_hooks_register(const int module_type)
{
    /* fiu_init only allocates the point table; with no point enabled every
     * fiu_fail() answers 0, so the points stay inert until a test asks. */
    (void)fiu_init(0);
    zend_register_functions(NULL, fault_hook_functions, NULL, module_type);
}

#else /* !FIU_ENABLE */

void fault_hooks_register(const int module_type)
{
    (void)module_type;
}

#endif /* FIU_ENABLE */
