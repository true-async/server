/*
 * Response-side compression plumbing — see header for the public surface.
 *
 * Two consumers, one decision function:
 *   - apply_buffered  : called from http_response_format[/_parts]; rewrites
 *                       smart_str body, mutates headers in place. The
 *                       buffered path knows the body length up-front, so
 *                       the size-threshold check is exact.
 *   - stream wrapper  : on first send() we substitute the installed
 *                       stream_ops with a compressing one. The wrapper's
 *                       append_chunk feeds chunks through the encoder and
 *                       forwards compressed slices to the underlying ops;
 *                       mark_ended drains finish() before delegating.
 *
 * `decide()` is the single source of truth: it reads request headers,
 * response headers, server config, and the opt-out flag, returning
 * GZIP or IDENTITY. Both consumers call it; the buffered path also
 * passes the known body length to bypass the streaming "unknown size"
 * branch.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_HTTP_COMPRESSION

#include "php.h"
#include "Zend/zend_smart_str.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "compression/http_compression_response.h"
#include "compression/http_compression_negotiate.h"
#include "compression/http_compression_defaults.h"

#include <string.h>

/* ----- state struct + accessors -------------------------------------- */

typedef struct {
    http_request_t                   *request;       /* non-owning */
    http_server_config_t             *cfg;           /* non-owning */

    bool                              no_compression;
    bool                              applied;        /* buffered: body already rewritten */

    /* Streaming wrapper state. Populated by
     * maybe_install_stream_wrapper; NULL on the buffered path. */
    const http_response_stream_ops_t *underlying_ops;
    void                             *underlying_ctx;
    http_encoder_t                   *encoder;
    void                             *wrapper_ctx;        /* ws_ctx_t — owned, freed at teardown */
    bool                              wrapper_installed;
    bool                              wrapper_first_chunk;
} http_compression_state_t;

/* http_response.c owns the response struct; we reach the field through
 * a tiny accessor it exports for us. Using a void** keeps the response
 * layout opaque outside the response TU. */
extern void  *http_response_get_compression_slot(zend_object *obj);
extern void   http_response_set_compression_slot(zend_object *obj, void *p);
extern HashTable *http_response_get_headers(zend_object *obj);
extern int        http_response_get_status(zend_object *obj);
extern const http_response_stream_ops_t *
                  http_response_get_stream_ops(zend_object *obj);
extern void      *http_response_get_stream_ctx(zend_object *obj);
extern void       http_response_replace_stream_ops(zend_object *obj,
                                  const http_response_stream_ops_t *ops,
                                  void *ctx);
extern smart_str *http_response_get_body_smart_str(zend_object *obj);

static inline http_compression_state_t *state_of(zend_object *obj)
{
    return (http_compression_state_t *)http_response_get_compression_slot(obj);
}

/* ----- header mutation helpers --------------------------------------- */

/* Direct HashTable insertion bypasses the user-facing setHeader guard
 * (which blocks edits after committed=true). That's intentional —
 * compression mutations happen *during* commit. zend_hash_str_update
 * skips the key zend_string allocation (vs. zend_hash_update); only
 * the value needs to be reified. */
static void put_header_string(HashTable *ht, const char *name, size_t name_len,
                              const char *value, size_t value_len)
{
    zval z;
    ZVAL_STR(&z, zend_string_init(value, value_len, 0));
    zend_hash_str_update(ht, name, name_len, &z);
}

static inline void delete_header(HashTable *ht, const char *name, size_t name_len)
{
    zend_hash_str_del(ht, name, name_len);
}

static inline bool has_header(const HashTable *ht, const char *name, size_t name_len)
{
    return zend_hash_str_exists(ht, name, name_len);
}

