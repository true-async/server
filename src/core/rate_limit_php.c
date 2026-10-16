/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Request rate limit (#414), the server side; see core/rate_limit_php.h. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "Zend/zend_atomic.h"
#include "Zend/zend_hrtime.h"
#include "ext/random/php_random_csprng.h"

#include "php_http_server.h"
#include "core/rate_limit_php.h"
#include "http1/http_parser.h"     /* http_request_t */
#include "log/http_log.h"

#include <string.h>

#include "../../stubs/RateLimit.php_arginfo.h"

zend_class_entry *http_rate_limit_ce = NULL;
static zend_object_handlers http_rate_limit_handlers;

#define RATE_LIMIT_MAX_REQUESTS      1000000u
#define RATE_LIMIT_MAX_PER_SECONDS   86400u
#define RATE_LIMIT_MIN_KEYS          1024u
#define RATE_LIMIT_MAX_KEYS          (1u << 20)
#define RATE_LIMIT_DEFAULT_KEYS      65536u
#define RATE_LIMIT_SERVER_MAX_SLOTS  (1u << 21)   /* 48 MiB of buckets per instance */
#define RATE_LIMIT_MIN_IPV6_PREFIX   48u
#define RATE_LIMIT_DEFAULT_IPV6      64u
#define RATE_LIMIT_MAX_NAME_LEN      64u
/* Targets up to this length are decoded on the stack. */
#define RATE_LIMIT_STACK_TARGET      1024u

/* {{{ TrueAsync\RateLimit */

typedef struct {
    uint32_t     requests;
    uint32_t     per_seconds;
    uint32_t     burst;          /* 0: requests */
    uint32_t     max_keys;
    uint8_t      ipv6_prefix_bits;
    uint8_t      path_count;
    uint8_t      method_count;
    bool         locked;
    zend_string *name;           /* NULL: rule<N> at add */
    zend_string *paths[HTTP_RL_MAX_PATHS];
    zend_string *methods[HTTP_RL_MAX_METHODS];
    zend_object  std;
} http_rate_limit_object;

static inline http_rate_limit_object *rate_limit_from_obj(zend_object *obj)
{
    return (http_rate_limit_object *)((char *)obj - offsetof(http_rate_limit_object, std));
}

#define Z_RATE_LIMIT_P(zv) rate_limit_from_obj(Z_OBJ_P(zv))

static zend_object *rate_limit_create(zend_class_entry *ce)
{
    http_rate_limit_object *rule = zend_object_alloc(sizeof(http_rate_limit_object), ce);

    memset(rule, 0, offsetof(http_rate_limit_object, std));
    rule->max_keys         = RATE_LIMIT_DEFAULT_KEYS;
    rule->ipv6_prefix_bits = RATE_LIMIT_DEFAULT_IPV6;

    zend_object_std_init(&rule->std, ce);
    object_properties_init(&rule->std, ce);
    rule->std.handlers = &http_rate_limit_handlers;

    return &rule->std;
}

static void rate_limit_clear_list(zend_string **list, uint8_t *count)
{
    for (unsigned i = 0; i < *count; i++) {
        zend_string_release(list[i]);
        list[i] = NULL;
    }

    *count = 0;
}

static void rate_limit_free(zend_object *object)
{
    http_rate_limit_object *rule = rate_limit_from_obj(object);

    rate_limit_clear_list(rule->paths, &rule->path_count);
    rate_limit_clear_list(rule->methods, &rule->method_count);

    if (rule->name != NULL) {
        zend_string_release(rule->name);
    }

    zend_object_std_dtor(object);
}

static bool rate_limit_writable(const http_rate_limit_object *rule)
{
    if (UNEXPECTED(rule->locked)) {
        zend_throw_exception(http_server_runtime_exception_ce,
            "Cannot modify a RateLimit after it was added to a server", 0);
        return false;
    }

    return true;
}

static bool rate_limit_bucket_fits(const uint32_t requests, const uint32_t per_seconds, const uint32_t burst)
{
    uint64_t cost, capacity;

    if (http_rl_bucket_params(requests, per_seconds, burst, &cost, &capacity)) {
        return true;
    }

    zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
        "RateLimit: a bucket of %u requests at %u per %u s holds more than 2^62 ns; "
        "lower the burst", burst, requests, per_seconds);
    return false;
}

