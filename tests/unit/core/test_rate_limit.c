/* +----------------------------------------------------------------------+
   | Copyright (c) TrueAsync                                              |
   +----------------------------------------------------------------------+
   | Licensed under the Apache License, Version 2.0                       |
   +----------------------------------------------------------------------+

   Request rate limit core (#414): the client a request is keyed by, the rules
   it falls under, and the token buckets behind them.

   Each surface is a pure function over its inputs, so every row is a call:
   SipHash against the reference vectors (the 15-byte one is the paper's,
   all of them agree with OpenSSL's SIPHASH MAC), the bucket arithmetic at the
   bounds the setters validate, the four-slot table, the un-mapping of IPv4
   peers on dual-stack listeners, the X-Forwarded-For walk, prefix matching
   over decoded and normalised paths, and check-then-commit over two rules. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <sys/un.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

#include "core/rate_limit.h"

#define NS HTTP_RL_NS_PER_S

/* {{{ helpers */

static http_rl_addr_t v4(const char *text)
{
    http_rl_addr_t addr;
    assert_true(http_rl_addr_parse(text, strlen(text), &addr));
    assert_int_equal(addr.family, HTTP_RL_FAMILY_V4);
    return addr;
}

static http_rl_addr_t v6(const char *text)
{
    http_rl_addr_t addr;
    assert_true(http_rl_addr_parse(text, strlen(text), &addr));
    assert_int_equal(addr.family, HTTP_RL_FAMILY_V6);
    return addr;
}

static void assert_addr_equal(const http_rl_addr_t *a, const http_rl_addr_t *b)
{
    assert_int_equal(a->family, b->family);
    assert_memory_equal(a->bytes, b->bytes, sizeof(a->bytes));
}

static http_rl_trusted_t trusted_list(const char *const *entries, const size_t count, const bool unix_trusted)
{
    http_rl_trusted_t trusted;
    memset(&trusted, 0, sizeof(trusted));

    for (size_t i = 0; i < count; i++) {
        assert_true(http_rl_cidr_parse(entries[i], strlen(entries[i]), &trusted.entries[i]));
    }

    trusted.count        = (uint8_t)count;
    trusted.unix_trusted = unix_trusted;
    return trusted;
}

static const char *const default_proxies[] = { "127.0.0.1", "::1", "10.0.0.0/8" };

static http_rl_addr_t client_behind(const char *peer_text, const char *forwarded)
{
    const http_rl_trusted_t trusted = trusted_list(default_proxies, 3, false);
    http_rl_addr_t peer;
    assert_true(http_rl_addr_parse(peer_text, strlen(peer_text), &peer));

    http_rl_addr_t client;
    http_rl_client_address(&trusted, &peer, forwarded, forwarded ? strlen(forwarded) : 0, &client);
    return client;
}

static http_rl_rule_t make_rule(const uint32_t requests, const uint32_t per_seconds, const uint32_t burst)
{
    http_rl_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    assert_true(http_rl_bucket_params(requests, per_seconds, burst, &rule.cost_ns, &rule.capacity_ns));
    rule.max_keys         = 1024;
    rule.ipv6_prefix_bits = 64;
    return rule;
}

static http_rl_table_t make_table(const uint32_t max_keys)
{
    http_rl_table_t table;
    table.max_keys = max_keys;
    table.slots    = calloc(max_keys, sizeof(http_rl_slot_t));
    assert_non_null(table.slots);
    return table;
}

/* Takes a token when the bucket allows it; returns the Retry-After otherwise. */
static uint32_t take(http_rl_table_t *table, const http_rl_rule_t *rule, const uint64_t hash, const uint64_t now)
{
    http_rl_probe_t probe;
    http_rl_table_probe(table, rule, hash, now, &probe);

    if (probe.credit_ns < rule->cost_ns) {
        return http_rl_retry_after_s(rule->cost_ns, probe.credit_ns);
    }

    http_rl_table_commit(table, rule, hash, now, &probe);
    return 0;
}

static http_rl_path_t path_of(const char *target, char *scratch)
{
    http_rl_path_t path;
    http_rl_path_prepare(target, strlen(target), scratch, &path);
    return path;
}
/* }}} */

