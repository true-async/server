/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* Request rate limit core; the contract is in include/core/rate_limit.h. */

#include "core/rate_limit.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
# ifndef AF_UNIX
#  define AF_UNIX 1
# endif
#else
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

/* {{{ SipHash-2-4 */

#define SIPHASH_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPHASH_ROUND(v0, v1, v2, v3)                                         \
    do {                                                                      \
        v0 += v1; v1 = SIPHASH_ROTL(v1, 13); v1 ^= v0; v0 = SIPHASH_ROTL(v0, 32); \
        v2 += v3; v3 = SIPHASH_ROTL(v3, 16); v3 ^= v2;                        \
        v0 += v3; v3 = SIPHASH_ROTL(v3, 21); v3 ^= v0;                        \
        v2 += v1; v1 = SIPHASH_ROTL(v1, 17); v1 ^= v2; v2 = SIPHASH_ROTL(v2, 32); \
    } while (0)

static uint64_t load_le64(const uint8_t *p)
{
    return (uint64_t)p[0]       | (uint64_t)p[1] << 8  | (uint64_t)p[2] << 16
         | (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40
         | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

uint64_t http_rl_siphash24(const uint8_t key[16], const void *data, const size_t len)
{
    const uint8_t *in = (const uint8_t *)data;
    const uint64_t k0 = load_le64(key);
    const uint64_t k1 = load_le64(key + 8);

    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;

    const size_t whole = len - (len % 8);

    for (size_t offset = 0; offset < whole; offset += 8) {
        const uint64_t m = load_le64(in + offset);

        v3 ^= m;
        SIPHASH_ROUND(v0, v1, v2, v3);
        SIPHASH_ROUND(v0, v1, v2, v3);
        v0 ^= m;
    }

    uint64_t last = (uint64_t)len << 56;

    for (size_t i = 0; i < len % 8; i++) {
        last |= (uint64_t)in[whole + i] << (8 * i);
    }

    v3 ^= last;
    SIPHASH_ROUND(v0, v1, v2, v3);
    SIPHASH_ROUND(v0, v1, v2, v3);
    v0 ^= last;

    v2 ^= 0xff;

    for (int i = 0; i < 4; i++) {
        SIPHASH_ROUND(v0, v1, v2, v3);
    }

    return v0 ^ v1 ^ v2 ^ v3;
}
/* }}} */

/* {{{ Addresses */

static const uint8_t v4_mapped_prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

/* A mapped IPv6 address (::ffff:a.b.c.d) is the IPv4 client it carries: a
 * dual-stack listener reports IPv4 peers that way, and keying them as IPv6
 * would put all of IPv4 in one /64. */
static void addr_set_v6(http_rl_addr_t *out, const uint8_t bytes[16])
{
    memset(out, 0, sizeof(*out));

    if (memcmp(bytes, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0) {
        out->family = HTTP_RL_FAMILY_V4;
        memcpy(out->bytes, bytes + 12, 4);
        return;
    }

    out->family = HTTP_RL_FAMILY_V6;
    memcpy(out->bytes, bytes, 16);
}

void http_rl_addr_from_sockaddr(const void *sa, const size_t sa_len, http_rl_addr_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Winsock has no sa_family_t; the member's own size is portable. */
    if (sa == NULL || sa_len < sizeof(((const struct sockaddr *)sa)->sa_family)) {
        return;
    }

    const struct sockaddr *const base = (const struct sockaddr *)sa;

    if (base->sa_family == AF_INET && sa_len >= sizeof(struct sockaddr_in)) {
        out->family = HTTP_RL_FAMILY_V4;
        memcpy(out->bytes, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        return;
    }

    if (base->sa_family == AF_INET6 && sa_len >= sizeof(struct sockaddr_in6)) {
        addr_set_v6(out, (const uint8_t *)&((const struct sockaddr_in6 *)sa)->sin6_addr);
        return;
    }

    if (base->sa_family == AF_UNIX) {
        out->family = HTTP_RL_FAMILY_UNIX;
    }
}

/* inet_pton wants a terminated string; addresses are short. */
static bool parse_v4(const char *text, const size_t len, http_rl_addr_t *out)
{
    char buf[INET_ADDRSTRLEN];

    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, text, len);
    buf[len] = '\0';

    struct in_addr addr;

    if (inet_pton(AF_INET, buf, &addr) != 1) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->family = HTTP_RL_FAMILY_V4;
    memcpy(out->bytes, &addr, 4);
    return true;
}

static bool parse_v6(const char *text, const size_t len, http_rl_addr_t *out)
{
    char buf[INET6_ADDRSTRLEN];

    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, text, len);
    buf[len] = '\0';

    struct in6_addr addr;

    if (inet_pton(AF_INET6, buf, &addr) != 1) {
        return false;
    }

    addr_set_v6(out, (const uint8_t *)&addr);
    return true;
}

/* A port as X-Forwarded-For writers append it: 1 to 5 decimal digits. */
static bool port_valid(const char *text, const size_t len)
{
    if (len == 0 || len > 5) {
        return false;
    }

    unsigned value = 0;

    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        value = value * 10 + (unsigned)(text[i] - '0');
    }

    return value <= 65535;
}

bool http_rl_addr_parse(const char *text, const size_t len, http_rl_addr_t *out)
{
    if (len == 0) {
        return false;
    }

    if (text[0] == '[') {
        const char *const close = memchr(text, ']', len);

        if (close == NULL) {
            return false;
        }

        const size_t inner_len = (size_t)(close - text) - 1;
        const size_t rest_len  = len - (size_t)(close - text) - 1;

        if (rest_len != 0 && (close[1] != ':' || !port_valid(close + 2, rest_len - 1))) {
            return false;
        }

        return parse_v6(text + 1, inner_len, out);
    }

    const char *const first_colon = memchr(text, ':', len);

    if (first_colon == NULL) {
        return parse_v4(text, len, out);
    }

    /* One colon is IPv4 with a port; more is a bare IPv6 address. */
    const size_t head_len = (size_t)(first_colon - text);

    if (memchr(first_colon + 1, ':', len - head_len - 1) == NULL) {
        return port_valid(first_colon + 1, len - head_len - 1)
            && parse_v4(text, head_len, out);
    }

    return parse_v6(text, len, out);
}

bool http_rl_cidr_parse(const char *text, const size_t len, http_rl_cidr_t *out)
{
    const char *const slash = memchr(text, '/', len);
    const size_t addr_len   = slash != NULL ? (size_t)(slash - text) : len;

    char buf[INET6_ADDRSTRLEN];

    if (addr_len == 0 || addr_len >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, text, addr_len);
    buf[addr_len] = '\0';

    bool    written_v6 = false;
    uint8_t raw[16];

    if (inet_pton(AF_INET, buf, raw) == 1) {
        memset(&out->addr, 0, sizeof(out->addr));
        out->addr.family = HTTP_RL_FAMILY_V4;
        memcpy(out->addr.bytes, raw, 4);
    } else if (inet_pton(AF_INET6, buf, raw) == 1) {
        written_v6 = true;
    } else {
        return false;
    }

    unsigned bits = written_v6 ? 128 : 32;

    if (slash != NULL) {
        const char  *digits     = slash + 1;
        const size_t digits_len = len - addr_len - 1;

        if (digits_len == 0 || digits_len > 3) {
            return false;
        }

        bits = 0;

        for (size_t i = 0; i < digits_len; i++) {
            if (digits[i] < '0' || digits[i] > '9') {
                return false;
            }

            bits = bits * 10 + (unsigned)(digits[i] - '0');
        }

        if (bits > (written_v6 ? 128u : 32u)) {
            return false;
        }
    }

    if (written_v6) {
        addr_set_v6(&out->addr, raw);

        if (out->addr.family == HTTP_RL_FAMILY_V4) {
            if (bits < 96) {
                return false;
            }

            bits -= 96;
        }
    }

    out->prefix_bits = (uint8_t)bits;

    /* Host bits are cleared so formatting and matching agree on one block. */
    const unsigned width = out->addr.family == HTTP_RL_FAMILY_V4 ? 32 : 128;

    for (unsigned bit = bits; bit < width; bit++) {
        out->addr.bytes[bit / 8] &= (uint8_t)~(0x80u >> (bit % 8));
    }

    return true;
}

size_t http_rl_cidr_format(const http_rl_cidr_t *cidr, char *out, const size_t out_size)
{
    char addr[INET6_ADDRSTRLEN];
    const int af = cidr->addr.family == HTTP_RL_FAMILY_V4 ? AF_INET : AF_INET6;

    if (inet_ntop(af, cidr->addr.bytes, addr, sizeof(addr)) == NULL) {
        return 0;
    }

    const int written = snprintf(out, out_size, "%s/%u", addr, (unsigned)cidr->prefix_bits);

    return written > 0 && (size_t)written < out_size ? (size_t)written : 0;
}

static bool prefix_bits_equal(const uint8_t *a, const uint8_t *b, const unsigned bits)
{
    const unsigned whole = bits / 8;

    if (memcmp(a, b, whole) != 0) {
        return false;
    }

    const unsigned rest = bits % 8;

    if (rest == 0) {
        return true;
    }

    const uint8_t mask = (uint8_t)(0xffu << (8 - rest));

    return (a[whole] & mask) == (b[whole] & mask);
}

bool http_rl_trusted_contains(const http_rl_trusted_t *trusted, const http_rl_addr_t *addr)
{
    if (trusted == NULL) {
        return false;
    }

    if (addr->family == HTTP_RL_FAMILY_UNIX) {
        return trusted->unix_trusted;
    }

    if (addr->family != HTTP_RL_FAMILY_V4 && addr->family != HTTP_RL_FAMILY_V6) {
        return false;
    }

    for (unsigned i = 0; i < trusted->count; i++) {
        const http_rl_cidr_t *const entry = &trusted->entries[i];

        if (entry->addr.family == addr->family
            && prefix_bits_equal(entry->addr.bytes, addr->bytes, entry->prefix_bits)) {
            return true;
        }
    }

    return false;
}

static bool is_blank(const char c)
{
    return c == ' ' || c == '\t';
}

void http_rl_client_address(const http_rl_trusted_t *trusted, const http_rl_addr_t *peer,
                            const char *forwarded, const size_t forwarded_len,
                            http_rl_addr_t *client)
{
    *client = *peer;

    if (forwarded == NULL || !http_rl_trusted_contains(trusted, peer)) {
        return;
    }

    size_t end = forwarded_len;

    for (;;) {
        size_t start = end;

        while (start > 0 && forwarded[start - 1] != ',') {
            start--;
        }

        size_t elem_start = start;
        size_t elem_end   = end;

        while (elem_start < elem_end && is_blank(forwarded[elem_start])) {
            elem_start++;
        }

        while (elem_end > elem_start && is_blank(forwarded[elem_end - 1])) {
            elem_end--;
        }

        http_rl_addr_t hop;

        if (!http_rl_addr_parse(forwarded + elem_start, elem_end - elem_start, &hop)) {
            return;   /* the last trusted hop stays the client */
        }

        if (!http_rl_trusted_contains(trusted, &hop)) {
            *client = hop;
            return;
        }

        *client = hop;

        if (start == 0) {
            return;
        }

        end = start - 1;   /* past the comma */
    }
}

size_t http_rl_key_bytes(const http_rl_addr_t *client, const uint8_t ipv6_prefix_bits, uint8_t out[18])
{
    memset(out, 0, 18);
    out[0] = client->family;

    switch (client->family) {
        case HTTP_RL_FAMILY_V4:
            out[1] = 32;
            memcpy(out + 2, client->bytes, 4);
            return 6;

        case HTTP_RL_FAMILY_V6: {
            const unsigned bits = ipv6_prefix_bits > 128 ? 128 : ipv6_prefix_bits;

            out[1] = (uint8_t)bits;
            memcpy(out + 2, client->bytes, bits / 8);

            if (bits % 8 != 0) {
                out[2 + bits / 8] = (uint8_t)(client->bytes[bits / 8] & (0xffu << (8 - bits % 8)));
            }

            return 18;
        }

        default:
            return 1;
    }
}
/* }}} */

/* {{{ Paths */

static int hex_value(const char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Where the path of a target begins and ends: an absolute-form target
 * (`scheme://authority/path`) loses its scheme and authority, and the query
 * is cut. An absolute-form target with no path has the path "/". */
static void target_path_span(const char *target, const size_t len, size_t *begin, size_t *end)
{
    size_t start = 0;

    if (len > 0 && target[0] != '/') {
        size_t i = 0;

        while (i < len && (target[i] == '+' || target[i] == '-' || target[i] == '.'
                           || (target[i] >= '0' && target[i] <= '9')
                           || ((target[i] | 0x20) >= 'a' && (target[i] | 0x20) <= 'z'))) {
            i++;
        }

        if (i > 0 && i + 3 <= len && memcmp(target + i, "://", 3) == 0) {
            start = i + 3;

            while (start < len && target[start] != '/' && target[start] != '?') {
                start++;
            }
        }
    }

    size_t stop = start;

    while (stop < len && target[stop] != '?') {
        stop++;
    }

    *begin = start;
    *end   = stop;
}

/* The decoded path with `//` collapsed, `.` dropped and `..` removing the
 * segment before it (never past the root). The result starts with "/" and has
 * no trailing slash, which matching does not need: a prefix matches "/a" and
 * "/a/" alike. Writes at most len + 1 bytes. */
static size_t path_normalise(const char *in, const size_t len, char *out)
{
    size_t written = 0;
    size_t i = 0;

    out[written++] = '/';

    while (i < len) {
        while (i < len && in[i] == '/') {
            i++;
        }

        size_t seg_end = i;

        while (seg_end < len && in[seg_end] != '/') {
            seg_end++;
        }

        const size_t seg_len = seg_end - i;

        if (seg_len == 2 && in[i] == '.' && in[i + 1] == '.') {
            while (written > 1 && out[written - 1] != '/') {
                written--;
            }

            if (written > 1) {
                written--;   /* the separator before the dropped segment */
            }
        } else if (seg_len > 0 && !(seg_len == 1 && in[i] == '.')) {
            if (written > 1) {
                out[written++] = '/';
            }

            memcpy(out + written, in + i, seg_len);
            written += seg_len;
        }

        i = seg_end;
    }

    return written;
}

void http_rl_path_prepare(const char *target, const size_t target_len, char *scratch, http_rl_path_t *out)
{
    size_t begin, end;
    target_path_span(target, target_len, &begin, &end);

    char *const decoded = scratch;
    size_t decoded_len  = 0;
    bool malformed      = false;

    for (size_t i = begin; i < end; i++) {
        if (target[i] != '%') {
            decoded[decoded_len++] = target[i];
            continue;
        }

        if (i + 2 >= end) {
            malformed = true;
            decoded[decoded_len++] = target[i];
            continue;
        }

        const int hi = hex_value(target[i + 1]);
        const int lo = hex_value(target[i + 2]);

        if (hi < 0 || lo < 0) {
            malformed = true;
            decoded[decoded_len++] = target[i];
            continue;
        }

        decoded[decoded_len++] = (char)(hi << 4 | lo);
        i += 2;
    }

    if (begin == end && begin > 0) {
        decoded[decoded_len++] = '/';   /* absolute-form with no path */
    }

    char *const normal = scratch + decoded_len + 1;

    out->decoded     = decoded;
    out->decoded_len = decoded_len;
    out->normal      = normal;
    out->normal_len  = path_normalise(decoded, decoded_len, normal);
    out->malformed   = malformed;
}

bool http_rl_prefix_valid(const char *prefix, const size_t prefix_len)
{
    if (prefix_len == 0 || prefix_len > HTTP_RL_MAX_PATH_LEN || prefix[0] != '/') {
        return false;
    }

    return prefix_len == 1 || prefix[prefix_len - 1] != '/';
}

bool http_rl_prefix_matches(const char *prefix, const size_t prefix_len,
                            const char *path, const size_t path_len)
{
    if (prefix_len == 1) {
        return true;   /* "/" is every path */
    }

    if (path_len < prefix_len || memcmp(path, prefix, prefix_len) != 0) {
        return false;
    }

    return path_len == prefix_len || path[prefix_len] == '/';
}
/* }}} */

/* {{{ Rules and buckets */

bool http_rl_bucket_params(const uint32_t requests, const uint32_t per_seconds, const uint32_t burst,
                           uint64_t *cost_ns, uint64_t *capacity_ns)
{
    if (requests == 0 || per_seconds == 0 || burst == 0) {
        return false;
    }

    /* per_seconds <= 86400 keeps the product below 2^47. */
    const uint64_t cost = (uint64_t)per_seconds * HTTP_RL_NS_PER_S / requests;

    if (cost == 0 || cost > HTTP_RL_MAX_BUCKET_NS / burst) {
        return false;
    }

    *cost_ns     = cost;
    *capacity_ns = cost * burst;
    return true;
}

bool http_rl_rule_matches(const http_rl_rule_t *rule, const char *method, const size_t method_len,
                          const http_rl_path_t *path)
{
    if (rule->method_count > 0) {
        bool method_listed = false;

        for (unsigned i = 0; i < rule->method_count; i++) {
            if (rule->method_lens[i] == method_len
                && memcmp(rule->methods[i], method, method_len) == 0) {
                method_listed = true;
                break;
            }
        }

        if (!method_listed) {
            return false;
        }
    }

    if (rule->path_count == 0 || path->malformed) {
        return true;
    }

    for (unsigned i = 0; i < rule->path_count; i++) {
        if (http_rl_prefix_matches(rule->paths[i], rule->path_lens[i], path->decoded, path->decoded_len)
            || http_rl_prefix_matches(rule->paths[i], rule->path_lens[i], path->normal, path->normal_len)) {
            return true;
        }
    }

    return false;
}

static uint64_t slot_credit_at(const http_rl_slot_t *slot, const http_rl_rule_t *rule, const uint64_t now_ns)
{
    uint64_t elapsed = now_ns > slot->stamp_ns ? now_ns - slot->stamp_ns : 0;

    /* Clamped before the add: credit and elapsed each stay <= 2^62. */
    if (elapsed > rule->capacity_ns) {
        elapsed = rule->capacity_ns;
    }

    const uint64_t credit = slot->credit_ns + elapsed;

    return credit > rule->capacity_ns ? rule->capacity_ns : credit;
}

void http_rl_table_probe(const http_rl_table_t *table, const http_rl_rule_t *rule,
                         const uint64_t hash, const uint64_t now_ns, http_rl_probe_t *out)
{
    const uint64_t tag  = hash | 1;
    const uint32_t base = (uint32_t)(hash % table->max_keys);

    bool     have_free   = false;
    uint32_t free_slot   = 0;
    bool     have_full   = false;
    uint32_t full_slot   = 0;
    uint32_t oldest_slot = base;
    uint64_t oldest      = UINT64_MAX;

    for (uint32_t j = 0; j < HTTP_RL_PROBE_WIDTH; j++) {
        const uint32_t index = (base + j) % table->max_keys;
        const http_rl_slot_t *const slot = &table->slots[index];

        if (slot->tag == tag) {
            out->slot      = index;
            out->credit_ns = slot_credit_at(slot, rule, now_ns);
            out->evicts    = false;
            return;
        }

        if (slot->tag == 0) {
            if (!have_free) {
                have_free = true;
                free_slot = index;
            }

            continue;
        }

        if (!have_full && slot_credit_at(slot, rule, now_ns) >= rule->capacity_ns) {
            have_full = true;
            full_slot = index;
        }

        if (slot->stamp_ns < oldest) {
            oldest      = slot->stamp_ns;
            oldest_slot = index;
        }
    }

    out->credit_ns = rule->capacity_ns;

    if (have_free) {
        out->slot   = free_slot;
        out->evicts = false;
        return;
    }

    out->slot   = have_full ? full_slot : oldest_slot;
    out->evicts = true;
}

void http_rl_table_commit(http_rl_table_t *table, const http_rl_rule_t *rule,
                          const uint64_t hash, const uint64_t now_ns, const http_rl_probe_t *probe)
{
    http_rl_slot_t *const slot = &table->slots[probe->slot];

    slot->tag       = hash | 1;
    slot->credit_ns = probe->credit_ns - rule->cost_ns;
    slot->stamp_ns  = now_ns;
}

uint32_t http_rl_retry_after_s(const uint64_t cost_ns, const uint64_t credit_ns)
{
    if (credit_ns >= cost_ns) {
        return 1;
    }

    const uint64_t seconds = (cost_ns - credit_ns + HTTP_RL_NS_PER_S - 1) / HTTP_RL_NS_PER_S;

    if (seconds == 0) {
        return 1;
    }

    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

static uint64_t rule_hash(const http_rl_rule_t *rule, const uint8_t hash_key[16], const http_rl_addr_t *client)
{
    uint8_t key[18];
    const size_t key_len = http_rl_key_bytes(client, rule->ipv6_prefix_bits, key);

    return http_rl_siphash24(hash_key, key, key_len);
}

uint32_t http_rl_evaluate(const http_rl_rule_t *const *rules, http_rl_table_t *tables, const size_t rule_count,
                          const uint8_t hash_key[16], const http_rl_request_t *request,
                          http_rl_rule_stats_t *stats, uint64_t *evictions)
{
    uint32_t retry_after = 0;
    bool     any_match   = false;

    /* Check: no rule's bucket moves until every matching rule admits. */
    for (size_t i = 0; i < rule_count; i++) {
        const http_rl_rule_t *const rule = rules[i];

        if (!http_rl_rule_matches(rule, request->method, request->method_len, request->path)) {
            continue;
        }

        any_match = true;

        http_rl_probe_t probe;
        http_rl_table_probe(&tables[i], rule, rule_hash(rule, hash_key, request->client),
                            request->now_ns, &probe);

        if (probe.credit_ns < rule->cost_ns) {
            const uint32_t wait = http_rl_retry_after_s(rule->cost_ns, probe.credit_ns);

            if (wait > retry_after) {
                retry_after = wait;
            }

            if (stats != NULL) {
                stats[i].refused++;
            }
        }
    }

    if (retry_after > 0 || !any_match) {
        return retry_after;
    }

    /* Commit. The probe is taken again: holding one per rule would need storage
     * sized by the rule count, and nothing changed the tables in between. */
    for (size_t i = 0; i < rule_count; i++) {
        const http_rl_rule_t *const rule = rules[i];

        if (!http_rl_rule_matches(rule, request->method, request->method_len, request->path)) {
            continue;
        }

        const uint64_t hash = rule_hash(rule, hash_key, request->client);
        http_rl_probe_t probe;

        http_rl_table_probe(&tables[i], rule, hash, request->now_ns, &probe);
        http_rl_table_commit(&tables[i], rule, hash, request->now_ns, &probe);

        if (!probe.evicts) {
            continue;
        }

        if (stats != NULL) {
            stats[i].evictions++;
        }

        if (evictions != NULL) {
            (*evictions)++;
        }
    }

    return 0;
}
/* }}} */