ZEND_METHOD(TrueAsync_RateLimit, __construct)
{
    zend_long requests;
    zend_long per_seconds;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(requests)
        Z_PARAM_LONG(per_seconds)
    ZEND_PARSE_PARAMETERS_END();

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (requests < 1 || requests > RATE_LIMIT_MAX_REQUESTS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit: requests must be 1 to %u, " ZEND_LONG_FMT " given",
            RATE_LIMIT_MAX_REQUESTS, requests);
        return;
    }

    if (per_seconds < 1 || per_seconds > RATE_LIMIT_MAX_PER_SECONDS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit: perSeconds must be 1 to %u, " ZEND_LONG_FMT " given",
            RATE_LIMIT_MAX_PER_SECONDS, per_seconds);
        return;
    }

    const uint32_t burst = rule->burst != 0 ? rule->burst : (uint32_t)requests;

    if (!rate_limit_bucket_fits((uint32_t)requests, (uint32_t)per_seconds, burst)) {
        return;
    }

    rule->requests    = (uint32_t)requests;
    rule->per_seconds = (uint32_t)per_seconds;
}

ZEND_METHOD(TrueAsync_RateLimit, burst)
{
    zend_long burst;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(burst)
    ZEND_PARSE_PARAMETERS_END();

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (burst < 1 || burst > RATE_LIMIT_MAX_REQUESTS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit::burst(): must be 1 to %u, " ZEND_LONG_FMT " given",
            RATE_LIMIT_MAX_REQUESTS, burst);
        return;
    }

    if (!rate_limit_bucket_fits(rule->requests, rule->per_seconds, (uint32_t)burst)) {
        return;
    }

    rule->burst = (uint32_t)burst;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* Variadic arguments get no type check from the engine. */
static bool rate_limit_args_are_strings(zval *args, const uint32_t argc)
{
    for (uint32_t i = 0; i < argc; i++) {
        if (Z_TYPE(args[i]) != IS_STRING) {
            zend_argument_type_error(i + 1, "must be of type string, %s given",
                                     zend_zval_value_name(&args[i]));
            return false;
        }
    }

    return true;
}

ZEND_METHOD(TrueAsync_RateLimit, paths)
{
    zval    *args = NULL;
    uint32_t argc = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_VARIADIC('+', args, argc)
    ZEND_PARSE_PARAMETERS_END();

    if (!rate_limit_args_are_strings(args, argc)) {
        RETURN_THROWS();
    }

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (argc == 0 || argc > HTTP_RL_MAX_PATHS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit::paths(): 1 to %u prefixes, %u given", HTTP_RL_MAX_PATHS, argc);
        return;
    }

    for (uint32_t i = 0; i < argc; i++) {
        const zend_string *prefix = Z_STR(args[i]);

        if (!http_rl_prefix_valid(ZSTR_VAL(prefix), ZSTR_LEN(prefix))) {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "RateLimit::paths(): \"%s\" must start with \"/\", not end with one "
                "unless it is \"/\", and be at most %u bytes",
                ZSTR_VAL(prefix), HTTP_RL_MAX_PATH_LEN);
            return;
        }
    }

    rate_limit_clear_list(rule->paths, &rule->path_count);

    /* "/" is every path: the same as no path list at all. */
    for (uint32_t i = 0; i < argc; i++) {
        if (zend_string_equals_literal(Z_STR(args[i]), "/")) {
            rate_limit_clear_list(rule->paths, &rule->path_count);
            break;
        }

        rule->paths[rule->path_count++] = zend_string_copy(Z_STR(args[i]));
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

/* RFC 9110 §5.6.2 tchar. */
static bool is_token_char(const unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }

    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

ZEND_METHOD(TrueAsync_RateLimit, methods)
{
    zval    *args = NULL;
    uint32_t argc = 0;

    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_VARIADIC('+', args, argc)
    ZEND_PARSE_PARAMETERS_END();

    if (!rate_limit_args_are_strings(args, argc)) {
        RETURN_THROWS();
    }

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (argc == 0 || argc > HTTP_RL_MAX_METHODS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit::methods(): 1 to %u methods, %u given", HTTP_RL_MAX_METHODS, argc);
        return;
    }

    for (uint32_t i = 0; i < argc; i++) {
        const zend_string *method = Z_STR(args[i]);
        bool valid = ZSTR_LEN(method) > 0 && ZSTR_LEN(method) <= HTTP_RL_MAX_METHOD_LEN;

        for (size_t k = 0; valid && k < ZSTR_LEN(method); k++) {
            valid = is_token_char((unsigned char)ZSTR_VAL(method)[k]);
        }

        if (!valid) {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "RateLimit::methods(): \"%s\" is not a method token", ZSTR_VAL(method));
            return;
        }
    }

    rate_limit_clear_list(rule->methods, &rule->method_count);

    for (uint32_t i = 0; i < argc; i++) {
        rule->methods[rule->method_count++] = zend_string_toupper(Z_STR(args[i]));
    }

    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_RateLimit, maxKeys)
{
    zend_long max_keys;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(max_keys)
    ZEND_PARSE_PARAMETERS_END();

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (max_keys < RATE_LIMIT_MIN_KEYS || max_keys > RATE_LIMIT_MAX_KEYS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit::maxKeys(): must be %u to %u, " ZEND_LONG_FMT " given",
            RATE_LIMIT_MIN_KEYS, RATE_LIMIT_MAX_KEYS, max_keys);
        return;
    }

    rule->max_keys = (uint32_t)max_keys;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