/* {{{ SipHash-2-4 */

static void test_siphash_reference_vectors(void **state)
{
    (void)state;
    uint8_t key[16], message[64];

    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++) message[i] = (uint8_t)i;

    assert_true(http_rl_siphash24(key, message, 0)  == 0x726fdb47dd0e0e31ULL);
    assert_true(http_rl_siphash24(key, message, 1)  == 0x74f839c593dc67fdULL);
    assert_true(http_rl_siphash24(key, message, 7)  == 0xab0200f58b01d137ULL);
    assert_true(http_rl_siphash24(key, message, 8)  == 0x93f5f5799a932462ULL);
    assert_true(http_rl_siphash24(key, message, 15) == 0xa129ca6149be45e5ULL);
    assert_true(http_rl_siphash24(key, message, 63) == 0x958a324ceb064572ULL);
}
/* }}} */

/* {{{ Bucket arithmetic */

static void test_bucket_params_at_the_bounds(void **state)
{
    (void)state;
    uint64_t cost, capacity;

    /* The fastest rule: a million requests a second. */
    assert_true(http_rl_bucket_params(1000000, 1, 1000000, &cost, &capacity));
    assert_true(cost == 1000);
    assert_true(capacity == 1000000000ULL);

    /* The slowest token: one a day. */
    assert_true(http_rl_bucket_params(1, 86400, 1, &cost, &capacity));
    assert_true(cost == 86400ULL * NS);

    /* A day per token times a burst of 10^6 is past 2^62 ns; the largest burst
     * that fits is floor(2^62 / 86400e9) = 53375. */
    assert_false(http_rl_bucket_params(1, 86400, 1000000, &cost, &capacity));
    assert_true(http_rl_bucket_params(1, 86400, 53375, &cost, &capacity));
    assert_true(capacity <= HTTP_RL_MAX_BUCKET_NS);
    assert_false(http_rl_bucket_params(1, 86400, 53376, &cost, &capacity));

    assert_false(http_rl_bucket_params(0, 1, 1, &cost, &capacity));
    assert_false(http_rl_bucket_params(1, 0, 1, &cost, &capacity));
    assert_false(http_rl_bucket_params(1, 1, 0, &cost, &capacity));
}

static void test_retry_after_rounds_up_and_is_at_least_one(void **state)
{
    (void)state;

    assert_int_equal(http_rl_retry_after_s(NS, 0), 1);
    assert_int_equal(http_rl_retry_after_s(NS, NS - 1), 1);
    assert_int_equal(http_rl_retry_after_s(5 * NS / 2, 0), 3);
    assert_int_equal(http_rl_retry_after_s(5 * NS / 2, 3 * NS / 2), 1);
    assert_int_equal(http_rl_retry_after_s(86400ULL * NS, 0), 86400);
    assert_int_equal(http_rl_retry_after_s(1000, 999), 1);
}

static void test_bucket_refill_and_burst(void **state)
{
    (void)state;
    /* Two a second, burst 2: half a second per token. */
    const http_rl_rule_t rule = make_rule(2, 1, 2);
    http_rl_table_t table = make_table(1024);
    const uint64_t t0 = 1000 * NS;

    assert_int_equal(take(&table, &rule, 77, t0), 0);
    assert_int_equal(take(&table, &rule, 77, t0), 0);
    assert_int_equal(take(&table, &rule, 77, t0), 1);

    /* A quarter second refills half a token: still refused. */
    assert_int_equal(take(&table, &rule, 77, t0 + NS / 4), 1);
    assert_int_equal(take(&table, &rule, 77, t0 + NS / 2), 0);
    assert_int_equal(take(&table, &rule, 77, t0 + NS / 2), 1);

    /* An hour idle refills to the burst and no further. */
    const uint64_t later = t0 + 3600 * NS;
    assert_int_equal(take(&table, &rule, 77, later), 0);
    assert_int_equal(take(&table, &rule, 77, later), 0);
    assert_int_equal(take(&table, &rule, 77, later), 1);

    /* Another key has its own bucket. */
    assert_int_equal(take(&table, &rule, 79, later), 0);

    /* A clock that steps back refills nothing. */
    assert_int_equal(take(&table, &rule, 77, later - NS), 1);

    free(table.slots);
}