/* Append "Accept-Encoding" to an existing Vary, or set it fresh. */
static void merge_vary_accept_encoding(HashTable *ht)
{
    static const char V[]    = "vary";
    static const char AE[]   = "Accept-Encoding";
    const size_t      AE_LEN = sizeof(AE) - 1;

    zval *const existing = zend_hash_str_find(ht, V, sizeof(V) - 1);
    if (existing == NULL) {
        put_header_string(ht, V, sizeof(V) - 1, AE, AE_LEN);
        return;
    }
    if (EXPECTED(Z_TYPE_P(existing) == IS_STRING)) {
        const char *const cur = Z_STRVAL_P(existing);
        const size_t      cl  = Z_STRLEN_P(existing);
        /* Already mentions Accept-Encoding (case-insensitive needle)?
         * Loop bound guarantees no read past the buffer end. */
        if (cl >= AE_LEN) {
            for (size_t i = 0, stop = cl - AE_LEN + 1; i < stop; i++) {
                if (strncasecmp(cur + i, AE, AE_LEN) == 0) return;
            }
        }
        /* Build "<cur>, Accept-Encoding" in one allocation, store it. */
        zend_string *const merged = zend_string_alloc(cl + 2 + AE_LEN, 0);
        memcpy(ZSTR_VAL(merged), cur, cl);
        memcpy(ZSTR_VAL(merged) + cl, ", ", 2);
        memcpy(ZSTR_VAL(merged) + cl + 2, AE, AE_LEN);
        ZSTR_VAL(merged)[cl + 2 + AE_LEN] = '\0';
        zval z;
        ZVAL_STR(&z, merged);
        zend_hash_str_update(ht, V, sizeof(V) - 1, &z);
        return;
    }
    /* IS_ARRAY: rare; just add a fresh entry — most clients dedup Vary. */
    if (Z_TYPE_P(existing) == IS_ARRAY) {
        zval z;
        ZVAL_STR(&z, zend_string_init(AE, AE_LEN, 0));
        zend_hash_next_index_insert(Z_ARRVAL_P(existing), &z);
    }
}

static void mutate_headers_for_codec(HashTable *ht, http_codec_id_t codec)
{
    const char *tok = http_compression_codec_token(codec);
    put_header_string(ht, "content-encoding", sizeof("content-encoding") - 1,
                      tok, strlen(tok));
    /* Content-Length is recomputed by emit_headers_block from the new
     * body size on the buffered path; on streaming we don't know it,
     * so we always strip whatever the handler set. */
    delete_header(ht, "content-length", sizeof("content-length") - 1);
    merge_vary_accept_encoding(ht);
}

/* ----- decide() ------------------------------------------------------- */

/* Look up a header value as a string. The H1 parser lowercases keys;
 * H2/H3 pass lowercase via :pseudo handling — case-sensitive lookup
 * suffices. Returns false on absence or non-string entry. */
static bool request_header_value(const http_request_t *req,
                                 const char *lower_name, size_t lower_len,
                                 const char **out_val, size_t *out_len)
{
    if (UNEXPECTED(req == NULL || req->headers == NULL)) return false;
    const zval *zv = zend_hash_str_find(req->headers, lower_name, lower_len);
    if (zv == NULL || Z_TYPE_P(zv) != IS_STRING) return false;
    *out_val = Z_STRVAL_P(zv);
    *out_len = Z_STRLEN_P(zv);
    return true;
}

/* Cheap presence-only check — no zval read, no string copy. */
static inline bool request_has_header(const http_request_t *req,
                                      const char *lower_name, size_t lower_len)
{
    if (UNEXPECTED(req == NULL || req->headers == NULL)) return false;
    return zend_hash_str_exists(req->headers, lower_name, lower_len);
}

static inline bool method_is_head(const http_request_t *req)
{
    return req && req->method &&
           ZSTR_LEN(req->method) == 4 &&
           zend_binary_strcasecmp(ZSTR_VAL(req->method), 4, "HEAD", 4) == 0;
}

/* Read the chosen content-type from the response's headers HT. The
 * dispatch flow lowercases stored keys, so a case-sensitive lookup
 * suffices. Returns 0 when the handler did not set one. */
static size_t response_content_type(const HashTable *resp_headers,
                                    char *buf, size_t buf_cap)
{
    const zval *zv = zend_hash_str_find(resp_headers, "content-type", 12);
    if (zv == NULL || Z_TYPE_P(zv) != IS_STRING) return 0;
    return http_compression_mime_normalize(
        Z_STRVAL_P(zv), Z_STRLEN_P(zv), buf, buf_cap);
}

/* size_hint==0 means "unknown / streaming" — skip the size-threshold
 * check (we'd rather compress and risk a slight overhead than refuse
 * compression on potentially huge streamed bodies). */