ZEND_METHOD(TrueAsync_RateLimit, ipv6Prefix)
{
    zend_long bits;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(bits)
    ZEND_PARSE_PARAMETERS_END();

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (bits < RATE_LIMIT_MIN_IPV6_PREFIX || bits > 128) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit::ipv6Prefix(): must be %u to 128, " ZEND_LONG_FMT " given",
            RATE_LIMIT_MIN_IPV6_PREFIX, bits);
        return;
    }

    rule->ipv6_prefix_bits = (uint8_t)bits;
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

static bool rate_limit_name_valid(const char *name, const size_t len)
{
    if (len == 0 || len > RATE_LIMIT_MAX_NAME_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }

    return true;
}

ZEND_METHOD(TrueAsync_RateLimit, name)
{
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    http_rate_limit_object *rule = Z_RATE_LIMIT_P(ZEND_THIS);

    if (!rate_limit_writable(rule)) {
        return;
    }

    if (!rate_limit_name_valid(ZSTR_VAL(name), ZSTR_LEN(name))) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "RateLimit::name(): 1 to %u bytes of letters, digits, \"_\", \"-\" and \".\"",
            RATE_LIMIT_MAX_NAME_LEN);
        return;
    }

    if (rule->name != NULL) {
        zend_string_release(rule->name);
    }

    rule->name = zend_string_copy(name);
    RETURN_OBJ_COPY(Z_OBJ_P(ZEND_THIS));
}

