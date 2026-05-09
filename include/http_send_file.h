/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef TRUE_ASYNC_HTTP_SEND_FILE_H
#define TRUE_ASYNC_HTTP_SEND_FILE_H

#include "php.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include <stdbool.h>
#include <stdint.h>

struct http_request_t;
typedef struct http_request_t http_request_t;

/* C-side snapshot of TrueAsync\SendFileOptions. Strings are addref'd
 * zend_string borrows; release via http_send_file_options_destroy. */
typedef struct {
	zend_string *content_type;     /* NULL = derive from MIME */
	zend_string *download_name;    /* NULL = no Content-Disposition filename */
	zend_string *cache_control;    /* NULL = no Cache-Control */
	int          status;           /* 0 = default 200/206/304 selection */
	uint8_t      disposition;      /* 0 = inline, 1 = attachment */
	bool         disposition_set;  /* user explicitly chose */
	bool         etag;
	bool         last_modified;
	bool         accept_ranges;
	bool         precompressed;
	bool         conditional;
	bool         delete_after_send;
} http_send_file_options_t;

#define HTTP_SEND_FILE_DISPOSITION_INLINE     0
#define HTTP_SEND_FILE_DISPOSITION_ATTACHMENT 1

typedef struct {
	zend_string             *path;
	http_send_file_options_t opts;
} http_send_file_request_t;

/* Class entries — populated in MINIT. */
extern zend_class_entry *http_send_file_options_ce;
extern zend_class_entry *http_send_file_disposition_ce;

void http_send_file_options_class_register(void);

/* Read the readonly properties off a SendFileOptions zend_object into a
 * C snapshot. NULL obj → fill defaults. Refcounts strings on out. */
void http_send_file_options_snapshot(zend_object *obj,
                                     http_send_file_options_t *out);

/* Release every refcounted field on the snapshot. NULL-safe (no-op). */
void http_send_file_options_destroy(http_send_file_options_t *opts);

/* Free the descriptor + path + options. emalloc'd by sendFile(). */
void http_send_file_request_free(http_send_file_request_t *req);

/* Kick off the sendfile FSM for a response that has a pending sendFile
 * descriptor. Owns `req` (frees it via the FSM). The protocol op
 * (response_obj's send_static_response slot) drives the actual writes;
 * on_done fires exactly once, after which the caller continues
 * with its post-request bookkeeping (counters, finalize, keep-alive).
 *
 * Returns true on synchronous arm — caller waits for on_done. False
 * means setup failed BEFORE the FSM took ownership; req has been
 * freed and the response was populated with a 500 inline body so the
 * caller's regular dispose flush can still emit it. */
bool http_send_file_dispatch(http_request_t *request,
                             zend_object *response_obj,
                             http_send_file_request_t *req,
                             void (*on_done)(void *user, int status),
                             void *user);

#endif /* TRUE_ASYNC_HTTP_SEND_FILE_H */