static http_codec_id_t decide(http_compression_state_t *st,
                              zend_object *response_obj,
                              size_t size_hint)
{
    if (st == NULL || st->no_compression || st->cfg == NULL ||
        !st->cfg->compression_enabled) {
        return HTTP_CODEC_IDENTITY;
    }

    /* --- request side --- */
    if (method_is_head(st->request)) {
        return HTTP_CODEC_IDENTITY;
    }
    /* Range responses are sliced; compressing them would corrupt the
     * byte ranges the client asked for. */
    if (request_has_header(st->request, "range", 5)) {
        return HTTP_CODEC_IDENTITY;
    }
    const char *ae_val = NULL; size_t ae_len = 0;
    http_accept_encoding_t ae;
    if (request_header_value(st->request, "accept-encoding", 15, &ae_val, &ae_len)) {
        http_accept_encoding_parse(ae_val, ae_len, &ae);
    } else {
        http_accept_encoding_init_default(&ae);
    }
    http_codec_id_t chosen = http_accept_encoding_select(&ae);
    /* select() may return ZSTD, BROTLI, GZIP, IDENTITY, or COUNT. Anything
     * other than a real encodable codec falls back to identity — the 406
     * path is not in scope here; the dispose code commits identity by
     * default. */
    if (chosen != HTTP_CODEC_GZIP &&
        chosen != HTTP_CODEC_BROTLI &&
        chosen != HTTP_CODEC_ZSTD) {
        return HTTP_CODEC_IDENTITY;
    }

    /* --- response side --- */
    int status = http_response_get_status(response_obj);
    if (status < 200 || status == 204 || status == 304) {
        return HTTP_CODEC_IDENTITY;
    }

    HashTable *resp_h = http_response_get_headers(response_obj);
    if (resp_h && has_header(resp_h, "content-encoding", 16)) {
        /* Handler already set its own coding (e.g. precompressed asset).
         * Don't double-encode. */
        return HTTP_CODEC_IDENTITY;
    }

    /* Body-size threshold is exact for buffered, skipped for streaming. */
    if (size_hint > 0 && size_hint < st->cfg->compression_min_size) {
        return HTTP_CODEC_IDENTITY;
    }

    /* MIME whitelist match. No content-type → assume non-text and skip
     * (whitelist semantics: compress only what we explicitly know is safe). */
    char ct_buf[128];
    size_t ct_len = response_content_type(resp_h, ct_buf, sizeof(ct_buf));
    if (ct_len == 0) {
        return HTTP_CODEC_IDENTITY;
    }
    if (st->cfg->compression_mime_types == NULL ||
        !zend_hash_str_exists(st->cfg->compression_mime_types, ct_buf, ct_len)) {
        return HTTP_CODEC_IDENTITY;
    }

    return chosen;
}

/* Pick the configured level for a codec. Each backend clamps internally,
 * so out-of-range values are still safe — this just keeps the call site
 * codec-agnostic. */
static inline int level_for_codec(const http_server_config_t *const cfg,
                                  const http_codec_id_t codec)
{
    switch (codec) {
        case HTTP_CODEC_GZIP:   return (int)cfg->compression_level;
        case HTTP_CODEC_BROTLI: return (int)cfg->brotli_level;
        case HTTP_CODEC_ZSTD:   return (int)cfg->zstd_level;
        default:                return 0;
    }
}

/* ----- attach / free / opt-out --------------------------------------- */

void http_compression_attach(zend_object *response_obj,
                             http_request_t *request,
                             http_server_config_t *cfg)
{
    if (response_obj == NULL || cfg == NULL) return;
    if (!cfg->compression_enabled) return;

    http_compression_state_t *st = state_of(response_obj);
    if (st == NULL) {
        st = ecalloc(1, sizeof(*st));
        http_response_set_compression_slot(response_obj, st);
    }
    st->request = request;
    st->cfg     = cfg;
}

void http_compression_state_free(zend_object *response_obj)
{
    http_compression_state_t *st = state_of(response_obj);
    if (st == NULL) return;
    if (st->encoder && st->encoder->vt && st->encoder->vt->destroy) {
        st->encoder->vt->destroy(st->encoder);
    }
    if (st->wrapper_ctx) {
        efree(st->wrapper_ctx);
    }
    efree(st);
    http_response_set_compression_slot(response_obj, NULL);
}

