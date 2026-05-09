/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/* TrueAsync\SendFileOptions + SendFileDisposition class registration
 * + C-snapshot helpers consumed by HttpResponse::sendFile(). */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "Zend/zend_enum.h"
#include "php_http_server.h"
#include "http_send_file.h"

#include "../stubs/SendFileOptions.php_arginfo.h"

zend_class_entry *http_send_file_options_ce        = NULL;
zend_class_entry *http_send_file_disposition_ce    = NULL;

static void copy_str_arg(zval *zv, zend_string **out)
{
	if (Z_TYPE_P(zv) == IS_STRING) {
		*out = zend_string_copy(Z_STR_P(zv));
	} else {
		*out = NULL;
	}
}

ZEND_METHOD(TrueAsync_SendFileOptions, __construct)
{
	zend_string *content_type     = NULL;
	zval        *disposition      = NULL;
	zend_string *download_name    = NULL;
	zend_string *cache_control    = NULL;
	bool         etag             = true;
	bool         last_modified    = true;
	bool         accept_ranges    = true;
	bool         precompressed    = true;
	bool         conditional      = true;
	bool         delete_after     = false;
	zend_long    status           = 0;
	bool         status_is_null   = true;

	ZEND_PARSE_PARAMETERS_START(0, 11)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(content_type)
		Z_PARAM_OBJECT_OF_CLASS_OR_NULL(disposition, http_send_file_disposition_ce)
		Z_PARAM_STR_OR_NULL(download_name)
		Z_PARAM_STR_OR_NULL(cache_control)
		Z_PARAM_BOOL(etag)
		Z_PARAM_BOOL(last_modified)
		Z_PARAM_BOOL(accept_ranges)
		Z_PARAM_BOOL(precompressed)
		Z_PARAM_BOOL(conditional)
		Z_PARAM_BOOL(delete_after)
		Z_PARAM_LONG_OR_NULL(status, status_is_null)
	ZEND_PARSE_PARAMETERS_END();

	zend_object *const this_obj = Z_OBJ_P(ZEND_THIS);

	/* Direct slot writes — readonly flag on these props rejects normal
	 * setter writes, but the slots already carry the
	 * declared defaults (set by zend_declare_typed_property). Drop the
	 * old zval and place the new one in. Slot order matches the
	 * register_class_TrueAsync_SendFileOptions declaration order. */
#define WRITE_SLOT(num, src) do { \
		zval *const _slot = OBJ_PROP_NUM(this_obj, (num)); \
		zval_ptr_dtor(_slot); \
		ZVAL_COPY_VALUE(_slot, (src)); \
	} while (0)

	{
		zval v;
		if (content_type != NULL) ZVAL_STR_COPY(&v, content_type);
		else                      ZVAL_NULL(&v);
		WRITE_SLOT(0, &v);
	}
	{
		zval v;
		if (disposition != NULL) {
			ZVAL_OBJ_COPY(&v, Z_OBJ_P(disposition));
		} else {
			zend_string *const inline_name = zend_string_init("INLINE", 6, 0);
			zend_object *const def =
				zend_enum_get_case(http_send_file_disposition_ce, inline_name);
			zend_string_release(inline_name);
			ZVAL_OBJ_COPY(&v, def);
		}
		WRITE_SLOT(1, &v);
	}
	{
		zval v;
		if (download_name != NULL) ZVAL_STR_COPY(&v, download_name);
		else                       ZVAL_NULL(&v);
		WRITE_SLOT(2, &v);
	}
	{
		zval v;
		if (cache_control != NULL) ZVAL_STR_COPY(&v, cache_control);
		else                       ZVAL_NULL(&v);
		WRITE_SLOT(3, &v);
	}
	{ zval v; ZVAL_BOOL(&v, etag);          WRITE_SLOT(4, &v); }
	{ zval v; ZVAL_BOOL(&v, last_modified); WRITE_SLOT(5, &v); }
	{ zval v; ZVAL_BOOL(&v, accept_ranges); WRITE_SLOT(6, &v); }
	{ zval v; ZVAL_BOOL(&v, precompressed); WRITE_SLOT(7, &v); }
	{ zval v; ZVAL_BOOL(&v, conditional);   WRITE_SLOT(8, &v); }
	{ zval v; ZVAL_BOOL(&v, delete_after);  WRITE_SLOT(9, &v); }
	{
		zval v;
		if (status_is_null) ZVAL_NULL(&v);
		else                ZVAL_LONG(&v, status);
		WRITE_SLOT(10, &v);
	}
