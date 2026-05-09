/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* HttpResponse::sendFile() — thin adapter on top of the unified
 * src/send_file.c engine. Resolves a precompressed sidecar (if the
 * caller opted in), builds a Content-Disposition string from
 * SendFileOptions, fills the engine config, and hands off. The engine
 * drives fs_open → fstat → headers → protocol op → finalize. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "Zend/zend_async_API.h"
#include "php_http_server.h"
#include "http1/http_parser.h"
#include "http_response_internal.h"
#include "http_send_file.h"
#include "send_file.h"
#include "http_rfc5987.h"
#include "compression/http_compression_negotiate.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>

/* Per-call adapter state — bridges http_send_file_request_t /
 * http_send_file_dispatch contract to send_file_cbs_t. Owned by the
 * engine for the lifetime of the async chain. */
typedef struct
{
	http_send_file_request_t *req;	/* owned, freed on done */
	zend_string *content_disposition; /* owned (may be NULL) */
	void (*on_done)(void *user, int status);
	void *user;
} sf_adapter_t;

static void sf_adapter_free(sf_adapter_t *a)
{
	if (a == NULL) {
		return;
	}

	if (a->req != NULL) {
		http_send_file_request_free(a->req);
	}

	if (a->content_disposition != NULL) {
		zend_string_release(a->content_disposition);
	}

	efree(a);
}

static void sf_adapter_on_done(void *user, int status)
{
	sf_adapter_t *a = (sf_adapter_t *)user;
	void (*on_done)(void *, int) = a->on_done;
	void *outer_user = a->user;
	sf_adapter_free(a);

	if (on_done != NULL) {
		on_done(outer_user, status);
	}
}

/* sendFile is one-shot; multi-protocol keep-alive matters only for
 * H1 and H1 is the multiplex-default. Mirror the pre-extraction
 * behaviour: always advertise keep-alive=true. */
static bool sf_adapter_keep_alive(void *user)
{
	(void)user;
	return true;
}

/* Best-effort RFC 5987 "filename*=UTF-8''..." encode. ASCII-clean
 * names also get a plain filename="" form so legacy clients work.
 * Returns NULL when the option set has nothing to emit. */
static zend_string *sf_build_content_disposition(const http_send_file_options_t *opts)
{
	const bool is_attachment = (opts->disposition == HTTP_SEND_FILE_DISPOSITION_ATTACHMENT) ||
							   (opts->download_name != NULL && !opts->disposition_set);

	if (!is_attachment && opts->download_name == NULL) {
		return NULL;
	}

	smart_str s = {0};
	smart_str_appends(&s, is_attachment ? "attachment" : "inline");

	if (opts->download_name != NULL) {
		const char *dn = ZSTR_VAL(opts->download_name);
		const size_t dn_len = ZSTR_LEN(opts->download_name);

		bool ascii_clean = true;
		for (size_t i = 0; i < dn_len; i++) {
			const unsigned char c = (unsigned char)dn[i];

			if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
				ascii_clean = false;
				break;
			}
		}

		if (ascii_clean) {
			smart_str_appendl(&s, "; filename=\"", 12);
			smart_str_appendl(&s, dn, dn_len);
			smart_str_appendc(&s, '"');
		} else {
			smart_str_appendl(&s, "; filename*=UTF-8''", 19);
			http_rfc5987_encode(&s, dn, dn_len);
		}
	}

	smart_str_0(&s);
	zend_string *out = zend_string_init(ZSTR_VAL(s.s), ZSTR_LEN(s.s), 0);
	smart_str_free(&s);
	return out;
}

/* Precompressed sidecar selection. When the caller opted in via
 * setPrecompressed and Accept-Encoding accepts at least one of the
 * supported codings, scan for the sibling and (on hit) rewrite
 * `*path_buf` to the sidecar path, emitting the encoding token via
 * `*out_encoding` (literal owned by the codec table). On miss the
 * buffer is left untouched. */