void http_compression_mark_no_compression(zend_object *response_obj)
{
    if (response_obj == NULL) return;
    http_compression_state_t *st = state_of(response_obj);
    if (st == NULL) {
        /* Allocate even without attach so the flag persists if attach
         * later discovers an enabled config (rare; safer than dropping). */
        st = ecalloc(1, sizeof(*st));
        http_response_set_compression_slot(response_obj, st);
    }
    st->no_compression = true;
}

/* ----- buffered apply ------------------------------------------------- */

void http_compression_apply_buffered(zend_object *response_obj)
{
    http_compression_state_t *st = state_of(response_obj);
    if (st == NULL || st->applied || st->wrapper_installed) {
        /* st->wrapper_installed: streaming path — body not used. */
        return;
    }

    smart_str *body = http_response_get_body_smart_str(response_obj);
    size_t body_len = (body && body->s) ? ZSTR_LEN(body->s) : 0;

    const http_codec_id_t codec = decide(st, response_obj, body_len);
    st->applied = true;  /* Whether or not we compress, never run twice. */

    if (codec == HTTP_CODEC_IDENTITY || body_len == 0) {
        return;
    }

    const http_encoder_vtable_t *const vt = http_compression_lookup(codec);
    if (UNEXPECTED(vt == NULL)) return;
    http_encoder_t *const enc = vt->create(level_for_codec(st->cfg, codec));
    if (UNEXPECTED(enc == NULL)) return;

    /* Pre-size for the worst-case output: gzip overhead on text is
     * <0.1% + 18-byte header/trailer; on already-compressed input deflate
     * may swell by up to 0.015% + 5 bytes per 32 KiB block. body_len + 64
     * covers the common case in one allocation; NEED_OUTPUT tail-grows
     * on the rare incompressible path. */
    smart_str out = {0};
    smart_str_alloc(&out, body_len + 64, 0);

    const unsigned char *const in = (const unsigned char *)ZSTR_VAL(body->s);
    size_t fed = 0;

    /* write() drain. smart_str_alloc on NEED_OUTPUT instead of every
     * iteration so the common single-pass case is one alloc. */
    while (fed < body_len) {
        size_t avail = out.a - ZSTR_LEN(out.s);
        if (UNEXPECTED(avail < 64)) {
            smart_str_alloc(&out, 4096, 0);
            avail = out.a - ZSTR_LEN(out.s);
        }
        size_t consumed = 0, produced = 0;
        const http_encoder_status_t s = vt->write(enc,
            in + fed, body_len - fed, &consumed,
            ZSTR_VAL(out.s) + ZSTR_LEN(out.s), avail, &produced);
        ZSTR_LEN(out.s) += produced;
        fed             += consumed;
        if (UNEXPECTED(s == HTTP_ENC_ERROR)) {
            vt->destroy(enc);
            smart_str_free(&out);
            return;  /* Leave body as-is; identity wins. */
        }
        /* HTTP_ENC_OK or NEED_OUTPUT: loop continues. */
    }

    /* finish() drain — emits gzip trailer (CRC32 + ISIZE). */
    for (;;) {
        size_t avail = out.a - ZSTR_LEN(out.s);
        if (UNEXPECTED(avail < 32)) {
            smart_str_alloc(&out, 64, 0);
            avail = out.a - ZSTR_LEN(out.s);
        }
        size_t produced = 0;
        const http_encoder_status_t s = vt->finish(enc,
            ZSTR_VAL(out.s) + ZSTR_LEN(out.s), avail, &produced);
        ZSTR_LEN(out.s) += produced;
        if (EXPECTED(s == HTTP_ENC_DONE))   break;
        if (s == HTTP_ENC_NEED_OUTPUT)      continue;
        /* Error mid-finish — abort, keep original body. */
        vt->destroy(enc);
        smart_str_free(&out);
        return;
    }
    vt->destroy(enc);
    smart_str_0(&out);

    /* Swap body — release old, transfer ownership of out's zend_string. */
    smart_str_free(body);
    body->s = out.s;
    body->a = out.a;

    mutate_headers_for_codec(http_response_get_headers(response_obj), codec);
}

/* ----- streaming wrapper --------------------------------------------- */

/* Wrapper context. Stored as the stream_ctx of the compressing ops;
 * holds a back-reference to the response (for header mutation on
 * first chunk) and the original ops/ctx we delegate to. */
