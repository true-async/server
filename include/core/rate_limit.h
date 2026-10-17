/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef HTTP_RATE_LIMIT_H
#define HTTP_RATE_LIMIT_H

/* Request rate limit core (#414): who the client is, which rules a request
 * falls under, and one token bucket per client and rule.
 *
 * The client is the socket peer, or, behind a trusted proxy, the rightmost
 * X-Forwarded-For element that is not itself a trusted proxy. Each rule keeps
 * a fixed open-addressed table of buckets keyed by a SipHash-2-4 tag of the
 * client; a key that finds its four slots taken evicts one, which only ever
 * gives the evicted client a fresh, full bucket. A request is checked against
 * every rule it matches before any token is taken, so a refusal by one rule
 * costs nothing in the others.
 *
 * Plain C over caller-owned memory, no Zend: the PHP side (rate_limit_php.c)
 * owns allocation, configuration and the per-thread tables, and the unit
 * suite drives this file directly. Nothing here locks: a table belongs to
 * one server instance, which runs on one thread. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HTTP_RL_MAX_TRUSTED 64u	   /* setTrustedProxies() entries */
#define HTTP_RL_MAX_PATHS 32u	   /* paths() prefixes per rule */
#define HTTP_RL_MAX_METHODS 32u	   /* methods() tokens per rule */
#define HTTP_RL_MAX_METHOD_LEN 64u /* bytes per method token */
#define HTTP_RL_MAX_PATH_LEN 1024u /* bytes per path prefix */
#define HTTP_RL_PROBE_WIDTH 4u	   /* slots a key may occupy */
#define HTTP_RL_NS_PER_S 1000000000ULL
/* Largest bucket, in nanoseconds of credit: keeps credit + elapsed below 2^63. */
#define HTTP_RL_MAX_BUCKET_NS (1ULL << 62)

/* Address families of a client key. NONE is a peer the transport could not
 * name (getpeername failed); it is its own key and never trusted. */
typedef enum
{
	HTTP_RL_FAMILY_NONE = 0,
	HTTP_RL_FAMILY_UNIX = 1,
	HTTP_RL_FAMILY_V4 = 4,
	HTTP_RL_FAMILY_V6 = 6,
} http_rl_family_t;

/* A client address. V4 uses bytes[0..3]; V6 all 16; UNIX and NONE none. */
typedef struct
{
	uint8_t family;
	uint8_t bytes[16];
} http_rl_addr_t;

/* An address block: the first `prefix_bits` bits of `addr` (0..32 or 0..128). */
typedef struct
{
	http_rl_addr_t addr;
	uint8_t prefix_bits;
} http_rl_cidr_t;

/* The trusted-proxy list of a config. Plain data: copied by value into the
 * frozen config and back out on each worker thread. */
typedef struct
{
	http_rl_cidr_t entries[HTTP_RL_MAX_TRUSTED];
	uint8_t count;
	bool unix_trusted; /* "unix": Unix-socket peers are proxies */
} http_rl_trusted_t;

/* SipHash-2-4 (Aumasson and Bernstein) of `len` bytes under a 16-byte key. */
uint64_t http_rl_siphash24(const uint8_t key[16], const void *data, size_t len);

/* The address of a socket peer. An IPv4-mapped IPv6 address (::ffff:0:0/96)
 * becomes its IPv4 address; nothing else is rewritten. AF_UNIX is UNIX;
 * `sa == NULL`, `sa_len == 0` or another family is NONE. */
void http_rl_addr_from_sockaddr(const void *sa, size_t sa_len, http_rl_addr_t *out);

/* One X-Forwarded-For element, already trimmed: `a.b.c.d`, `a.b.c.d:port`,
 * `[v6]`, `[v6]:port` or a bare IPv6 address. Un-maps as above. False for
 * anything else (`unknown`, an obfuscated `_name`, empty, a bad port). */
bool http_rl_addr_parse(const char *text, size_t len, http_rl_addr_t *out);

/* A setTrustedProxies() entry other than `unix`: an IPv4 or IPv6 address,
 * optionally `/bits`. A mapped IPv6 block of /96 or longer becomes the IPv4
 * block it covers; a shorter one is refused, as it covers no IPv4 address
 * alone. False on anything else. */
bool http_rl_cidr_parse(const char *text, size_t len, http_rl_cidr_t *out);

/* `addr/bits` into `out` (at least 64 bytes); returns the length. */
size_t http_rl_cidr_format(const http_rl_cidr_t *cidr, char *out, size_t out_size);

/* Whether `addr` is a trusted proxy. UNIX answers `unix_trusted`; NONE never. */
bool http_rl_trusted_contains(const http_rl_trusted_t *trusted, const http_rl_addr_t *addr);

/* The address a rule keys the request by. A peer that is not trusted is the
 * client. A trusted one hands over to X-Forwarded-For (`forwarded` may be
 * NULL), walked from the right: trusted elements are skipped, the first other
 * element is the client, and an element that does not parse, or the end of
 * the list, leaves the last trusted hop as the client. Elements left of the
 * stop are never parsed. */
void http_rl_client_address(const http_rl_trusted_t *trusted, const http_rl_addr_t *peer,
							const char *forwarded, size_t forwarded_len, http_rl_addr_t *client);

/* The bytes hashed for a client: family, prefix length and the address cut to
 * that prefix (IPv4 whole, IPv6 to `ipv6_prefix_bits`). `out` holds 18 bytes;
 * returns the length used. */