static void test_bucket_at_the_slowest_rule(void **state)
{
    (void)state;
    /* One a day with the largest burst that validates. */
    const http_rl_rule_t rule = make_rule(1, 86400, 53375);
    http_rl_table_t table = make_table(1024);
    const uint64_t t0 = 5 * NS;

    for (int i = 0; i < 53375; i++) {
        assert_int_equal(take(&table, &rule, 3, t0), 0);
    }

    assert_int_equal(take(&table, &rule, 3, t0), 86400);
    assert_int_equal(take(&table, &rule, 3, t0 + 86399 * NS), 1);
    assert_int_equal(take(&table, &rule, 3, t0 + 86400 * NS), 0);

    /* Idle for the largest elapsed time the clock can show: no overflow. */
    assert_int_equal(take(&table, &rule, 3, UINT64_MAX), 0);

    free(table.slots);
}
/* }}} */

/* {{{ Table */

static void test_tags_are_odd_and_never_free(void **state)
{
    (void)state;
    const http_rl_rule_t rule = make_rule(10, 1, 10);
    http_rl_table_t table = make_table(1024);

    assert_int_equal(take(&table, &rule, 0, NS), 0);
    assert_int_equal(take(&table, &rule, 1024, NS), 0);

    assert_true(table.slots[0].tag == 1);
    assert_true(table.slots[1].tag == 1025);

    for (uint32_t i = 0; i < table.max_keys; i++) {
        assert_true(table.slots[i].tag == 0 || (table.slots[i].tag & 1) == 1);
    }

    free(table.slots);
}

static void test_probe_takeover_prefers_refilled_then_oldest(void **state)
{
    (void)state;
    /* One a second, burst 2. Keys 5 + 2048k share base slot 5 and fill 5..8. */
    const http_rl_rule_t rule = make_rule(1, 1, 2);
    http_rl_table_t table = make_table(1024);
    const uint64_t t0 = 100 * NS;
    const uint64_t key[5] = { 5, 5 + 2048, 5 + 2 * 2048, 5 + 3 * 2048, 5 + 4 * 2048 };

    /* key[2] keeps a token; the others are drained. */
    assert_int_equal(take(&table, &rule, key[0], t0), 0);
    assert_int_equal(take(&table, &rule, key[0], t0), 0);
    assert_int_equal(take(&table, &rule, key[1], t0), 0);
    assert_int_equal(take(&table, &rule, key[1], t0), 0);
    assert_int_equal(take(&table, &rule, key[2], t0 + 1), 0);
    assert_int_equal(take(&table, &rule, key[3], t0 + 2), 0);
    assert_int_equal(take(&table, &rule, key[3], t0 + 2), 0);

    for (uint32_t i = 5; i < 9; i++) {
        assert_true(table.slots[i].tag != 0);
    }

    /* No bucket has refilled: the oldest stamp goes. */
    http_rl_probe_t probe;
    http_rl_table_probe(&table, &rule, key[4], t0 + 3, &probe);
    assert_true(probe.evicts);
    assert_int_equal(probe.slot, 5);
    assert_true(probe.credit_ns == rule.capacity_ns);

    /* A second on, key[2]'s bucket is full again while the older ones are
     * not: it is the one taken. */
    http_rl_table_probe(&table, &rule, key[4], t0 + NS + 1, &probe);
    assert_true(probe.evicts);
    assert_int_equal(probe.slot, 7);
    http_rl_table_commit(&table, &rule, key[4], t0 + NS + 1, &probe);

    /* The evicted key comes back with a full bucket: the takeover only ever
     * gave it tokens. */
    http_rl_table_probe(&table, &rule, key[2], t0 + NS + 2, &probe);
    assert_true(probe.credit_ns == rule.capacity_ns);

    free(table.slots);
}