void http_rate_limit_class_register(void)
{
    http_rate_limit_ce = register_class_TrueAsync_RateLimit();
    http_rate_limit_ce->create_object = rate_limit_create;

    memcpy(&http_rate_limit_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    http_rate_limit_handlers.offset    = offsetof(http_rate_limit_object, std);
    http_rate_limit_handlers.free_obj  = rate_limit_free;
    http_rate_limit_handlers.clone_obj = NULL;
}
/* }}} */

/* {{{ Frozen rules */

/* One rule as every thread serving the server sees it: persistent memory, the
 * strings in the same block, released by the last of the servers and transit
 * shells holding it. */
typedef struct {
    zend_atomic_int refcount;
    http_rl_rule_t  rule;
    const char     *name;
    size_t          name_len;
    char            strings[];   /* name, paths and methods, back to back */
} rate_limit_frozen_t;

static rate_limit_frozen_t *rate_limit_freeze(const http_rate_limit_object *source,
                                              const char *name, const size_t name_len)
{
    size_t strings_len = name_len;

    for (unsigned i = 0; i < source->path_count; i++) {
        strings_len += ZSTR_LEN(source->paths[i]);
    }

    for (unsigned i = 0; i < source->method_count; i++) {
        strings_len += ZSTR_LEN(source->methods[i]);
    }

    rate_limit_frozen_t *frozen = pecalloc(1, sizeof(*frozen) + strings_len + 1, 1);
    char *cursor = frozen->strings;

    ZEND_ATOMIC_INT_INIT(&frozen->refcount, 1);

    memcpy(cursor, name, name_len);
    frozen->name     = cursor;
    frozen->name_len = name_len;
    cursor += name_len;

    const uint32_t burst = source->burst != 0 ? source->burst : source->requests;

    /* The setters validated these: the parameters fit. */
    (void)http_rl_bucket_params(source->requests, source->per_seconds, burst,
                                &frozen->rule.cost_ns, &frozen->rule.capacity_ns);
    frozen->rule.max_keys         = source->max_keys;
    frozen->rule.ipv6_prefix_bits = source->ipv6_prefix_bits;

    for (unsigned i = 0; i < source->path_count; i++) {
        memcpy(cursor, ZSTR_VAL(source->paths[i]), ZSTR_LEN(source->paths[i]));
        frozen->rule.paths[i]     = cursor;
        frozen->rule.path_lens[i] = (uint16_t)ZSTR_LEN(source->paths[i]);
        cursor += ZSTR_LEN(source->paths[i]);
    }

    frozen->rule.path_count = source->path_count;

    for (unsigned i = 0; i < source->method_count; i++) {
        memcpy(cursor, ZSTR_VAL(source->methods[i]), ZSTR_LEN(source->methods[i]));
        frozen->rule.methods[i]     = cursor;
        frozen->rule.method_lens[i] = (uint8_t)ZSTR_LEN(source->methods[i]);
        cursor += ZSTR_LEN(source->methods[i]);
    }

    frozen->rule.method_count = source->method_count;

    return frozen;
}

static void rate_limit_frozen_addref(rate_limit_frozen_t *frozen)
{
    zend_atomic_int_fetch_add(&frozen->refcount, 1);
}

static void rate_limit_frozen_release(rate_limit_frozen_t *frozen)
{
    if (frozen != NULL && zend_atomic_int_fetch_sub(&frozen->refcount, 1) == 1) {
        pefree(frozen, 1);
    }
}
/* }}} */

/* {{{ A server's set */

struct http_rate_limits_s {
    size_t                 count;
    size_t                 capacity;
    rate_limit_frozen_t  **rules;
    const http_rl_rule_t **views;          /* &rules[i]->rule, for the core */
    size_t                 total_keys;
    bool                   any_paths;      /* some rule matches on the path */

    /* Serving instance only, from http_rate_limits_prepare. */
    http_rl_table_t       *tables;
    http_rl_rule_stats_t  *stats;
    uint8_t                hash_key[16];
};

static void rate_limits_append(http_rate_limits_t *set, rate_limit_frozen_t *frozen)
{
    if (set->count == set->capacity) {
        set->capacity = set->capacity == 0 ? 4 : set->capacity * 2;
        set->rules = erealloc(set->rules, sizeof(*set->rules) * set->capacity);
        set->views = erealloc(set->views, sizeof(*set->views) * set->capacity);
    }

    set->rules[set->count] = frozen;
    set->views[set->count] = &frozen->rule;
    set->count++;
    set->total_keys += frozen->rule.max_keys;
    set->any_paths  |= frozen->rule.path_count > 0;
}

bool http_rate_limits_add(http_rate_limits_t **set_ptr, zend_object *rule_obj)
{
    http_rate_limit_object *rule = rate_limit_from_obj(rule_obj);
    http_rate_limits_t *set = *set_ptr;

    if (UNEXPECTED(rule->requests == 0)) {
        zend_throw_exception(http_server_invalid_argument_exception_ce,
            "addRateLimit(): the RateLimit was not constructed", 0);
        return false;
    }

    const size_t total_keys = (set != NULL ? set->total_keys : 0) + rule->max_keys;

    if (total_keys > RATE_LIMIT_SERVER_MAX_SLOTS) {
        zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
            "addRateLimit(): the server's rules would keep %zu buckets, more than %u; "
            "lower maxKeys()", total_keys, RATE_LIMIT_SERVER_MAX_SLOTS);
        return false;
    }

    char default_name[32];
    const char *name = default_name;
    size_t name_len;

    if (rule->name != NULL) {
        name     = ZSTR_VAL(rule->name);
        name_len = ZSTR_LEN(rule->name);
    } else {
        name_len = (size_t)snprintf(default_name, sizeof(default_name), "rule%zu",
                                    (set != NULL ? set->count : 0) + 1);
    }

    for (size_t i = 0; set != NULL && i < set->count; i++) {
        if (set->rules[i]->name_len == name_len
            && memcmp(set->rules[i]->name, name, name_len) == 0) {
            zend_throw_exception_ex(http_server_invalid_argument_exception_ce, 0,
                "addRateLimit(): the server already has a rule named \"%.*s\"",
                (int)name_len, name);
            return false;
        }
    }

    if (set == NULL) {
        set = ecalloc(1, sizeof(*set));
        *set_ptr = set;
    }

    rule->locked = true;
    rate_limits_append(set, rate_limit_freeze(rule, name, name_len));
    return true;
}