size_t http_rl_key_bytes(const http_rl_addr_t *client, uint8_t ipv6_prefix_bits, uint8_t out[18]);

/* The path of a request target as rules match it. `decoded` is the target up
 * to `?`, without the scheme and authority of an absolute-form target, with
 * %XX sequences decoded; `normal` is `decoded` with `//` collapsed and `.` and
 * `..` segments resolved. A `%` not followed by two hex digits sets
 * `malformed`, and such a target matches every rule's path condition. */
typedef struct
{
	const char *decoded;
	size_t decoded_len;
	const char *normal;
	size_t normal_len;
	bool malformed;
} http_rl_path_t;

/* Fills `out` from `target`, writing both forms into `scratch`, which must
 * hold 2 * target_len + 2 bytes. `out` points into `scratch`. */
void http_rl_path_prepare(const char *target, size_t target_len, char *scratch,
						  http_rl_path_t *out);

/* Whether `path` is under `prefix`: equal to it, or starting with it followed
 * by `/`. A prefix is `/` alone or starts with `/` and does not end with it. */
bool http_rl_prefix_matches(const char *prefix, size_t prefix_len, const char *path,
							size_t path_len);

/* Whether a setter's prefix is well formed (see http_rl_prefix_matches). */
bool http_rl_prefix_valid(const char *prefix, size_t prefix_len);

/* One rule as the request path sees it. The strings belong to the owner and
 * outlive the rule. No paths means every path; no methods, every method. */
typedef struct
{
	uint64_t cost_ns;	  /* credit one request takes */
	uint64_t capacity_ns; /* bucket size, cost_ns * burst */
	uint32_t max_keys;	  /* table slots */
	uint8_t ipv6_prefix_bits;
	uint8_t path_count;
	uint8_t method_count;
	const char *paths[HTTP_RL_MAX_PATHS];
	uint16_t path_lens[HTTP_RL_MAX_PATHS];
	const char *methods[HTTP_RL_MAX_METHODS];
	uint8_t method_lens[HTTP_RL_MAX_METHODS];
} http_rl_rule_t;

/* cost and capacity of a bucket of `burst` tokens refilling `requests` per
 * `per_seconds`. False when the bucket would exceed HTTP_RL_MAX_BUCKET_NS.
 * Callers pass validated ranges: requests and burst 1..10^6, per_seconds
 * 1..86400. */
bool http_rl_bucket_params(uint32_t requests, uint32_t per_seconds, uint32_t burst,
						   uint64_t *cost_ns, uint64_t *capacity_ns);

/* Whether a request falls under the rule. Methods compare exactly. */
bool http_rl_rule_matches(const http_rl_rule_t *rule, const char *method, size_t method_len,
						  const http_rl_path_t *path);

/* A bucket. tag 0 marks a free slot; a used tag is odd. */
typedef struct
{
	uint64_t tag;
	uint64_t credit_ns;
	uint64_t stamp_ns;
} http_rl_slot_t;

typedef struct
{
	http_rl_slot_t *slots; /* max_keys entries, zeroed before first use */
	uint32_t max_keys;
} http_rl_table_t;

/* The slot a key takes and the credit it would have at `now`, before a token is
 * taken. `evicts` is true when the slot holds another key. */
typedef struct
{
	uint32_t slot;
	uint64_t credit_ns;
	bool evicts;
} http_rl_probe_t;

/* Finds `hash`'s bucket among HTTP_RL_PROBE_WIDTH slots from hash % max_keys.
 * A new key gets a free slot, else the slot whose bucket has refilled, else
 * the oldest; its credit is a full bucket. Writes nothing. */
void http_rl_table_probe(const http_rl_table_t *table, const http_rl_rule_t *rule, uint64_t hash,
						 uint64_t now_ns, http_rl_probe_t *out);

/* Takes one token from the probed bucket. The probe must say the credit
 * covers rule->cost_ns. */
void http_rl_table_commit(http_rl_table_t *table, const http_rl_rule_t *rule, uint64_t hash,
						  uint64_t now_ns, const http_rl_probe_t *probe);

/* Whole seconds until `credit_ns` reaches `cost_ns`, rounded up, at least 1. */
uint32_t http_rl_retry_after_s(uint64_t cost_ns, uint64_t credit_ns);

/* The client and request a set of rules is evaluated for. */
typedef struct
{
	const http_rl_addr_t *client;
	const char *method;
	size_t method_len;
	const http_rl_path_t *path;
	uint64_t now_ns;
} http_rl_request_t;

/* What one rule did, accumulated across calls. */
typedef struct http_rl_rule_stats_s
{
	uint64_t refused;	/* requests this rule refused */
	uint64_t evictions; /* buckets a new key took from another */
} http_rl_rule_stats_t;

/* Checks every rule the request matches and, only when all admit it, takes a
 * token from each. Returns 0 when admitted, else the longest Retry-After in
 * seconds among the refusing rules; nothing is written to the tables on a
 * refusal. `stats` (rule_count entries) and `evictions` (the total buckets a
 * commit took from other keys) accumulate; either may be NULL. */
uint32_t http_rl_evaluate(const http_rl_rule_t *const *rules, http_rl_table_t *tables,
						  size_t rule_count, const uint8_t hash_key[16],
						  const http_rl_request_t *request, http_rl_rule_stats_t *stats,
						  uint64_t *evictions);

#endif /* HTTP_RATE_LIMIT_H */
