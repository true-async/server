/*
 * Minimal HTTP/3 client harness for php-http-server tests.
 *
 * Usage: h3client <host> <port> <path> [<method>] [<body-file>]
 *
 * Performs one HTTPS request over QUIC, prints the response status to
 * stderr (line "STATUS=<code>") and the response body to stdout, then
 * exits 0 on success / non-zero on protocol or transport failure.
 *
 * Built only by the tests' SKIPIF block — host curl is not linked
 * against ngtcp2/nghttp3 in the project's pinned stack, so we ship a
 * ~300 LoC client of our own. Missing on purpose: 0-RTT, connection
 * migration, retransmit-bookkeeping niceties, multiple in-flight
 * streams. Single GET / single POST / one shot is enough to validate
 * Step 4 end-to-end.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <nghttp3/nghttp3.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#define MAX_DATAGRAM 1500

static const uint8_t ALPN_H3[] = { 2, 'h', '3' };

typedef struct {
    int                          fd;
    struct sockaddr_storage      remote;
    socklen_t                    remote_len;
    struct sockaddr_storage      local;
    socklen_t                    local_len;

    SSL_CTX                     *ssl_ctx;
    SSL                         *ssl;
    ngtcp2_crypto_ossl_ctx      *octx;
    ngtcp2_crypto_conn_ref       conn_ref;

    ngtcp2_conn                 *qc;
    nghttp3_conn                *h3;
    int64_t                      stream_id;

    int                          response_status;
    char                        *response_body;
    size_t                       response_body_len;
    size_t                       response_body_cap;
    /* Count of non-status response headers received on the current
     * stream. Phpt 116 needs this to verify the single-pass-emit
     * overflow path in submit_response (>32 nv triggers heap promotion,
     * >64 triggers realloc-doubling). Reset between requests in
     * multi-request mode. */
    unsigned long                response_header_count;
    bool                         response_done;
    bool                         h3_streams_bound;

    /* For POST. */
    const uint8_t               *req_body;
    size_t                       req_body_len;
    size_t                       req_body_offset;
} h3c_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static ngtcp2_conn *get_conn_from_ref(ngtcp2_crypto_conn_ref *ref) {
    return ((h3c_t *)ref->user_data)->qc;
}

static void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *ctx) {
    (void)ctx;
    if (RAND_bytes(dest, (int)destlen) != 1) {
        fprintf(stderr, "h3client: RAND_bytes failed\n");
        abort();
    }
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                    uint8_t *token, size_t cidlen,
                                    void *user_data) {
    (void)conn; (void)user_data;
    if (RAND_bytes(cid->data, (int)cidlen) != 1) return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

/* ----- nghttp3 user callbacks ----- */

static int h3_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                          nghttp3_rcbuf *name, nghttp3_rcbuf *value,
                          uint8_t flags, void *cu, void *su) {
    (void)conn; (void)stream_id; (void)flags; (void)su;
    h3c_t *c = cu;
    if (token == NGHTTP3_QPACK_TOKEN__STATUS) {
        nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
        char buf[16] = {0};
        size_t n = v.len < sizeof(buf) - 1 ? v.len : sizeof(buf) - 1;
        memcpy(buf, v.base, n);
        c->response_status = atoi(buf);
    } else {
        c->response_header_count++;
    }
    (void)name;
    return 0;
}

static int h3_recv_data(nghttp3_conn *conn, int64_t stream_id,
                        const uint8_t *data, size_t datalen,
                        void *cu, void *su) {
    (void)conn; (void)stream_id; (void)su;
    h3c_t *c = cu;
    if (c->response_body_len + datalen > c->response_body_cap) {
        size_t new_cap = c->response_body_cap == 0 ? 4096 : c->response_body_cap * 2;
        while (new_cap < c->response_body_len + datalen) new_cap *= 2;
        char *nb = realloc(c->response_body, new_cap);
        if (!nb) return NGHTTP3_ERR_CALLBACK_FAILURE;
        c->response_body = nb;
        c->response_body_cap = new_cap;
    }
    memcpy(c->response_body + c->response_body_len, data, datalen);
    c->response_body_len += datalen;
    return 0;
}