void http_rate_limits_free(http_rate_limits_t *set)
{
    if (set == NULL) {
        return;
    }

    if (set->tables != NULL) {
        for (size_t i = 0; i < set->count; i++) {
            pefree(set->tables[i].slots, 1);
        }

        pefree(set->tables, 1);
        pefree(set->stats, 1);
    }

    for (size_t i = 0; i < set->count; i++) {
        rate_limit_frozen_release(set->rules[i]);
    }

    if (set->rules != NULL) {
        efree(set->rules);
        efree(set->views);
    }

    efree(set);
}

typedef struct {
    size_t               count;
    rate_limit_frozen_t *rules[];
} rate_limits_transit_t;

void *http_rate_limits_transit_create(const http_rate_limits_t *set)
{
    if (set == NULL || set->count == 0) {
        return NULL;
    }

    rate_limits_transit_t *transit =
        pemalloc(sizeof(*transit) + sizeof(transit->rules[0]) * set->count, 1);

    transit->count = set->count;

    for (size_t i = 0; i < set->count; i++) {
        transit->rules[i] = set->rules[i];
        rate_limit_frozen_addref(transit->rules[i]);
    }

    return transit;
}

void http_rate_limits_transit_release(void *transit_ptr)
{
    rate_limits_transit_t *transit = transit_ptr;

    if (transit == NULL) {
        return;
    }

    for (size_t i = 0; i < transit->count; i++) {
        rate_limit_frozen_release(transit->rules[i]);
    }

    pefree(transit, 1);
}

http_rate_limits_t *http_rate_limits_load(const void *transit_ptr)
{
    const rate_limits_transit_t *transit = transit_ptr;

    if (transit == NULL || transit->count == 0) {
        return NULL;
    }

    http_rate_limits_t *set = ecalloc(1, sizeof(*set));

    for (size_t i = 0; i < transit->count; i++) {
        rate_limit_frozen_addref(transit->rules[i]);
        rate_limits_append(set, transit->rules[i]);
    }

    return set;
}

void http_rate_limits_prepare(http_rate_limits_t *set)
{
    if (set == NULL || set->tables != NULL || set->count == 0) {
        return;
    }

    /* Persistent: 1.5 MiB a rule by default is the server's own state, not a
     * request's, and must not count against memory_limit. */
    set->tables = pecalloc(set->count, sizeof(*set->tables), 1);
    set->stats  = pecalloc(set->count, sizeof(*set->stats), 1);

    for (size_t i = 0; i < set->count; i++) {
        set->tables[i].max_keys = set->rules[i]->rule.max_keys;
        set->tables[i].slots    = pecalloc(set->tables[i].max_keys, sizeof(http_rl_slot_t), 1);
    }

    /* A key the clients cannot learn keeps them from steering their buckets
     * into one probe window. */
    if (php_random_bytes_silent(set->hash_key, sizeof(set->hash_key)) != SUCCESS) {
        const uint64_t fallback[2] = { (uint64_t)zend_hrtime(), (uint64_t)(uintptr_t)set };

        memcpy(set->hash_key, fallback, sizeof(set->hash_key));
    }
}

void http_rate_limits_log_start(const http_rate_limits_t *set, const http_server_config_t *config,
                                struct http_log_state *log_state)
{
    if (set == NULL || set->count == 0 || config == NULL
        || config->trusted_proxies.unix_trusted) {
        return;
    }

    for (size_t i = 0; i < config->listener_count; i++) {
        if (config->listeners[i].type == LISTENER_TYPE_UNIX) {
            http_logf_warn(log_state,
                "rate_limit.unix_peers_share_one_key: \"unix\" is not a trusted proxy, "
                "so every client of the Unix listener %s shares one bucket per rule",
                config->listeners[i].host != NULL ? ZSTR_VAL(config->listeners[i].host) : "");
            return;
        }
    }
}

void http_rate_limits_telemetry(const http_rate_limits_t *set, zval *telemetry)
{
    if (set == NULL || set->count == 0) {
        return;
    }

    zval rules;
    array_init_size(&rules, (uint32_t)set->count);

    for (size_t i = 0; i < set->count; i++) {
        zval entry;
        array_init_size(&entry, 2);
        add_assoc_long(&entry, "limited",
                       set->stats != NULL ? (zend_long)set->stats[i].refused : 0);
        add_assoc_long(&entry, "evictions",
                       set->stats != NULL ? (zend_long)set->stats[i].evictions : 0);
        add_assoc_zval_ex(&rules, set->rules[i]->name, set->rules[i]->name_len, &entry);
    }

    add_assoc_zval(telemetry, "rate_limits", &rules);
}

