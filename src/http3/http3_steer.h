/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+

  HTTP/3 CID steering (#80 D6 / #72): encode the owning reactor's id into
  every server-minted Connection ID so any reactor that receives a stray
  datagram (after a client migration / NAT rebind reshuffles SO_REUSEPORT)
  can decode the owner from the DCID and forward the packet to it.

  The id occupies one byte of the 8-byte CID, masked with a CTR-style
  keystream byte derived from the random nonce in the remaining bytes via
  AES-128(server-secret). Recovering or targeting an id therefore needs the
  per-process key — a plaintext id would let an attacker pin every
  connection onto one reactor (targeted single-thread DoS).
*/

#ifndef HTTP3_STEER_H
#define HTTP3_STEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The CID width steering encodes into — matches HTTP3_SCID_LEN. */
#define HTTP3_STEER_CID_LEN 8

/* Generate the per-process steering secret and expand the AES key. Idempotent;
 * call once on the parent before any reactor mints a CID. Returns false if the
 * DRBG or cipher setup fails (the caller leaves steering inactive). */
bool http3_steer_init(void);

/* Arm/disarm steering process-wide. Active only with >1 reactor (a single
 * reactor owns every connection, so there is nothing to steer) and <=256
 * reactors (the id is one byte). When inactive, encode/decode are never
 * reached — minting falls back to a fully random CID. */
void http3_steer_set_active(bool active);
bool http3_steer_active(void);

/* Encode `reactor_id` into a fresh HTTP3_STEER_CID_LEN-byte CID: random nonce
 * in bytes [1..], id byte at [0] masked with AES(key, nonce)[0]. Returns false
 * on DRBG / cipher failure (caller must fail the mint, never ship a zero CID). */
bool http3_steer_encode(uint8_t *cid, int reactor_id);

/* Recover the reactor id encoded in `cid` (the DCID a peer addressed us with).
 * Returns 0..255, or -1 if the CID is too short or the cipher failed. The
 * caller validates the id against the live reactor count. */
int http3_steer_decode(const uint8_t *cid, size_t cidlen);

#endif /* HTTP3_STEER_H */