static bool sf_try_precompressed(http_request_t *request, char *path_buf, size_t buf_cap,
								 size_t *path_len, const char **out_encoding,
								 size_t *out_encoding_len)
{
	if (request == NULL || request->headers == NULL) {
		return false;
	}

	const zval *ae_zv = zend_hash_str_find(request->headers, "accept-encoding", 15);

	if (ae_zv == NULL) {
		return false;
	}

	const zend_string *ae = NULL;

	if (Z_TYPE_P(ae_zv) == IS_STRING) {
		ae = Z_STR_P(ae_zv);
	} else if (Z_TYPE_P(ae_zv) == IS_ARRAY) {
		const zval *first = zend_hash_index_find(Z_ARRVAL_P(ae_zv), 0);

		if (first != NULL && Z_TYPE_P(first) == IS_STRING) {
			ae = Z_STR_P(first);
		}
	}

	if (ae == NULL) {
		return false;
	}

	http_accept_encoding_t parsed;
	http_accept_encoding_parse(ZSTR_VAL(ae), ZSTR_LEN(ae), &parsed);

	static const struct
	{
		const char *suffix;
		size_t suffix_len;
		const char *token;
		size_t token_len;
	} codecs[] = {
		{".zst", 4, "zstd", 4},
		{".br", 3, "br", 2},
		{".gz", 3, "gzip", 4},
	};
	const bool acceptable[] = {
		parsed.zstd_acceptable,
		parsed.brotli_acceptable,
		parsed.gzip_acceptable,
	};

	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		if (!acceptable[i]) {
			continue;
		}

		if (*path_len + codecs[i].suffix_len + 1 > buf_cap) {
			continue;
		}

		char candidate[MAXPATHLEN];
		memcpy(candidate, path_buf, *path_len);
		memcpy(candidate + *path_len, codecs[i].suffix, codecs[i].suffix_len);
		candidate[*path_len + codecs[i].suffix_len] = '\0';

		struct stat st;

		if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}

		memcpy(path_buf + *path_len, codecs[i].suffix, codecs[i].suffix_len);
		*path_len += codecs[i].suffix_len;
		path_buf[*path_len] = '\0';
		*out_encoding = codecs[i].token;
		*out_encoding_len = codecs[i].token_len;
		return true;
	}

	return false;
}

bool http_send_file_dispatch(http_request_t *request, zend_object *response_obj,
							 http_send_file_request_t *req,
							 void (*on_done)(void *user, int status), void *user)
{
	if (UNEXPECTED(req == NULL || response_obj == NULL || req->path == NULL)) {
		if (req != NULL) {
			http_send_file_request_free(req);
		}

		http_response_synth_error(response_obj, 500, "sendFile: invalid arguments");
		return false;
	}

	const size_t path_len = ZSTR_LEN(req->path);

	if (UNEXPECTED(path_len == 0 || path_len + 4 + 1 >= MAXPATHLEN ||
				   ZSTR_VAL(req->path)[0] != '/')) {
		http_send_file_request_free(req);
		http_response_synth_error(response_obj, 500, "sendFile: path must be absolute");
		return false;
	}

	/* Engine refuses to drive without a protocol op — synthesize 500. */
	const http_response_stream_ops_t *ops = http_response_get_stream_ops(response_obj);

	if (UNEXPECTED(ops == NULL || ops->send_static_response == NULL)) {
		http_send_file_request_free(req);
		http_response_synth_error(response_obj, 500,
								  "sendFile: protocol does not support sendfile");
		return false;
	}

	/* Resolve precompressed sidecar synchronously, before the engine
	 * opens the file. The engine emits Content-Encoding + Vary if
	 * picked_encoding is non-NULL; the original Content-Type is
	 * carried via cfg.content_type so the sidecar's .gz extension
	 * doesn't poison MIME detection. */
	char fs_path[MAXPATHLEN];
	size_t fs_path_len = path_len;
	memcpy(fs_path, ZSTR_VAL(req->path), path_len);
	fs_path[path_len] = '\0';

	const char *picked_encoding = NULL;
	size_t picked_encoding_len = 0;

	if (req->opts.precompressed) {
		(void)sf_try_precompressed(request, fs_path, sizeof(fs_path), &fs_path_len,
								   &picked_encoding, &picked_encoding_len);
	}

	zend_string *cd = sf_build_content_disposition(&req->opts);

	sf_adapter_t *adapter = emalloc(sizeof(*adapter));
	adapter->req = req;
	adapter->content_disposition = cd;
	adapter->on_done = on_done;
	adapter->user = user;

	send_file_config_t cfg = {0};
	cfg.abs_path = fs_path;
	cfg.abs_path_len = fs_path_len;
	cfg.content_type = req->opts.content_type;
	cfg.content_disposition = cd;
	cfg.cache_control = req->opts.cache_control;
	cfg.etag = req->opts.etag;
	cfg.last_modified = req->opts.last_modified;
	cfg.accept_ranges = req->opts.accept_ranges;
	cfg.conditional = req->opts.conditional && (req->opts.etag || req->opts.last_modified);
	cfg.delete_after_send = req->opts.delete_after_send;
	cfg.content_encoding = picked_encoding;
	cfg.content_encoding_len = picked_encoding_len;
	cfg.status_override = req->opts.status;
	cfg.on_error = SEND_FILE_ERR_INLINE_500;

	const send_file_cbs_t cbs = {
		.on_armed = NULL,
		.on_done = sf_adapter_on_done,
		.on_passthrough = NULL,
		.keep_alive = sf_adapter_keep_alive,
	};

	const send_file_result_t r = send_file(request, response_obj, &cfg, &cbs, adapter);

	if (r == SEND_FILE_ASYNC) {
		return true;
	}

	/* Engine refused before kick-off (MAXPATHLEN, FS_OPEN failure,
	 * cb alloc). adapter wasn't handed off; on_done not fired. */
	sf_adapter_free(adapter);

	if (r != SEND_FILE_HANDLED) {
		http_response_synth_error(response_obj, 500, "sendFile: engine refused");
	} else {
		http_response_synth_error(response_obj, 500, "sendFile: cannot open file");
	}

	return false;
}