typedef struct {
    zend_object                      *response_obj;
    const http_response_stream_ops_t *underlying_ops;
    void                             *underlying_ctx;
    http_encoder_t                   *encoder;
    http_codec_id_t                   codec;
    bool                              first_chunk_done;
} ws_ctx_t;

/* Hand off an accumulated zend_string to the underlying stream ops.
 * One call → one underlying append_chunk → one downstream chunk on the
 * wire. Compared with emitting per-loop slices, this trades a small
 * temporary buffer for fewer protocol-level frames (H2 DATA / chunked
 * size-line). zs is consumed; the underlying owns the refcount. */
static int forward_compressed(ws_ctx_t *const w, zend_string *zs)
{
    if (UNEXPECTED(zs == NULL || ZSTR_LEN(zs) == 0)) {
        if (zs) zend_string_release(zs);
        return HTTP_STREAM_APPEND_OK;
    }
    return w->underlying_ops->append_chunk(w->underlying_ctx, zs);
}

static int ws_append_chunk(void *ctx_opaque, zend_string *chunk)
{
    ws_ctx_t *const w = (ws_ctx_t *)ctx_opaque;

    if (UNEXPECTED(!w->first_chunk_done)) {
        /* Header mutation deferred to first chunk: by now the handler
         * has finalised setHeader/setStatusCode (committed=true was set
         * by HttpResponse::send before we got here), and we know we
         * are actually going to encode at least one byte. */
        mutate_headers_for_codec(
            http_response_get_headers(w->response_obj), w->codec);
        w->first_chunk_done = true;
    }

    /* Accumulate compressed output across all encoder iterations into
     * one zend_string, hand the whole thing to the underlying ops as a
     * single chunk. Avoids one zend_string_init + one append_chunk
     * round-trip per inner pass — relevant for chunked H1 (per-chunk
     * size line + CRLF on the wire) and H2 (one DATA frame per call). */
    const unsigned char *const in = (const unsigned char *)ZSTR_VAL(chunk);
    const size_t in_len = ZSTR_LEN(chunk);
    smart_str out = {0};
    /* Pre-size: gzipped text is typically <50% of source. The estimate
     * reduces realloc churn on the common case; finish() is not called
     * here (mark_ended drains it) so 0-bytes-produced is also valid. */
    smart_str_alloc(&out, in_len + 32, 0);

    size_t fed = 0;
    while (fed < in_len) {
        size_t avail = out.a - ZSTR_LEN(out.s);
        if (UNEXPECTED(avail < 64)) {
            smart_str_alloc(&out, 4096, 0);
            avail = out.a - ZSTR_LEN(out.s);
        }
        size_t consumed = 0, produced = 0;
        const http_encoder_status_t s = w->encoder->vt->write(w->encoder,
            in + fed, in_len - fed, &consumed,
            ZSTR_VAL(out.s) + ZSTR_LEN(out.s), avail, &produced);
        ZSTR_LEN(out.s) += produced;
        fed             += consumed;
        if (UNEXPECTED(s == HTTP_ENC_ERROR)) {
            smart_str_free(&out);
            zend_string_release(chunk);
            return HTTP_STREAM_APPEND_STREAM_DEAD;
        }
        /* HTTP_ENC_OK with all input consumed → loop exits. */
    }
    zend_string_release(chunk);

    if (out.s == NULL || ZSTR_LEN(out.s) == 0) {
        /* Encoder had nothing to flush yet (deflate buffers internally). */
        smart_str_free(&out);
        return HTTP_STREAM_APPEND_OK;
    }
    smart_str_0(&out);
    return forward_compressed(w, out.s);  /* transfers ownership */
}