static void test_probe_wraps_at_the_end_of_the_table(void **state)
{
    (void)state;
    const http_rl_rule_t rule = make_rule(1, 1, 1);
    http_rl_table_t table = make_table(1024);

    for (uint64_t k = 0; k < 4; k++) {
        assert_int_equal(take(&table, &rule, 1023 + 2048 * k, NS), 0);
    }

    assert_true(table.slots[1023].tag != 0);
    assert_true(table.slots[0].tag != 0);
    assert_true(table.slots[1].tag != 0);
    assert_true(table.slots[2].tag != 0);
    assert_true(table.slots[3].tag == 0);

    free(table.slots);
}
/* }}} */

/* {{{ Addresses */

static void test_unmapping_only_touches_v4_mapped(void **state)
{
    (void)state;
    struct sockaddr_in6 sin6;
    http_rl_addr_t addr;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    assert_int_equal(inet_pton(AF_INET6, "::ffff:192.0.2.33", &sin6.sin6_addr), 1);
    http_rl_addr_from_sockaddr(&sin6, sizeof(sin6), &addr);
    assert_addr_equal(&addr, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 192, 0, 2, 33 } });

    assert_int_equal(inet_pton(AF_INET6, "::1", &sin6.sin6_addr), 1);
    http_rl_addr_from_sockaddr(&sin6, sizeof(sin6), &addr);
    assert_int_equal(addr.family, HTTP_RL_FAMILY_V6);
    assert_int_equal(addr.bytes[15], 1);

    /* IPv4-compatible (::a.b.c.d) is not mapped and stays IPv6. */
    assert_int_equal(inet_pton(AF_INET6, "::192.0.2.33", &sin6.sin6_addr), 1);
    http_rl_addr_from_sockaddr(&sin6, sizeof(sin6), &addr);
    assert_int_equal(addr.family, HTTP_RL_FAMILY_V6);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    assert_int_equal(inet_pton(AF_INET, "198.51.100.7", &sin.sin_addr), 1);
    http_rl_addr_from_sockaddr(&sin, sizeof(sin), &addr);
    assert_addr_equal(&addr, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 198, 51, 100, 7 } });

#ifndef _WIN32
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    http_rl_addr_from_sockaddr(&sun, sizeof(sa_family_t), &addr);
    assert_int_equal(addr.family, HTTP_RL_FAMILY_UNIX);
#endif

    http_rl_addr_from_sockaddr(NULL, 0, &addr);
    assert_int_equal(addr.family, HTTP_RL_FAMILY_NONE);
    http_rl_addr_from_sockaddr(&sin, 0, &addr);
    assert_int_equal(addr.family, HTTP_RL_FAMILY_NONE);

    /* The same rule for a forwarded element. */
    const http_rl_addr_t mapped = v4("::ffff:203.0.113.8");
    assert_addr_equal(&mapped, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 203, 0, 113, 8 } });
}

static void test_element_grammar(void **state)
{
    (void)state;
    http_rl_addr_t addr;

    (void)v4("192.0.2.1");
    (void)v4("192.0.2.1:8080");
    (void)v6("[2001:db8::1]");
    (void)v6("[2001:db8::1]:443");
    (void)v6("2001:db8::1");

    const char *const refused[] = {
        "", "unknown", "_obf", "_hidden_proxy", "192.0.2", "192.0.2.256", "01.2.3.4",
        "192.0.2.1:", "192.0.2.1:70000", "192.0.2.1:8a", "[2001:db8::1", "[2001:db8::1]x",
        "[2001:db8::1]:", "[192.0.2.1]", " 192.0.2.1", "2001:db8::g",
    };

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        assert_false(http_rl_addr_parse(refused[i], strlen(refused[i]), &addr));
    }
}