#undef WRITE_SLOT
}

void http_send_file_options_class_register(void)
{
	http_send_file_disposition_ce = register_class_TrueAsync_SendFileDisposition();
	http_send_file_options_ce     = register_class_TrueAsync_SendFileOptions(
		http_send_file_disposition_ce);
}

static void defaults(http_send_file_options_t *out)
{
	memset(out, 0, sizeof(*out));
	out->status            = 0;
	out->disposition       = HTTP_SEND_FILE_DISPOSITION_INLINE;
	out->disposition_set   = false;
	out->etag              = true;
	out->last_modified     = true;
	out->accept_ranges     = true;
	out->precompressed     = true;
	out->conditional       = true;
	out->delete_after_send = false;
}

void http_send_file_options_snapshot(zend_object *obj,
                                     http_send_file_options_t *out)
{
	defaults(out);
	if (obj == NULL || obj->ce != http_send_file_options_ce) {
		return;
	}

	zval rv, *zv;

	zv = zend_read_property(http_send_file_options_ce, obj, "contentType", 11, 1, &rv);
	if (zv != NULL) copy_str_arg(zv, &out->content_type);

	zv = zend_read_property(http_send_file_options_ce, obj, "disposition", 11, 1, &rv);
	if (zv != NULL && Z_TYPE_P(zv) == IS_OBJECT) {
		zval rv2;
		zval *backing = zend_read_property_ex(Z_OBJCE_P(zv), Z_OBJ_P(zv),
			ZSTR_KNOWN(ZEND_STR_NAME), 1, &rv2);
		if (backing != NULL && Z_TYPE_P(backing) == IS_STRING) {
			out->disposition = (ZSTR_LEN(Z_STR_P(backing)) == 10
				&& memcmp(ZSTR_VAL(Z_STR_P(backing)), "ATTACHMENT", 10) == 0)
				? HTTP_SEND_FILE_DISPOSITION_ATTACHMENT
				: HTTP_SEND_FILE_DISPOSITION_INLINE;
			out->disposition_set = true;
		}
	}

	zv = zend_read_property(http_send_file_options_ce, obj, "downloadName", 12, 1, &rv);
	if (zv != NULL) copy_str_arg(zv, &out->download_name);

	zv = zend_read_property(http_send_file_options_ce, obj, "cacheControl", 12, 1, &rv);
	if (zv != NULL) copy_str_arg(zv, &out->cache_control);

#define READ_BOOL(prop, len, field) do { \
		zv = zend_read_property(http_send_file_options_ce, obj, (prop), (len), 1, &rv); \
		if (zv != NULL && (Z_TYPE_P(zv) == IS_TRUE || Z_TYPE_P(zv) == IS_FALSE)) { \
			out->field = (Z_TYPE_P(zv) == IS_TRUE); \
		} \
	} while (0)
	READ_BOOL("etag",            4,  etag);
	READ_BOOL("lastModified",    12, last_modified);
	READ_BOOL("acceptRanges",    12, accept_ranges);
	READ_BOOL("precompressed",   13, precompressed);
	READ_BOOL("conditional",     11, conditional);
	READ_BOOL("deleteAfterSend", 15, delete_after_send);
#undef READ_BOOL

	zv = zend_read_property(http_send_file_options_ce, obj, "status", 6, 1, &rv);
	if (zv != NULL && Z_TYPE_P(zv) == IS_LONG) {
		out->status = (int)Z_LVAL_P(zv);
	}
}

void http_send_file_options_destroy(http_send_file_options_t *opts)
{
	if (opts == NULL) {
		return;
	}
	if (opts->content_type != NULL) {
		zend_string_release(opts->content_type);
		opts->content_type = NULL;
	}
	if (opts->download_name != NULL) {
		zend_string_release(opts->download_name);
		opts->download_name = NULL;
	}
	if (opts->cache_control != NULL) {
		zend_string_release(opts->cache_control);
		opts->cache_control = NULL;
	}
}

void http_send_file_request_free(http_send_file_request_t *req)
{
	if (req == NULL) {
		return;
	}
	if (req->path != NULL) {
		zend_string_release(req->path);
		req->path = NULL;
	}
	http_send_file_options_destroy(&req->opts);
	efree(req);
}