void http_rate_limits_telemetry_reset(http_rate_limits_t *set)
{
    if (set != NULL && set->stats != NULL) {
        memset(set->stats, 0, sizeof(*set->stats) * set->count);
    }
}
/* }}} */

/* {{{ The request path */

bool http_rate_limits_active(const http_server_object *server)
{
    const http_rate_limits_t *set = http_server_rate_limits(server);

    return set != NULL && set->tables != NULL;
}

uint32_t http_rate_limit_check(http_server_object *server, const http_request_t *req,
                               http_server_counters_t *counters)
{
    http_rate_limits_t *set = http_server_rate_limits(server);

    if (EXPECTED(set == NULL || set->tables == NULL)) {
        return 0;
    }

    const http_server_config_t *config = http_server_get_config(server);
    const http_rl_trusted_t *trusted   = config != NULL ? &config->trusted_proxies : NULL;

    http_rl_addr_t peer, client;
    http_rl_addr_from_sockaddr(&req->peer, req->peer_len, &peer);

    const char *forwarded  = NULL;
    size_t forwarded_len   = 0;

    if (trusted != NULL && (trusted->count > 0 || trusted->unix_trusted) && req->headers != NULL) {
        const zval *header = zend_hash_str_find(req->headers, "x-forwarded-for",
                                                sizeof("x-forwarded-for") - 1);

        if (header != NULL && Z_TYPE_P(header) == IS_STRING) {
            forwarded     = Z_STRVAL_P(header);
            forwarded_len = Z_STRLEN_P(header);
        }
    }

    http_rl_client_address(trusted, &peer, forwarded, forwarded_len, &client);

    /* The path is decoded only when some rule reads it. */
    http_rl_path_t path = { "/", 1, "/", 1, false };
    char  stack_scratch[2 * RATE_LIMIT_STACK_TARGET + 2];
    char *scratch = NULL;

    if (set->any_paths && req->uri != NULL) {
        const size_t target_len = ZSTR_LEN(req->uri);

        scratch = target_len <= RATE_LIMIT_STACK_TARGET
            ? stack_scratch : emalloc(2 * target_len + 2);
        http_rl_path_prepare(ZSTR_VAL(req->uri), target_len, scratch, &path);
    }

    const http_rl_request_t request = {
        .client     = &client,
        .method     = req->method != NULL ? ZSTR_VAL(req->method) : "",
        .method_len = req->method != NULL ? ZSTR_LEN(req->method) : 0,
        .path       = &path,
        .now_ns     = http_now_coarse_ns(),
    };

    uint64_t evictions = 0;
    const uint32_t retry_after = http_rl_evaluate(set->views, set->tables, set->count,
                                                  set->hash_key, &request, set->stats, &evictions);

    if (scratch != NULL && scratch != stack_scratch) {
        efree(scratch);
    }

    counters->requests_rate_limit_evictions_total += evictions;

    if (retry_after != 0) {
        counters->requests_rate_limited_total++;
    }

    return retry_after;
}

void http_rate_limit_refuse(zend_object *response, const uint32_t retry_after_s, const bool grpc)
{
    char value[24];

    if (grpc) {
        /* RESOURCE_EXHAUSTED; the response keeps its 200, as gRPC requires. */
        http_response_ensure_grpc_status(response, 8);
        http_response_static_set_trailer(response, "grpc-message", sizeof("grpc-message") - 1,
                                         "rate limit exceeded", sizeof("rate limit exceeded") - 1);

        const int len = snprintf(value, sizeof(value), "%llu",
                                 (unsigned long long)retry_after_s * 1000ULL);
        http_response_static_set_trailer(response, "grpc-retry-pushback-ms",
                                         sizeof("grpc-retry-pushback-ms") - 1, value, (size_t)len);
        return;
    }

    const int len = snprintf(value, sizeof(value), "%u", retry_after_s);

    http_response_static_set_status(response, 429);
    http_response_static_set_header(response, "retry-after", sizeof("retry-after") - 1,
                                    value, (size_t)len);
    http_response_static_set_header(response, "content-type", sizeof("content-type") - 1,
                                    "text/plain; charset=utf-8", sizeof("text/plain; charset=utf-8") - 1);
    http_response_static_set_body_cstr(response, "Too Many Requests\n",
                                       sizeof("Too Many Requests\n") - 1);
}
/* }}} */