static void test_cidr_parse_and_format(void **state)
{
    (void)state;
    http_rl_cidr_t cidr;
    char text[64];

    assert_true(http_rl_cidr_parse("10.1.2.3/8", 10, &cidr));
    assert_int_equal(cidr.prefix_bits, 8);
    http_rl_cidr_format(&cidr, text, sizeof(text));
    assert_string_equal(text, "10.0.0.0/8");

    assert_true(http_rl_cidr_parse("::ffff:10.0.0.0/104", 19, &cidr));
    assert_int_equal(cidr.addr.family, HTTP_RL_FAMILY_V4);
    http_rl_cidr_format(&cidr, text, sizeof(text));
    assert_string_equal(text, "10.0.0.0/8");

    assert_true(http_rl_cidr_parse("2001:db8::/32", 13, &cidr));
    http_rl_cidr_format(&cidr, text, sizeof(text));
    assert_string_equal(text, "2001:db8::/32");

    assert_true(http_rl_cidr_parse("::1", 3, &cidr));
    assert_int_equal(cidr.prefix_bits, 128);

    const char *const refused[] = {
        "", "unix", "10.0.0.0/33", "10.0.0.0/", "10.0.0.0/-1", "10.0.0.0/8/8", "::/129",
        "::ffff:0:0/95", "10.0.0.0/0008", "10.0.0.0:80", "host.example",
    };

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        assert_false(http_rl_cidr_parse(refused[i], strlen(refused[i]), &cidr));
    }
}

static void test_forwarded_walk(void **state)
{
    (void)state;
    http_rl_addr_t client;

    /* A trusted peer hands over to the header. */
    client = client_behind("127.0.0.1", "203.0.113.7");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 203, 0, 113, 7 } });

    /* Trusted hops on the right are skipped. */
    client = client_behind("127.0.0.1", "203.0.113.7, 10.0.0.2, 10.9.9.9");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 203, 0, 113, 7 } });

    /* Ports and brackets. */
    client = client_behind("127.0.0.1", "198.51.100.1:4711, 10.1.1.1");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 198, 51, 100, 1 } });
    client = client_behind("::1", "[2001:db8::1]:443");
    assert_int_equal(client.family, HTTP_RL_FAMILY_V6);
    assert_int_equal(client.bytes[15], 1);
    client = client_behind("::1", "2001:db8::2");
    assert_int_equal(client.bytes[15], 2);

    /* Spaces and tabs around an element. */
    client = client_behind("127.0.0.1", " \t198.51.100.2 \t");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 198, 51, 100, 2 } });

    /* An element that does not parse stops the walk at the last trusted hop. */
    client = client_behind("127.0.0.1", "203.0.113.7, unknown, 10.0.0.5");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 10, 0, 0, 5 } });
    client = client_behind("127.0.0.1", "_obf, 10.0.0.5");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 10, 0, 0, 5 } });
    client = client_behind("127.0.0.1", "203.0.113.9, , 10.0.0.5");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 10, 0, 0, 5 } });
    client = client_behind("127.0.0.1", "203.0.113.9:99999");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 127, 0, 0, 1 } });
    client = client_behind("127.0.0.1", "");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 127, 0, 0, 1 } });
    client = client_behind("127.0.0.1", ",");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 127, 0, 0, 1 } });

    /* All trusted: the leftmost trusted hop. */
    client = client_behind("127.0.0.1", "10.0.0.1, 10.0.0.2");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 10, 0, 0, 1 } });

    /* A mapped trusted hop is un-mapped before the trust match. */
    client = client_behind("127.0.0.1", "203.0.113.4, ::ffff:10.0.0.3");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 203, 0, 113, 4 } });

    /* Garbage left of the stop is never read. */
    client = client_behind("127.0.0.1", "garbage!!, 203.0.113.5, 10.0.0.9");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 203, 0, 113, 5 } });

    /* No header: the trusted peer itself. */
    client = client_behind("127.0.0.1", NULL);
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 127, 0, 0, 1 } });

    /* An untrusted peer's header is ignored. */
    client = client_behind("192.0.2.1", "1.1.1.1");
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 192, 0, 2, 1 } });
}