static int h3_end_stream(nghttp3_conn *conn, int64_t stream_id,
                         void *cu, void *su) {
    (void)conn; (void)su;
    h3c_t *c = cu;
    if (stream_id == c->stream_id) c->response_done = true;
    return 0;
}

static int h3_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t err, void *cu, void *su) {
    (void)conn; (void)stream_id; (void)err; (void)cu; (void)su;
    return 0;
}

static int h3_reset_stream(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t err, void *cu, void *su) {
    (void)conn; (void)stream_id; (void)err; (void)cu; (void)su;
    return 0;
}

static int h3_stream_close(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t err, void *cu, void *su) {
    (void)conn; (void)err; (void)su;
    h3c_t *c = cu;
    if (stream_id == c->stream_id) c->response_done = true;
    return 0;
}

/* nghttp3 data_reader for POST body. */
static nghttp3_ssize req_body_reader(nghttp3_conn *conn, int64_t stream_id,
                                     nghttp3_vec *vec, size_t veccnt,
                                     uint32_t *pflags, void *cu, void *su) {
    (void)conn; (void)stream_id; (void)veccnt; (void)su;
    h3c_t *c = cu;
    size_t left = c->req_body_len - c->req_body_offset;
    if (left == 0) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    vec[0].base = (uint8_t *)c->req_body + c->req_body_offset;
    vec[0].len  = left;
    c->req_body_offset = c->req_body_len;
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

/* ----- ngtcp2 ↔ nghttp3 bridge ----- */

static int recv_stream_data_cb(ngtcp2_conn *qc, uint32_t flags,
                               int64_t stream_id, uint64_t off,
                               const uint8_t *data, size_t datalen,
                               void *user_data, void *stream_user_data) {
    (void)qc; (void)off; (void)stream_user_data;
    h3c_t *c = user_data;
    if (!c->h3) return 0;
    int fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0;
    nghttp3_ssize n = nghttp3_conn_read_stream(c->h3, stream_id, data, datalen, fin);
    if (n < 0) return NGTCP2_ERR_CALLBACK_FAILURE;
    ngtcp2_conn_extend_max_stream_offset(c->qc, stream_id, (uint64_t)n);
    ngtcp2_conn_extend_max_offset(c->qc, (uint64_t)n);
    return 0;
}

static int acked_stream_data_offset_cb(ngtcp2_conn *qc, int64_t stream_id,
                                       uint64_t off, uint64_t datalen,
                                       void *user_data, void *stream_user_data) {
    (void)qc; (void)off; (void)stream_user_data;
    h3c_t *c = user_data;
    if (c->h3) nghttp3_conn_add_ack_offset(c->h3, stream_id, datalen);
    return 0;
}

static int stream_close_cb(ngtcp2_conn *qc, uint32_t flags, int64_t stream_id,
                           uint64_t err, void *user_data, void *stream_user_data) {
    (void)qc; (void)flags; (void)stream_user_data;
    h3c_t *c = user_data;
    if (c->h3) nghttp3_conn_close_stream(c->h3, stream_id,
        (flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) ? err : NGHTTP3_H3_NO_ERROR);
    return 0;
}

static int stream_reset_cb(ngtcp2_conn *qc, int64_t stream_id, uint64_t fs,
                           uint64_t err, void *user_data, void *stream_user_data) {
    (void)qc; (void)fs; (void)err; (void)stream_user_data;
    h3c_t *c = user_data;
    if (c->h3) nghttp3_conn_shutdown_stream_read(c->h3, stream_id);
    return 0;
}

static int extend_max_local_streams_bidi_cb(ngtcp2_conn *qc, uint64_t max,
                                            void *user_data) {
    (void)qc; (void)max; (void)user_data;
    return 0;
}

/* ----- setup helpers ----- */

static const nghttp3_callbacks H3_CB;   /* tentative — defined below */

static bool init_h3(h3c_t *c) {
    nghttp3_settings s; nghttp3_settings_default(&s);
    if (nghttp3_conn_client_new(&c->h3, &H3_CB, &s, nghttp3_mem_default(), c) != 0)
        return false;

    int64_t ctrl, qenc, qdec;
    if (ngtcp2_conn_open_uni_stream(c->qc, &ctrl, NULL) != 0
     || ngtcp2_conn_open_uni_stream(c->qc, &qenc, NULL) != 0
     || ngtcp2_conn_open_uni_stream(c->qc, &qdec, NULL) != 0) return false;
    if (nghttp3_conn_bind_control_stream(c->h3, ctrl) != 0
     || nghttp3_conn_bind_qpack_streams(c->h3, qenc, qdec) != 0) return false;
    c->h3_streams_bound = true;
    return true;
}

static int handshake_completed_cb(ngtcp2_conn *qc, void *user_data) {
    (void)qc;
    h3c_t *c = user_data;
    if (!c->h3) return init_h3(c) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

static const ngtcp2_callbacks CALLBACKS = {
    .client_initial          = ngtcp2_crypto_client_initial_cb,
    .recv_crypto_data        = ngtcp2_crypto_recv_crypto_data_cb,
    .encrypt                 = ngtcp2_crypto_encrypt_cb,
    .decrypt                 = ngtcp2_crypto_decrypt_cb,
    .hp_mask                 = ngtcp2_crypto_hp_mask_cb,
    .recv_retry              = ngtcp2_crypto_recv_retry_cb,
    .rand                    = rand_cb,
    .get_new_connection_id   = get_new_connection_id_cb,
    .update_key              = ngtcp2_crypto_update_key_cb,
    .delete_crypto_aead_ctx  = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
    .delete_crypto_cipher_ctx= ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
    .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
    .version_negotiation     = ngtcp2_crypto_version_negotiation_cb,
    .handshake_completed     = handshake_completed_cb,
    .recv_stream_data        = recv_stream_data_cb,
    .acked_stream_data_offset= acked_stream_data_offset_cb,
    .stream_close            = stream_close_cb,
    .stream_reset            = stream_reset_cb,
    .extend_max_local_streams_bidi = extend_max_local_streams_bidi_cb,
};

static const nghttp3_callbacks H3_CB = {
    .stream_close = h3_stream_close,
    .recv_data    = h3_recv_data,
    .stop_sending = h3_stop_sending,
    .end_stream   = h3_end_stream,
    .reset_stream = h3_reset_stream,
    .recv_header  = h3_recv_header,
};

/* Submit the GET/POST request. Called from main once nghttp3 is up.
 * `bloat_kib` >0 appends a single oversized custom header of that size
 * (filled with 'A') to drive the server-side HTTP3_MAX_HEADERS_BYTES
 * cap regression test (118). 0 = normal request. */
static bool submit_request(h3c_t *c, const char *method, const char *host,
                           const char *path, bool has_body,
                           unsigned long bloat_kib) {
    if (ngtcp2_conn_open_bidi_stream(c->qc, &c->stream_id, NULL) != 0)
        return false;

    nghttp3_nv base[5] = {
        { .name = (uint8_t *)":method",    .namelen = 7,  .value = (uint8_t *)method, .valuelen = strlen(method) },
        { .name = (uint8_t *)":scheme",    .namelen = 7,  .value = (uint8_t *)"https", .valuelen = 5 },
        { .name = (uint8_t *)":authority", .namelen = 10, .value = (uint8_t *)host, .valuelen = strlen(host) },
        { .name = (uint8_t *)":path",      .namelen = 5,  .value = (uint8_t *)path, .valuelen = strlen(path) },
        { .name = (uint8_t *)"user-agent", .namelen = 10, .value = (uint8_t *)"h3client/test", .valuelen = 13 },
    };

    nghttp3_data_reader dr = { .read_data = req_body_reader };

    if (bloat_kib == 0) {
        return nghttp3_conn_submit_request(c->h3, c->stream_id, base, 5,
            has_body ? &dr : NULL, NULL) == 0;
    }

    /* Spread the bloat across many 1-KiB headers rather than one giant
     * value: nghttp3 + ngtcp2 stop accepting a single header field
     * around the ~70 KiB mark on this stack regardless of FC, so a
     * single huge value never reaches the server-side cap check. Many
     * small fields hit the cumulative cap while every individual entry
     * stays well within nghttp3's per-field limits. The server's cap
     * counts (name + value + 32) per entry, so one entry per KiB ≈ N
     * KiB of accounting + ~3 KiB header pseudo-headers. */
    size_t n_extra = bloat_kib;
    if (n_extra > 1024) n_extra = 1024;
    size_t total_nv = 5 + n_extra;
    nghttp3_nv *nv = calloc(total_nv, sizeof(nghttp3_nv));
    if (nv == NULL) return false;
    memcpy(nv, base, sizeof(base));

    /* Each x-bloat-NNNN header carries a 1-KiB 'A' value. Names are
     * stack-encoded; values share one buffer (nghttp3 copies on submit
     * by default). */
    static char value_blob[1024];
    memset(value_blob, 'A', sizeof(value_blob));
    char *names = malloc(n_extra * 16);
    if (names == NULL) { free(nv); return false; }

    for (size_t i = 0; i < n_extra; i++) {
        char *nm = names + i * 16;
        int nlen = snprintf(nm, 16, "x-bloat-%04zu", i);
        nv[5 + i].name     = (uint8_t *)nm;
        nv[5 + i].namelen  = (size_t)nlen;
        nv[5 + i].value    = (uint8_t *)value_blob;
        nv[5 + i].valuelen = sizeof(value_blob);
        nv[5 + i].flags    = NGHTTP3_NV_FLAG_NONE;
    }
    bool ok = nghttp3_conn_submit_request(c->h3, c->stream_id, nv, total_nv,
        has_body ? &dr : NULL, NULL) == 0;
    free(names);
    free(nv);
    return ok;
}

/* Drain outbound packets through ngtcp2 + sendto. */
static int drain_out(h3c_t *c) {
    uint8_t buf[MAX_DATAGRAM];
    for (;;) {
        int64_t  hsid = -1; int hfin = 0;
        nghttp3_vec hvec[16]; nghttp3_ssize hcnt = 0;
        if (c->h3) {
            hcnt = nghttp3_conn_writev_stream(c->h3, &hsid, &hfin, hvec, 16);
            if (hcnt < 0) hcnt = 0;
        }
        ngtcp2_path_storage ps = {0};
        ngtcp2_path_storage_init(&ps,
            (struct sockaddr *)&c->local, c->local_len,
            (struct sockaddr *)&c->remote, c->remote_len, NULL);
        ngtcp2_pkt_info pi = {0};
        ngtcp2_ssize pdatalen = 0;
        /* WRITE_MORE only meaningful when we have stream data to feed; on
         * the bare handshake path (hsid=-1) it just spins. */
        uint32_t fl = (hsid >= 0)
            ? (NGTCP2_WRITE_STREAM_FLAG_MORE | (hfin ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0))
            : 0;
        ngtcp2_ssize n = ngtcp2_conn_writev_stream(c->qc, &ps.path, &pi,
            buf, sizeof(buf), &pdatalen, fl, hsid,
            (const ngtcp2_vec *)hvec, (size_t)hcnt, now_ns());
        if (n == NGTCP2_ERR_WRITE_MORE) {
            if (c->h3 && pdatalen > 0)
                nghttp3_conn_add_write_offset(c->h3, hsid, (size_t)pdatalen);
            continue;
        }
        if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED || n == NGTCP2_ERR_STREAM_SHUT_WR) {
            if (c->h3) nghttp3_conn_block_stream(c->h3, hsid);
            continue;
        }
        if (n == 0) {
            if (c->h3 && pdatalen > 0) {
                nghttp3_conn_add_write_offset(c->h3, hsid, (size_t)pdatalen);
                continue;
            }
            return 0;
        }
        if (n < 0) return -1;
        if (sendto(c->fd, buf, (size_t)n, 0,
                   (struct sockaddr *)&c->remote, c->remote_len) < 0) {
            return -1;
        }
        if (c->h3 && pdatalen > 0)
            nghttp3_conn_add_write_offset(c->h3, hsid, (size_t)pdatalen);
    }
}

/* ----- main ----- */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> <path> [<method> [<body-file>]]\n", argv[0]);
        return 2;
    }
    const char *host   = argv[1];
    const char *port   = argv[2];
    const char *path   = argv[3];
    const char *method = (argc > 4) ? argv[4] : "GET";
    const char *body_path = (argc > 5) ? argv[5] : NULL;

    h3c_t c = {0};
    c.stream_id = -1;

    /* Optional body file. */
    if (body_path) {
        FILE *fp = fopen(body_path, "rb");
        if (!fp) { perror("fopen body"); return 2; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        c.req_body = malloc((size_t)sz);
        c.req_body_len = (size_t)sz;
        if (fread((void *)c.req_body, 1, (size_t)sz, fp) != (size_t)sz) {
            fprintf(stderr, "body read short\n"); return 2;
        }
        fclose(fp);
    }

    /* Resolve host:port. */
    struct addrinfo hints = {0}, *ai;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0) {
        fprintf(stderr, "h3client: getaddrinfo failed\n"); return 2;
    }
    memcpy(&c.remote, ai->ai_addr, ai->ai_addrlen);
    c.remote_len = ai->ai_addrlen;
    freeaddrinfo(ai);

    c.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (c.fd < 0) { perror("socket"); return 2; }
    /* Bind ephemeral so getsockname returns a valid local addr. */
    struct sockaddr_in zero = { .sin_family = AF_INET };
    if (bind(c.fd, (struct sockaddr *)&zero, sizeof(zero)) < 0) {
        perror("bind"); return 2;
    }
    c.local_len = sizeof(c.local);
    if (getsockname(c.fd, (struct sockaddr *)&c.local, &c.local_len) < 0) {
        perror("getsockname"); return 2;
    }

    /* OpenSSL setup. */
    SSL_library_init();
    if (ngtcp2_crypto_ossl_init() != 0) {
        fprintf(stderr, "ngtcp2_crypto_ossl_init failed\n"); return 2;
    }
    c.ssl_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(c.ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(c.ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(c.ssl_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_alpn_protos(c.ssl_ctx, ALPN_H3, sizeof(ALPN_H3));

    c.ssl = SSL_new(c.ssl_ctx);
    if (ngtcp2_crypto_ossl_configure_client_session(c.ssl) != 0) {
        fprintf(stderr, "ngtcp2_crypto_ossl_configure_client_session failed\n");
        return 2;
    }
    SSL_set_tlsext_host_name(c.ssl, host);
    c.conn_ref.get_conn  = get_conn_from_ref;
    c.conn_ref.user_data = &c;
    SSL_set_app_data(c.ssl, &c.conn_ref);
    SSL_set_connect_state(c.ssl);
    if (ngtcp2_crypto_ossl_ctx_new(&c.octx, c.ssl) != 0) {
        fprintf(stderr, "ngtcp2_crypto_ossl_ctx_new failed\n");
        return 2;
    }

    /* ngtcp2 client setup. */
    ngtcp2_cid scid, dcid;
    scid.datalen = 8; if (RAND_bytes(scid.data, 8) != 1) return 2;
    dcid.datalen = 8; if (RAND_bytes(dcid.data, 8) != 1) return 2;

    ngtcp2_path qpath = {
        .local  = { .addr = (struct sockaddr *)&c.local,  .addrlen = c.local_len },
        .remote = { .addr = (struct sockaddr *)&c.remote, .addrlen = c.remote_len },
    };
    ngtcp2_settings settings; ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ns();
    ngtcp2_transport_params tp; ngtcp2_transport_params_default(&tp);
    tp.initial_max_streams_bidi = 1000;
    tp.initial_max_streams_uni  = 100;
    /* Generous flow-control caps so multi-request bench mode (one
     * connection × thousands of large-body requests) doesn't stall
     * waiting for MAX_DATA. The single-shot phpt path never gets close
     * to these. */
    tp.initial_max_data         = 256 * 1024 * 1024;
    tp.initial_max_stream_data_bidi_local  = 1 * 1024 * 1024;
    tp.initial_max_stream_data_bidi_remote = 1 * 1024 * 1024;
    tp.initial_max_stream_data_uni         = 256 * 1024;
    tp.max_idle_timeout         = 5 * NGTCP2_SECONDS;
    tp.active_connection_id_limit = 7;

    if (ngtcp2_conn_client_new(&c.qc, &dcid, &scid, &qpath,
            NGTCP2_PROTO_VER_V1, &CALLBACKS, &settings, &tp,
            NULL, &c) != 0) {
        fprintf(stderr, "ngtcp2_conn_client_new failed\n"); return 2;
    }
    ngtcp2_conn_set_tls_native_handle(c.qc, c.octx);

    /* Default 5s deadline. H3CLIENT_DEADLINE_MS overrides — useful for
     * tests that expect a server-side reject (Step 6f budget) and
     * don't want to wait the full default timeout. */
    uint64_t deadline_ms = 5000;
    {
        const char *env = getenv("H3CLIENT_DEADLINE_MS");
        if (env != NULL && *env != '\0') {
            char *end = NULL;
            unsigned long n = strtoul(env, &end, 10);
            if (end != env && *end == '\0' && n > 0 && n <= 60000) {
                deadline_ms = n;
            }
        }
    }

    /* Multi-request mode (benchmark). H3CLIENT_REQUEST_COUNT=N reuses
     * the QUIC connection for N sequential requests over fresh bidi
     * streams. Default 1 preserves single-shot behaviour required by the
     * phpt suite. H3CLIENT_QUIET=1 suppresses the per-request body dump
     * + STATUS line so a 100k-iter run doesn't drown the terminal. */
    unsigned long request_count = 1;
    {
        const char *env = getenv("H3CLIENT_REQUEST_COUNT");
        if (env != NULL && *env != '\0') {
            char *end = NULL;
            unsigned long n = strtoul(env, &end, 10);
            if (end != env && *end == '\0' && n > 0 && n <= 10000000ul) {
                request_count = n;
            }
        }
    }
    bool quiet = (getenv("H3CLIENT_QUIET") != NULL);
    /* Opt-in: emit `HEADERS=N` (count of non-status response headers)
     * to stderr after each STATUS= line. Off by default so the seven
     * existing phpts that grep for STATUS= and strip it from the body
     * stream don't regress. Phpt 116 enables this to verify the
     * single-pass-emit overflow path. */
    bool verbose_headers = (getenv("H3CLIENT_VERBOSE_HEADERS") != NULL);

    /* H3CLIENT_BLOAT_HEADER_KIB=N — add a single x-bloat header of N
     * KiB to the FIRST request so phpt 118 can drive the server's
     * HTTP3_MAX_HEADERS_BYTES (256 KiB) cap. Only first request: lets
     * a single connection prove "stream rejected, conn survives" by
     * issuing a normal request right after. */
    unsigned long bloat_kib = 0;
    {
        const char *env = getenv("H3CLIENT_BLOAT_HEADER_KIB");
        if (env != NULL && *env != '\0') {
            char *end = NULL;
            unsigned long n = strtoul(env, &end, 10);
            if (end != env && *end == '\0' && n <= 4096ul) {
                bloat_kib = n;
            }
        }
    }

    unsigned long completed = 0;
    bool sent = false;
    uint64_t deadline_ns = now_ns() + deadline_ms * 1000000ull;

    while (completed < request_count && now_ns() < deadline_ns) {
        if (drain_out(&c) < 0) {
            fprintf(stderr, "drain_out fail\n"); return 1;
        }

        /* Submit a request once nghttp3 is bound. After the first one,
         * we re-enter this branch each time the previous response
         * finishes — `sent` is cleared in the response-done path below. */
        if (c.h3_streams_bound && !sent) {
            unsigned long this_bloat = (completed == 0) ? bloat_kib : 0;
            if (!submit_request(&c, method, host, path, c.req_body_len > 0,
                                this_bloat)) {
                fprintf(stderr, "submit_request failed\n"); return 1;
            }
            sent = true;
            c.req_body_offset = 0;  /* re-read body for next request */
            continue;
        }

        /* If the current request finished, snapshot it, reset per-request
         * state, and either move to the next request or break out. */
        if (c.response_done) {
            if (!quiet) {
                fprintf(stderr, "STATUS=%d\n", c.response_status);
                if (verbose_headers) {
                    fprintf(stderr, "HEADERS=%lu\n", c.response_header_count);
                }
                if (c.response_body_len > 0) {
                    fwrite(c.response_body, 1, c.response_body_len, stdout);
                }
                fflush(stdout);
            }
            completed++;
            if (completed >= request_count) break;

            /* Reset per-request state — keep the connection + h3 conn. */
            c.response_status = 0;
            c.response_header_count = 0;
            c.response_body_len = 0;
            c.response_done = false;
            c.stream_id = -1;
            sent = false;
            /* Re-arm deadline so a long batch doesn't trip the 5s cap. */
            deadline_ns = now_ns() + deadline_ms * 1000000ull;
            continue;
        }

        /* Wait for a packet or expiry, whichever is sooner. */
        ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(c.qc);
        int timeout_ms = 100;
        if (expiry != UINT64_MAX) {
            uint64_t now = now_ns();
            if (expiry > now) {
                uint64_t delay = (expiry - now) / 1000000ull;
                if (delay < (uint64_t)timeout_ms) timeout_ms = (int)delay;
            } else timeout_ms = 0;
        }
        struct pollfd pfd = { .fd = c.fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) { if (errno == EINTR) continue; perror("poll"); return 1; }

        if (pr == 0) {
            /* Timer expiry. */
            ngtcp2_conn_handle_expiry(c.qc, now_ns());
            continue;
        }
        uint8_t rbuf[2048];
        struct sockaddr_storage src; socklen_t srclen = sizeof(src);
        ssize_t r = recvfrom(c.fd, rbuf, sizeof(rbuf), 0,
                             (struct sockaddr *)&src, &srclen);
        if (r < 0) { if (errno == EINTR) continue; perror("recvfrom"); return 1; }

        ngtcp2_path_storage ps = {0};
        ngtcp2_path_storage_init(&ps,
            (struct sockaddr *)&c.local,  c.local_len,
            (struct sockaddr *)&src,      srclen, NULL);
        ngtcp2_pkt_info pi = {0};
        int rv = ngtcp2_conn_read_pkt(c.qc, &ps.path, &pi, rbuf, (size_t)r, now_ns());
        if (rv != 0) {
            if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_CLOSING) {
                break;
            }
            fprintf(stderr, "h3client: read_pkt rv=%d\n", rv);
            return 1;
        }
    }

    if (completed < request_count) {
        fprintf(stderr, "h3client: timeout (completed=%lu of %lu)\n",
                completed, request_count);
        return 1;
    }

    if (request_count > 1) {
        fprintf(stderr, "COMPLETED=%lu\n", completed);
    }

    /* Step 6a — emit a graceful application-error CONNECTION_CLOSE so
     * the server-side reaper has something to react to (otherwise the
     * conn would only be torn down by the 30s idle timer). Best-effort:
     * any failure here just leaves the server's idle timer to clean up.
     *
     * `H3CLIENT_NO_CLOSE=1` skips the emit so test harnesses can drive
     * the server's idle-timeout path (Step 6e). */
    if (getenv("H3CLIENT_NO_CLOSE") == NULL) {
        ngtcp2_ccerr ccerr;
        ngtcp2_ccerr_set_application_error(&ccerr, /* H3_NO_ERROR */ 0x100, NULL, 0);
        ngtcp2_path_storage ps2 = {0};
        ngtcp2_path_storage_init(&ps2,
            (struct sockaddr *)&c.local,  c.local_len,
            (struct sockaddr *)&c.remote, c.remote_len, NULL);
        ngtcp2_pkt_info pi2 = {0};
        uint8_t cbuf[1280];
        ngtcp2_ssize cn = ngtcp2_conn_write_connection_close(
            c.qc, &ps2.path, &pi2, cbuf, sizeof(cbuf), &ccerr, now_ns());
        if (cn > 0) {
            (void)sendto(c.fd, cbuf, (size_t)cn, 0,
                         (struct sockaddr *)&c.remote, c.remote_len);
        }
    }
    return 0;
}