static void ws_mark_ended(void *ctx_opaque)
{
    ws_ctx_t *const w = (ws_ctx_t *)ctx_opaque;
    /* Drain finish() into the underlying stream. If the handler never
     * sent a single byte we still need to commit headers, encode the
     * empty stream's footer (10-byte gzip header + CRC trailer), and
     * tell the underlying side to terminate. */
    if (UNEXPECTED(!w->first_chunk_done)) {
        mutate_headers_for_codec(
            http_response_get_headers(w->response_obj), w->codec);
        w->first_chunk_done = true;
    }

    /* Same accumulator pattern as append_chunk: build the trailer (and
     * any deflate-buffered bytes finish() emits) into one zend_string
     * and ship as a single underlying chunk. */
    smart_str out = {0};
    smart_str_alloc(&out, 64, 0);
    for (;;) {
        size_t avail = out.a - ZSTR_LEN(out.s);
        if (UNEXPECTED(avail < 32)) {
            smart_str_alloc(&out, 4096, 0);
            avail = out.a - ZSTR_LEN(out.s);
        }
        size_t produced = 0;
        const http_encoder_status_t s = w->encoder->vt->finish(
            w->encoder, ZSTR_VAL(out.s) + ZSTR_LEN(out.s), avail, &produced);
        ZSTR_LEN(out.s) += produced;
        if (EXPECTED(s == HTTP_ENC_DONE))   break;
        if (s == HTTP_ENC_NEED_OUTPUT)      continue;
        break;  /* error — fall through, still close the underlying stream */
    }
    if (out.s != NULL && ZSTR_LEN(out.s) > 0) {
        smart_str_0(&out);
        (void)forward_compressed(w, out.s);  /* transfers ownership */
    } else {
        smart_str_free(&out);
    }
    w->underlying_ops->mark_ended(w->underlying_ctx);
}

static zend_async_event_t *ws_get_wait_event(void *ctx_opaque)
{
    ws_ctx_t *const w = (ws_ctx_t *)ctx_opaque;
    return w->underlying_ops->get_wait_event(w->underlying_ctx);
}

static const http_response_stream_ops_t compressing_stream_ops = {
    .append_chunk   = ws_append_chunk,
    .mark_ended     = ws_mark_ended,
    .get_wait_event = ws_get_wait_event,
};

void http_compression_maybe_install_stream_wrapper(zend_object *response_obj)
{
    http_compression_state_t *st = state_of(response_obj);
    if (st == NULL || st->wrapper_installed) return;

    /* Streaming path: size unknown → pass 0 to skip the threshold check.
     * decide() only ever returns IDENTITY or one of the encodable codecs,
     * so a single inequality is enough to short-circuit. */
    const http_codec_id_t codec = decide(st, response_obj, 0);
    if (codec == HTTP_CODEC_IDENTITY) return;

    const http_encoder_vtable_t *const vt = http_compression_lookup(codec);
    if (UNEXPECTED(vt == NULL)) return;
    http_encoder_t *const enc = vt->create(level_for_codec(st->cfg, codec));
    if (UNEXPECTED(enc == NULL)) return;

    const http_response_stream_ops_t *under_ops =
        http_response_get_stream_ops(response_obj);
    void *under_ctx = http_response_get_stream_ctx(response_obj);
    if (under_ops == NULL) {
        /* No underlying ops to wrap — abort cleanly. */
        vt->destroy(enc);
        return;
    }

    ws_ctx_t *w = ecalloc(1, sizeof(*w));
    w->response_obj    = response_obj;
    w->underlying_ops  = under_ops;
    w->underlying_ctx  = under_ctx;
    w->encoder         = enc;
    w->codec           = codec;
    w->first_chunk_done = false;

    http_response_replace_stream_ops(response_obj, &compressing_stream_ops, w);

    /* Stash on state for cleanup; encoder destroy on object teardown. */
    st->encoder           = enc;
    st->underlying_ops    = under_ops;
    st->underlying_ctx    = under_ctx;
    st->wrapper_ctx       = w;
    st->wrapper_installed = true;
}

#else /* HAVE_HTTP_COMPRESSION not defined: provide stubs so callers compile. */

#include <stddef.h>
struct _zend_object;
struct http_request_t;
struct _http_server_config_t;

void http_compression_attach(struct _zend_object *o, struct http_request_t *r,
                             struct _http_server_config_t *c)
{ (void)o; (void)r; (void)c; }
void http_compression_state_free(struct _zend_object *o) { (void)o; }
void http_compression_mark_no_compression(struct _zend_object *o) { (void)o; }
void http_compression_apply_buffered(struct _zend_object *o) { (void)o; }
void http_compression_maybe_install_stream_wrapper(struct _zend_object *o) { (void)o; }

#endif /* HAVE_HTTP_COMPRESSION */