static void test_unix_peer(void **state)
{
    (void)state;
    const http_rl_addr_t unix_peer = { HTTP_RL_FAMILY_UNIX, { 0 } };
    const char *const forwarded = "203.0.113.20";
    http_rl_addr_t client;

    /* Without `unix` trusted: the single key `unix`, header ignored. */
    http_rl_trusted_t trusted = trusted_list(default_proxies, 3, false);
    http_rl_client_address(&trusted, &unix_peer, forwarded, strlen(forwarded), &client);
    assert_int_equal(client.family, HTTP_RL_FAMILY_UNIX);

    /* With it: the header names the client, and without one the key is `unix`. */
    trusted.unix_trusted = true;
    http_rl_client_address(&trusted, &unix_peer, forwarded, strlen(forwarded), &client);
    assert_addr_equal(&client, &(http_rl_addr_t){ HTTP_RL_FAMILY_V4, { 203, 0, 113, 20 } });
    http_rl_client_address(&trusted, &unix_peer, NULL, 0, &client);
    assert_int_equal(client.family, HTTP_RL_FAMILY_UNIX);

    /* A peer that could not be named is never trusted. */
    const http_rl_addr_t nameless = { HTTP_RL_FAMILY_NONE, { 0 } };
    assert_false(http_rl_trusted_contains(&trusted, &nameless));
    http_rl_client_address(&trusted, &nameless, forwarded, strlen(forwarded), &client);
    assert_int_equal(client.family, HTTP_RL_FAMILY_NONE);
}

static void test_key_bytes(void **state)
{
    (void)state;
    uint8_t key_a[18], key_b[18];
    size_t len_a, len_b;

    /* One /64 is one key; the next /64 is another. */
    const http_rl_addr_t host_one = v6("2001:db8:1:2::1");
    const http_rl_addr_t host_two = v6("2001:db8:1:2:ffff::9");
    const http_rl_addr_t other    = v6("2001:db8:1:3::1");

    len_a = http_rl_key_bytes(&host_one, 64, key_a);
    len_b = http_rl_key_bytes(&host_two, 64, key_b);
    assert_int_equal(len_a, len_b);
    assert_memory_equal(key_a, key_b, len_a);

    len_b = http_rl_key_bytes(&other, 64, key_b);
    assert_memory_not_equal(key_a, key_b, len_a);

    /* /128 tells hosts apart; /48 groups the subnets. */
    len_a = http_rl_key_bytes(&host_one, 128, key_a);
    len_b = http_rl_key_bytes(&host_two, 128, key_b);
    assert_memory_not_equal(key_a, key_b, len_a);
    len_a = http_rl_key_bytes(&host_one, 48, key_a);
    len_b = http_rl_key_bytes(&other, 48, key_b);
    assert_memory_equal(key_a, key_b, len_a);

    /* A /60 cuts inside a byte. */
    const http_rl_addr_t in_60  = v6("2001:db8:1:f::1");
    const http_rl_addr_t out_60 = v6("2001:db8:1:12::1");
    len_a = http_rl_key_bytes(&host_one, 60, key_a);
    len_b = http_rl_key_bytes(&in_60, 60, key_b);
    assert_memory_equal(key_a, key_b, len_a);
    len_b = http_rl_key_bytes(&out_60, 60, key_b);
    assert_memory_not_equal(key_a, key_b, len_a);

    /* Family is part of the key: 1.2.3.4 and ::102:304 differ. */
    const http_rl_addr_t ipv4   = v4("1.2.3.4");
    const http_rl_addr_t compat = v6("::102:304");
    len_a = http_rl_key_bytes(&ipv4, 128, key_a);
    len_b = http_rl_key_bytes(&compat, 128, key_b);
    assert_true(len_a != len_b || memcmp(key_a, key_b, len_a) != 0);
    assert_int_equal(len_a, 6);
}
/* }}} */

/* {{{ Paths */

static void test_path_prepare(void **state)
{
    (void)state;
    char scratch[256];
    http_rl_path_t path;

    path = path_of("/api/works?x=1", scratch);
    assert_int_equal(path.decoded_len, 10);
    assert_memory_equal(path.decoded, "/api/works", 10);
    assert_false(path.malformed);

    path = path_of("http://example.com/api/works?q", scratch);
    assert_memory_equal(path.decoded, "/api/works", path.decoded_len);
    path = path_of("https://example.com:8443", scratch);
    assert_int_equal(path.decoded_len, 1);
    assert_memory_equal(path.decoded, "/", 1);

    path = path_of("/api/%77orks", scratch);
    assert_memory_equal(path.decoded, "/api/works", path.decoded_len);
    assert_false(path.malformed);

    assert_true(path_of("/api/%7", scratch).malformed);
    assert_true(path_of("/api/%", scratch).malformed);
    assert_true(path_of("/api/%zzworks", scratch).malformed);
    assert_false(path_of("/api/works?%zz", scratch).malformed);

    path = path_of("//api//works", scratch);
    assert_int_equal(path.normal_len, 10);
    assert_memory_equal(path.normal, "/api/works", 10);

    path = path_of("/static/../api/./works", scratch);
    assert_memory_equal(path.normal, "/api/works", path.normal_len);

    path = path_of("/../../api", scratch);
    assert_memory_equal(path.normal, "/api", path.normal_len);

    path = path_of("/a/b/..", scratch);
    assert_int_equal(path.normal_len, 2);
    assert_memory_equal(path.normal, "/a", 2);

    path = path_of("/%2e%2e/api", scratch);
    assert_memory_equal(path.normal, "/api", path.normal_len);

    path = path_of("", scratch);
    assert_int_equal(path.decoded_len, 0);
    assert_memory_equal(path.normal, "/", path.normal_len);
}

static void test_prefix_matching(void **state)
{
    (void)state;

    assert_true(http_rl_prefix_matches("/", 1, "/anything", 9));
    assert_true(http_rl_prefix_matches("/api", 4, "/api", 4));
    assert_true(http_rl_prefix_matches("/api", 4, "/api/works", 10));
    assert_false(http_rl_prefix_matches("/api", 4, "/apiworks", 9));
    assert_true(http_rl_prefix_matches("/api/works", 10, "/api/works/7", 12));
    assert_false(http_rl_prefix_matches("/api/works", 10, "/api/worksheet", 14));
    assert_false(http_rl_prefix_matches("/api/works", 10, "/api", 4));

    assert_true(http_rl_prefix_valid("/", 1));
    assert_true(http_rl_prefix_valid("/api", 4));
    assert_false(http_rl_prefix_valid("/api/", 5));
    assert_false(http_rl_prefix_valid("api", 3));
    assert_false(http_rl_prefix_valid("", 0));
}

static void test_rule_matching(void **state)
{
    (void)state;
    char scratch[256];
    http_rl_rule_t rule = make_rule(10, 1, 10);

    rule.paths[0] = "/api/works";
    rule.path_lens[0] = 10;
    rule.path_count = 1;
    rule.methods[0] = "GET";
    rule.method_lens[0] = 3;
    rule.methods[1] = "HEAD";
    rule.method_lens[1] = 4;
    rule.method_count = 2;

    http_rl_path_t path = path_of("/api/works/3", scratch);
    assert_true(http_rl_rule_matches(&rule, "GET", 3, &path));
    assert_true(http_rl_rule_matches(&rule, "HEAD", 4, &path));
    assert_false(http_rl_rule_matches(&rule, "POST", 4, &path));
    assert_false(http_rl_rule_matches(&rule, "get", 3, &path));

    path = path_of("/api/worksheet", scratch);
    assert_false(http_rl_rule_matches(&rule, "GET", 3, &path));

    /* Encoded and dot-segment spellings reach the same route. */
    path = path_of("/api/%77orks", scratch);
    assert_true(http_rl_rule_matches(&rule, "GET", 3, &path));
    path = path_of("//api//works", scratch);
    assert_true(http_rl_rule_matches(&rule, "GET", 3, &path));
    path = path_of("/static/../api/works", scratch);
    assert_true(http_rl_rule_matches(&rule, "GET", 3, &path));

    /* A malformed target meets the path condition; the method still counts. */
    path = path_of("/other/%zz", scratch);
    assert_true(http_rl_rule_matches(&rule, "GET", 3, &path));
    assert_false(http_rl_rule_matches(&rule, "POST", 4, &path));

    /* No paths and no methods: everything. */
    rule.path_count   = 0;
    rule.method_count = 0;
    path = path_of("/x", scratch);
    assert_true(http_rl_rule_matches(&rule, "DELETE", 6, &path));
}
/* }}} */

/* {{{ Check-then-commit */

static void test_check_then_commit_over_two_rules(void **state)
{
    (void)state;
    char scratch[256];
    const uint8_t hash_key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

    /* `strict`: one per minute under /api. `wide`: ten a second everywhere. */
    http_rl_rule_t strict = make_rule(1, 60, 1);
    strict.paths[0] = "/api";
    strict.path_lens[0] = 4;
    strict.path_count = 1;
    http_rl_rule_t wide = make_rule(10, 1, 10);

    const http_rl_rule_t *const rules[] = { &strict, &wide };
    http_rl_table_t tables[2] = { make_table(1024), make_table(1024) };

    const http_rl_addr_t client = v4("192.0.2.10");
    http_rl_path_t api   = path_of("/api/works", scratch);
    char other_scratch[64];
    http_rl_path_t other = path_of("/home", other_scratch);
    uint64_t evictions = 0;
    http_rl_rule_stats_t stats[2] = { { 0, 0 }, { 0, 0 } };

    http_rl_request_t request = { &client, "GET", 3, &api, 10 * NS };

    assert_int_equal(http_rl_evaluate(rules, tables, 2, hash_key, &request, stats, &evictions), 0);

    /* `wide` has one token gone. */
    uint8_t key[18];
    const size_t key_len = http_rl_key_bytes(&client, 64, key);
    const uint64_t hash  = http_rl_siphash24(hash_key, key, key_len);
    http_rl_probe_t probe;
    http_rl_table_probe(&tables[1], &wide, hash, 10 * NS, &probe);
    assert_true(probe.credit_ns == wide.capacity_ns - wide.cost_ns);

    /* `strict` refuses the next one for a minute, and `wide` keeps its tokens. */
    assert_int_equal(http_rl_evaluate(rules, tables, 2, hash_key, &request, stats, &evictions), 60);
    http_rl_table_probe(&tables[1], &wide, hash, 10 * NS, &probe);
    assert_true(probe.credit_ns == wide.capacity_ns - wide.cost_ns);

    /* Outside /api only `wide` applies. */
    request.path = &other;
    assert_int_equal(http_rl_evaluate(rules, tables, 2, hash_key, &request, stats, &evictions), 0);

    /* Both refusing: the longer wait is reported. */
    for (int i = 0; i < 8; i++) {
        assert_int_equal(http_rl_evaluate(rules, tables, 2, hash_key, &request, stats, &evictions), 0);
    }

    assert_int_equal(http_rl_evaluate(rules, tables, 2, hash_key, &request, stats, &evictions), 1);
    request.path = &api;
    assert_int_equal(http_rl_evaluate(rules, tables, 2, hash_key, &request, stats, &evictions), 60);
    assert_true(evictions == 0);

    /* Each refusing rule counts its own refusal: the last request was
     * refused by both. */
    assert_true(stats[0].refused == 2);
    assert_true(stats[1].refused == 2);

    /* No rule matches: admitted, nothing written. */
    const http_rl_rule_t *const only_strict[] = { &strict };
    request.path = &other;
    assert_int_equal(http_rl_evaluate(only_strict, tables, 1, hash_key, &request, NULL, NULL), 0);

    free(tables[0].slots);
    free(tables[1].slots);
}
/* }}} */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_siphash_reference_vectors),
        cmocka_unit_test(test_bucket_params_at_the_bounds),
        cmocka_unit_test(test_retry_after_rounds_up_and_is_at_least_one),
        cmocka_unit_test(test_bucket_refill_and_burst),
        cmocka_unit_test(test_bucket_at_the_slowest_rule),
        cmocka_unit_test(test_tags_are_odd_and_never_free),
        cmocka_unit_test(test_probe_takeover_prefers_refilled_then_oldest),
        cmocka_unit_test(test_probe_wraps_at_the_end_of_the_table),
        cmocka_unit_test(test_unmapping_only_touches_v4_mapped),
        cmocka_unit_test(test_element_grammar),
        cmocka_unit_test(test_cidr_parse_and_format),
        cmocka_unit_test(test_forwarded_walk),
        cmocka_unit_test(test_unix_peer),
        cmocka_unit_test(test_key_bytes),
        cmocka_unit_test(test_path_prepare),
        cmocka_unit_test(test_prefix_matching),
        cmocka_unit_test(test_rule_matching),
        cmocka_unit_test(test_check_then_commit_over_two_rules),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
