/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "formats/multipart_processor.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "php.h"
#include "main/php_open_temporary_file.h"  /* cross-platform temp fd */
#include "Zend/zend_virtual_cwd.h"         /* VCWD_UNLINK */
#include "log/http_log.h"
#include "http_rfc5987.h"
#include "http_param_parse.h"

/* PHP's request allocator in every build: the processor runs on the worker
 * thread that owns the request, and what an upload costs counts against
 * memory_limit like the rest of the request. */
#define MP_MALLOC(size)       emalloc(size)
#define MP_CALLOC(n, size)    ecalloc(n, size)
#define MP_REALLOC(ptr, size) erealloc(ptr, size)
#define MP_FREE(ptr)          efree(ptr)
#define MP_STRDUP(s)          estrdup(s)
#define MP_STRNDUP(s, n)      estrndup(s, n)

#define INITIAL_FILES_CAPACITY  4
#define INITIAL_FIELDS_CAPACITY 8
#define INITIAL_FIELD_VALUE_CAP 256
/* Spare capacity a finished field value may keep; past it the value is copied
 * into a buffer of its own size. */
#define MP_FIT_SLACK            4096
#define INITIAL_HEADER_CAP      128

static int on_part_begin(multipart_parser_t* parser);
static int on_header_field(multipart_parser_t* parser, const char* at, size_t length);
static int on_header_value(multipart_parser_t* parser, const char* at, size_t length);
static int on_headers_complete(multipart_parser_t* parser);
static int on_part_data(multipart_parser_t* parser, const char* at, size_t length);
static int on_part_end(multipart_parser_t* parser);
static int on_body_end(multipart_parser_t* parser);

static const multipart_callbacks_t processor_callbacks = {
    .on_part_begin = on_part_begin,
    .on_header_field = on_header_field,
    .on_header_value = on_header_value,
    .on_headers_complete = on_headers_complete,
    .on_part_data = on_part_data,
    .on_part_end = on_part_end,
    .on_body_end = on_body_end
};

/* Helper: Safe string append */
static int str_append(char** buf, size_t* len, size_t* cap, const char* data, size_t data_len)
{
    /* Subtractive form for overflow safety. Callers gate inputs on
     * per-component max_size, so realistic values are far from
     * SIZE_MAX, but defense-in-depth: refuse the append if
     * len + data_len + 1 (NUL) would wrap. */
    if (data_len >= SIZE_MAX - *len - 1) {
        return -1;
    }

    size_t need = *len + data_len + 1;

    if (need > *cap) {
        size_t new_cap = (*cap == 0) ? INITIAL_HEADER_CAP : (*cap * 2);
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) {
                return -1;
            }

            new_cap *= 2;
        }

        char* new_buf = MP_REALLOC(*buf, new_cap);

        if (!new_buf) {
            return -1;
        }
        *buf = new_buf;
        *cap = new_cap;
    }

    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
    return 0;
}

/* Helper: Generate temp file path */
static char* generate_tmp_path(mp_processor_t* proc, const char* original_filename)
{
    (void)original_filename;  /* May be used in custom generators */

    if (proc->config.tmp_path_generator) {
        return proc->config.tmp_path_generator(proc, original_filename);
    }

    /* Directory as in PHP's own upload handler (main/rfc1867.c): upload_tmp_dir where it is
     * set, NULL otherwise, which leaves the system temporary directory to the core.
     * PHP_TMP_FILE_SILENT suppresses the notice the core raises when it falls back from a
     * directory it cannot use. That notice would be raised in an event-loop callback with no
     * PHP frame above it, where a user error handler that throws has nothing to unwind to
     * and the request is dropped; a temporary file that cannot be created at all is still
     * reported, through MP_UPLOAD_ERR_NO_TMP_DIR. */
    const char* tmp_dir = proc->config.tmp_dir ? proc->config.tmp_dir : PG(upload_tmp_dir);

    /* php_open_temporary_fd_ex picks a unique name in tmp_dir with our
     * prefix and returns an fd + zend_string path. Works on POSIX
     * (mkstemp) and Windows (GetTempFileName). We close the fd here
     * and reopen via fopen later — same pattern as before, just portable. */
    zend_string *opened_path = NULL;
    int fd = php_open_temporary_fd_ex(tmp_dir,
                                      "trueasync_upload_",
                                      &opened_path,
                                      PHP_TMP_FILE_SILENT | PHP_TMP_FILE_OPEN_BASEDIR_CHECK_ON_FALLBACK);

    if (fd < 0) {
        if (opened_path) {
            zend_string_release(opened_path);
        }

        return NULL;
    }

#ifdef PHP_WIN32
    _close(fd);
#else
    close(fd);
#endif

    if (!opened_path) {
        return NULL;
    }

    char *result = MP_STRDUP(ZSTR_VAL(opened_path));
    zend_string_release(opened_path);
    return result;
}

/* Decode RFC 5987 ext-value (charset'lang'percent-encoded) into a
 * freshly MP_MALLOC'd NUL-terminated buffer. Returns NULL if the input
 * doesn't have the two single-quote separators. */
static char *decode_rfc5987_value(const char *src, const size_t src_len)
{
    const char *p = src;
    const char *const end = src + src_len;

    while (p < end && *p != '\'') p++;

    if (p == end) return NULL;
    p++;

    while (p < end && *p != '\'') p++;

    if (p == end) return NULL;
    p++;

    const size_t enc_len = (size_t)(end - p);
    char *out = MP_MALLOC(enc_len + 1);
    const size_t dec_len = http_rfc5987_decode(out, p, enc_len);
    out[dec_len] = '\0';
    return out;
}

/* Helper: Parse Content-Disposition header.
 * Format: form-data; name="field_name"; filename="file.txt"
 * filename* (RFC 5987) takes precedence over filename when both appear. */
static void parse_content_disposition(mp_processor_t* proc, const char* value)
{
    const char *p = value;

    /* Skip the disposition token ("form-data") up to the first ';'. */
    while (*p && *p != ';') p++;

    http_param_t param;

    while (http_header_param_next(&p, NULL, &param)) {
        if (param.name_len == 4 && memcmp(param.name, "name", 4) == 0) {
            if (proc->field_name) MP_FREE(proc->field_name);
            proc->field_name = MP_STRNDUP(param.value, param.value_len);
        } else if (param.name_len == 8 && memcmp(param.name, "filename", 8) == 0) {
            if (proc->filename) MP_FREE(proc->filename);
            proc->filename = MP_STRNDUP(param.value, param.value_len);
        } else if (param.name_len == 9 && memcmp(param.name, "filename*", 9) == 0) {
            char *decoded = decode_rfc5987_value(param.value, param.value_len);

            if (decoded != NULL) {
                if (proc->filename) MP_FREE(proc->filename);
                proc->filename = decoded;
            }
        }
    }
}

/* Helper: Parse Content-Type header.
 * Format: text/plain; charset=utf-8 */
static void parse_content_type(mp_processor_t* proc, const char* value)
{
    const char *p = value;

    /* Media type up to ';' or whitespace. */
    const char *start = p;
    while (*p && *p != ';' && *p != ' ' && *p != '\t') p++;

    if (proc->content_type) MP_FREE(proc->content_type);
    proc->content_type = MP_STRNDUP(start, p - start);

    http_param_t param;

    while (http_header_param_next(&p, NULL, &param)) {
        if (param.name_len == 7 && strncasecmp(param.name, "charset", 7) == 0) {
            if (proc->charset) MP_FREE(proc->charset);
            proc->charset = MP_STRNDUP(param.value, param.value_len);
            break;
        }
    }
}

/* Helper: Check for path traversal or NUL injection in a candidate
 * filename. The function uses C-string scanning (strstr, strlen) and
 * therefore relies on `filename` being NUL-clean — see the invariant
 * note in on_part_begin. The audit (S-06, 2026-04-26) raised the
 * concern that an embedded NUL would let `..` slip past strstr; today
 * the upstream parser truncates at NUL so it can't happen, but this
 * helper is the right place to enforce it explicitly. */
static bool has_path_traversal_or_nul(const char* filename, size_t len)
{
    if (!filename) return false;

    /* NUL injection — strlen would lie, so callers can't rely on
     * downstream C-string predicates. Reject upfront. */
    if (memchr(filename, '\0', len) != NULL) return true;

    /* Check for .. sequences */
    if (strstr(filename, "..") != NULL) return true;

    /* Check for absolute paths */
    if (filename[0] == '/') return true;

    if (filename[0] == '\\') return true;

    /* Check for Windows drive letters */
    if (len >= 2 && filename[1] == ':') return true;

    return false;
}

/* True when @p add more bytes on top of @p have pass @p limit; 0 is no limit.
 * Subtractive, so a count near SIZE_MAX cannot wrap (S-02). */
static bool size_exceeds(const size_t have, const size_t add, const size_t limit)
{
    return limit > 0 && (have > limit || add > limit - have);
}

/* The code a file is refused with when @p length more bytes would pass a
 * limit, UPLOAD_ERR_OK while they fit. The server's limit is checked before
 * the form's, as PHP checks upload_max_filesize before MAX_FILE_SIZE. */
static int file_size_error(const mp_processor_t *proc, const size_t length)
{
    if (size_exceeds(proc->file_size, length, proc->config.max_file_size)) {
        return MP_UPLOAD_ERR_INI_SIZE;
    }

    if (size_exceeds(proc->file_size, length, proc->form_max_file_size)) {
        return MP_UPLOAD_ERR_FORM_SIZE;
    }

    return MP_UPLOAD_ERR_OK;
}

/* Callbacks */

static int on_part_begin(multipart_parser_t* parser)
{
    mp_processor_t* proc = multipart_parser_get_data(parser);

    /* Reset current part state */
    if (proc->current_header_name) {
        proc->current_header_name[0] = '\0';
        proc->current_header_name_len = 0;
    }

    if (proc->current_header_value) {
        proc->current_header_value[0] = '\0';
        proc->current_header_value_len = 0;
    }

    if (proc->field_name) { MP_FREE(proc->field_name); proc->field_name = NULL; }

    if (proc->filename) { MP_FREE(proc->filename); proc->filename = NULL; }

    if (proc->content_type) { MP_FREE(proc->content_type); proc->content_type = NULL; }

    if (proc->charset) { MP_FREE(proc->charset); proc->charset = NULL; }

    proc->file_handle = NULL;

    if (proc->tmp_path) { MP_FREE(proc->tmp_path); proc->tmp_path = NULL; }
    proc->file_size = 0;
    proc->skip_part = false;

    if (proc->field_value) {
        proc->field_value[0] = '\0';
    }

    proc->field_value_len = 0;

    proc->current_error = MP_UPLOAD_ERR_OK;

    return 0;
}

static int on_header_field(multipart_parser_t* parser, const char* at, size_t length)
{
    mp_processor_t* proc = multipart_parser_get_data(parser);

    /* If we were accumulating a value, this is a new header */
    if (proc->current_header_value_len > 0) {
        http_logf_debug(proc->log_state,
            "multipart.header.flush name='%s' value='%s'",
            proc->current_header_name ? proc->current_header_name : "(null)",
            proc->current_header_value ? proc->current_header_value : "(null)");
        /* Process previous header */
        if (strcasecmp(proc->current_header_name, "Content-Disposition") == 0) {
            parse_content_disposition(proc, proc->current_header_value);
            http_logf_debug(proc->log_state,
                "multipart.content_disposition.parsed field_name=%s filename=%s",
                proc->field_name ? proc->field_name : "(null)",
                proc->filename ? proc->filename : "(null)");
        } else if (strcasecmp(proc->current_header_name, "Content-Type") == 0) {
            parse_content_type(proc, proc->current_header_value);
        }

        /* Reset for new header */
        proc->current_header_name[0] = '\0';
        proc->current_header_name_len = 0;
        proc->current_header_value[0] = '\0';
        proc->current_header_value_len = 0;
    }

    if (!proc->current_header_name) {
        proc->current_header_name_cap = INITIAL_HEADER_CAP;
        proc->current_header_name = MP_MALLOC(proc->current_header_name_cap);

        if (!proc->current_header_name) return -1;
        proc->current_header_name[0] = '\0';
    }

    return str_append(&proc->current_header_name, &proc->current_header_name_len,
                      &proc->current_header_name_cap, at, length);
}

static int on_header_value(multipart_parser_t* parser, const char* at, size_t length)
{
    mp_processor_t* proc = multipart_parser_get_data(parser);

    if (!proc->current_header_value) {
        proc->current_header_value_cap = INITIAL_HEADER_CAP;
        proc->current_header_value = MP_MALLOC(proc->current_header_value_cap);

        if (!proc->current_header_value) return -1;
        proc->current_header_value[0] = '\0';
    }

    return str_append(&proc->current_header_value, &proc->current_header_value_len,
                      &proc->current_header_value_cap, at, length);
}

static int on_headers_complete(multipart_parser_t* parser)
{
    mp_processor_t* proc = multipart_parser_get_data(parser);

    /* Process last header if any */
    http_logf_debug(proc->log_state,
        "multipart.headers_complete name='%s'(%zu) value='%s'(%zu)",
        proc->current_header_name ? proc->current_header_name : "(null)",
        proc->current_header_name_len,
        proc->current_header_value ? proc->current_header_value : "(null)",
        proc->current_header_value_len);

    if (proc->current_header_name_len > 0 && proc->current_header_value_len > 0) {
        if (strcasecmp(proc->current_header_name, "Content-Disposition") == 0) {
            parse_content_disposition(proc, proc->current_header_value);
        } else if (strcasecmp(proc->current_header_name, "Content-Type") == 0) {
            parse_content_type(proc, proc->current_header_value);
        }
    }

    http_logf_debug(proc->log_state,
                    "multipart.part.dispatch field_name=%s filename=%s",
                    proc->field_name ? proc->field_name : "(null)",
                    proc->filename ? proc->filename : "(null)");

    /* Every part that names itself counts, fields and files alike, as PHP
     * counts them against max_multipart_body_parts. */
    if (proc->field_name != NULL || proc->filename != NULL) {
        if (proc->config.max_parts > 0 && proc->parts_count >= proc->config.max_parts) {
            proc->refusal = MP_REFUSAL_TOO_MANY_PARTS;
            return -1;
        }

        proc->parts_count++;
    }

    /* Once the file count is spent, every later file part is left out, an
     * empty one ("no file selected") too, as PHP leaves them out
     * (main/rfc1867.c). Only a saved file spends the count. */
    if (proc->filename != NULL) {
        const size_t max_files = proc->config.max_files > 0 ? proc->config.max_files : MP_MAX_FILES;

        if (proc->config.skip_files || proc->files_saved >= max_files) {
            if (!proc->config.skip_files && !proc->files_warned) {
                proc->files_warned = true;
                http_logf_warn(proc->log_state,
                    "multipart.files_left_out max_file_uploads=%zu", max_files);
            }

            proc->skip_part = true;
            return 0;
        }
    }

    /* Determine if this is a file or field */
    if (proc->filename != NULL && strlen(proc->filename) > 0) {
        /* This is a FILE upload */

        /* Filename validation. Invariant from parse_content_disposition:
         * proc->filename is C-string-walked and therefore truncated at
         * the first NUL — embedded NULs cannot reach here today. The
         * NUL check inside has_path_traversal_or_nul is defense-in-
         * depth: if any future change makes filename a length-prefixed
         * blob (e.g. proper RFC 5987 decoding), it stays robust. */
        size_t filename_len = strlen(proc->filename);

        if (filename_len > MP_MAX_FILENAME_LEN) {
            proc->current_error = MP_UPLOAD_ERR_INVALID_NAME;
            return 0;  /* Continue but mark as error */
        }

        if (has_path_traversal_or_nul(proc->filename, filename_len)) {
            proc->current_error = MP_UPLOAD_ERR_INVALID_NAME;
            return 0;
        }

        /* Generate temp file path */
        proc->tmp_path = generate_tmp_path(proc, proc->filename);
        proc->files_saved++;

        if (!proc->tmp_path) {
            proc->current_error = MP_UPLOAD_ERR_NO_TMP_DIR;
            return 0;
        }

        /* Open file for writing */
        proc->file_handle = fopen(proc->tmp_path, "wb");

        if (!proc->file_handle) {
            proc->current_error = MP_UPLOAD_ERR_CANT_WRITE;
            /* The name was taken by creating the file. */
            VCWD_UNLINK(proc->tmp_path);
            MP_FREE(proc->tmp_path);
            proc->tmp_path = NULL;
            return 0;
        }

        proc->file_size = 0;

    } else if (proc->filename != NULL && strlen(proc->filename) == 0) {
        /* Empty filename = no file selected */
        proc->current_error = MP_UPLOAD_ERR_NO_FILE;

    } else {
        /* This is a regular FIELD */
        /* Allocate field value buffer if needed */
        if (!proc->field_value) {
            proc->field_value_cap = INITIAL_FIELD_VALUE_CAP;
            proc->field_value = MP_MALLOC(proc->field_value_cap);

            if (!proc->field_value) return -1;
        }

        proc->field_value[0] = '\0';
        proc->field_value_len = 0;
    }

    return 0;
}

/* Fail the file in flight as PHP fails one (main/rfc1867.c): the error, and no
 * temp file and no size left behind. */
static void cancel_file(mp_processor_t* proc, const int error)
{
    proc->current_error = error;

    if (proc->file_handle) {
        fclose(proc->file_handle);
        proc->file_handle = NULL;
    }

    if (proc->tmp_path) {
        VCWD_UNLINK(proc->tmp_path);
        MP_FREE(proc->tmp_path);
        proc->tmp_path = NULL;
    }

    proc->file_size = 0;
}

static int on_part_data(multipart_parser_t* parser, const char* at, size_t length)
{
    mp_processor_t* proc = multipart_parser_get_data(parser);

    if (proc->file_handle) {
        /* FILE: write to disk */

        const int size_error = file_size_error(proc, length);

        if (size_error != MP_UPLOAD_ERR_OK) {
            cancel_file(proc, size_error);
            return 0;  /* Continue parsing but don't write */
        }

        /* Write data to file (STREAMING!) */
        size_t written = fwrite(at, 1, length, proc->file_handle);

        if (written != length) {
            cancel_file(proc, MP_UPLOAD_ERR_CANT_WRITE);
            return 0;
        }

        proc->file_size += length;

    } else if (proc->current_error == MP_UPLOAD_ERR_OK && proc->filename == NULL) {
        /* FIELD: accumulate in memory */

        size_t max_size = proc->config.max_field_size > 0 ?
                          proc->config.max_field_size : MP_MAX_FIELD_VALUE_LEN;

        /* Subtractive form — see file_size check above (S-02). */
        if (proc->field_value_len > max_size ||
            length > max_size - proc->field_value_len) {
            proc->refusal = MP_REFUSAL_FIELD_TOO_LARGE;
            return -1;
        }

        if (str_append(&proc->field_value, &proc->field_value_len,
                       &proc->field_value_cap, at, length) != 0) {
            return -1;
        }
    }
    /* If error or no file selected, just skip data */

    return 0;
}

static int on_part_end(multipart_parser_t* parser)
{
    mp_processor_t* proc = multipart_parser_get_data(parser);

    if (proc->file_handle) {
        /* A part smaller than the stdio buffer reaches the disk only here, so
         * a failed flush or close is a failed write. */
        FILE* const file = proc->file_handle;
        proc->file_handle = NULL;

        const bool flushed = fflush(file) == 0;
        const bool closed  = fclose(file) == 0;

        if (!flushed || !closed) {
            cancel_file(proc, MP_UPLOAD_ERR_CANT_WRITE);
        }
    }

    if (proc->skip_part) {
        return 0;
    }

    /* Store result */
    if (proc->filename != NULL) {
        /* FILE: add to files array */

        /* Expand array if needed */
        if (proc->files_count >= proc->files_capacity) {
            size_t new_cap = proc->files_capacity == 0 ?
                             INITIAL_FILES_CAPACITY : proc->files_capacity * 2;
            /* Safe-mult: reject doubling that overflows or wraps the
             * realloc byte count (S-05). Doubling itself can overflow
             * if files_capacity > SIZE_MAX/2; the byte multiplication
             * can overflow even when the doubling does not. */
            if (new_cap < proc->files_capacity ||
                new_cap > SIZE_MAX / sizeof(mp_file_info_t)) {
                return -1;
            }

            mp_file_info_t* new_files = MP_REALLOC(proc->files,
                                                    new_cap * sizeof(mp_file_info_t));

            if (!new_files) return -1;
            proc->files = new_files;
            proc->files_capacity = new_cap;
        }

        mp_file_info_t* info = &proc->files[proc->files_count];
        memset(info, 0, sizeof(mp_file_info_t));

        info->field_name = proc->field_name ? MP_STRDUP(proc->field_name) : NULL;
        info->client_filename = proc->filename ? MP_STRDUP(proc->filename) : NULL;
        info->client_media_type = proc->content_type ? MP_STRDUP(proc->content_type) : NULL;
        info->client_charset = proc->charset ? MP_STRDUP(proc->charset) : NULL;
        info->tmp_path = proc->tmp_path ? MP_STRDUP(proc->tmp_path) : NULL;
        info->size = proc->file_size;
        info->error = proc->current_error;
        info->is_ready = (proc->current_error == MP_UPLOAD_ERR_OK);

        proc->files_count++;

        /* Clear tmp_path so it won't be freed in part_begin */
        if (proc->tmp_path) {
            MP_FREE(proc->tmp_path);
            proc->tmp_path = NULL;
        }

    } else if (proc->field_name != NULL) {
        /* FIELD: add to fields array */

        /* Check fields limit */
        size_t max_fields = proc->config.max_fields > 0 ?
                            proc->config.max_fields : MP_MAX_FIELDS;

        if (proc->fields_count >= max_fields) {
            proc->refusal = MP_REFUSAL_TOO_MANY_FIELDS;
            return -1;
        }

        /* Expand array if needed */
        if (proc->fields_count >= proc->fields_capacity) {
            size_t new_cap = proc->fields_capacity == 0 ?
                             INITIAL_FIELDS_CAPACITY : proc->fields_capacity * 2;
            /* Safe-mult: see files-array branch above (S-05). */
            if (new_cap < proc->fields_capacity ||
                new_cap > SIZE_MAX / sizeof(mp_field_info_t)) {
                return -1;
            }

            mp_field_info_t* new_fields = MP_REALLOC(proc->fields,
                                                      new_cap * sizeof(mp_field_info_t));

            if (!new_fields) return -1;
            proc->fields = new_fields;
            proc->fields_capacity = new_cap;
        }

        mp_field_info_t* info = &proc->fields[proc->fields_count];

        info->name = proc->field_name ? MP_STRDUP(proc->field_name) : NULL;
        /* The accumulated buffer moves into the field rather than being
         * copied: a value may be as large as the body. The length, not a NUL,
         * ends it, since a field may carry NUL bytes; str_append keeps room
         * for the terminator. */
        if (proc->field_value) {
            /* Fitted to the value: the buffer grew by doubling, and a value as
             * large as the body would hold up to twice its size until the
             * request ends. A copy, not erealloc: Windows cannot shrink a huge
             * block in place, and keeps all of it. */
            const size_t fitted = proc->field_value_len + 1;

            if (proc->field_value_cap - fitted >= MP_FIT_SLACK) {
                info->value = MP_MALLOC(fitted);
                memcpy(info->value, proc->field_value, fitted);
                MP_FREE(proc->field_value);
            } else {
                info->value = proc->field_value;
            }

            proc->field_value = NULL;
            proc->field_value_cap = 0;
        } else {
            info->value = MP_MALLOC(1);

            if (!info->value) {
                if (info->name) {
                    MP_FREE(info->name);
                }

                return -1;
            }
        }

        info->value[proc->field_value_len] = '\0';
        info->value_len = proc->field_value_len;
        proc->field_value_len = 0;

        /* The form's own limit on the files that follow it, as PHP reads it.
         * A value that is not a positive number sets none. */
        if (info->name != NULL && strcasecmp(info->name, "MAX_FILE_SIZE") == 0) {
            const long long limit = strtoll(info->value, NULL, 10);

            proc->form_max_file_size = limit > 0 ? (size_t)limit : 0;
        }

        proc->fields_count++;
    }

    return 0;
}

static int on_body_end(multipart_parser_t* parser)
{
    (void)parser;
    /* Nothing special to do here */
    return 0;
}

/* Public API */

mp_processor_t* mp_processor_create(const char* boundary, const mp_config_t* config)
{
    mp_processor_t* proc = MP_CALLOC(1, sizeof(mp_processor_t));

    if (!proc) return NULL;

    proc->parser = multipart_parser_create(boundary);

    if (!proc->parser) {
        MP_FREE(proc);
        return NULL;
    }

    multipart_parser_set_callbacks(proc->parser, &processor_callbacks);
    multipart_parser_set_data(proc->parser, proc);

    if (config) {
        proc->config = *config;

        if (config->tmp_dir) {
            proc->config.tmp_dir = MP_STRDUP(config->tmp_dir);
        }
    }

    return proc;
}

ssize_t mp_processor_feed(mp_processor_t* proc, const char* data, size_t len)
{
    if (!proc || !data) return -1;

    const ssize_t processed = multipart_parser_execute(proc->parser, data, len);

    if (processed < 0 && proc->refusal == MP_REFUSAL_NONE) {
        proc->refusal = MP_REFUSAL_MALFORMED;
    }

    /* All of `len` counts: past the closing boundary the parser consumes
     * nothing, and bytes a peer keeps sending there are body bytes still. */
    if (processed >= 0) {
        proc->bytes_fed += len;
    }

    return processed;
}

bool mp_processor_is_complete(const mp_processor_t* proc)
{
    return proc && multipart_parser_is_complete(proc->parser);
}

bool mp_processor_has_error(const mp_processor_t* proc)
{
    return proc && multipart_parser_has_error(proc->parser);
}

const char* mp_processor_get_error(const mp_processor_t* proc)
{
    if (!proc) return NULL;

    if (proc->error_message) return proc->error_message;
    return multipart_parser_get_error(proc->parser);
}

mp_file_info_t* mp_processor_get_files(const mp_processor_t* proc, size_t* count)
{
    if (!proc) {
        if (count) *count = 0;
        return NULL;
    }

    if (count) *count = proc->files_count;
    return proc->files;
}

mp_field_info_t* mp_processor_get_fields(const mp_processor_t* proc, size_t* count)
{
    if (!proc) {
        if (count) *count = 0;
        return NULL;
    }

    if (count) *count = proc->fields_count;
    return proc->fields;
}

void mp_processor_set_user_data(mp_processor_t* proc, void* data)
{
    if (proc) proc->user_data = data;
}

void* mp_processor_get_user_data(const mp_processor_t* proc)
{
    return proc ? proc->user_data : NULL;
}

void mp_processor_cleanup_temp_files(mp_processor_t* proc)
{
    if (!proc) return;

    for (size_t i = 0; i < proc->files_count; i++) {
        if (proc->files[i].tmp_path) {
            VCWD_UNLINK(proc->files[i].tmp_path);
        }
    }

    /* A file part the body stopped inside (a refusal, a reset, a parse
     * error) reaches files[] only when it ends; until then tmp_path names
     * its file, open or already closed by a failed write. */
    if (proc->file_handle) {
        fclose(proc->file_handle);
        proc->file_handle = NULL;
    }

    if (proc->tmp_path) {
        VCWD_UNLINK(proc->tmp_path);
    }
}

void mp_file_info_release_tmp_path(mp_file_info_t* info)
{
    if (info && info->tmp_path) {
        MP_FREE(info->tmp_path);
        info->tmp_path = NULL;
    }
}

void mp_file_info_free(mp_file_info_t* info)
{
    if (!info) return;

    if (info->field_name) MP_FREE(info->field_name);

    if (info->client_filename) MP_FREE(info->client_filename);

    if (info->client_media_type) MP_FREE(info->client_media_type);

    if (info->client_charset) MP_FREE(info->client_charset);

    if (info->tmp_path) MP_FREE(info->tmp_path);
    memset(info, 0, sizeof(mp_file_info_t));
}

void mp_field_info_free(mp_field_info_t* info)
{
    if (!info) return;

    if (info->name) MP_FREE(info->name);

    if (info->value) MP_FREE(info->value);
    memset(info, 0, sizeof(mp_field_info_t));
}

void mp_processor_destroy(mp_processor_t* proc)
{
    if (!proc) return;

    if (proc->parser) {
        multipart_parser_destroy(proc->parser);
    }

    if (proc->current_header_name) MP_FREE(proc->current_header_name);

    if (proc->current_header_value) MP_FREE(proc->current_header_value);

    if (proc->field_name) MP_FREE(proc->field_name);

    if (proc->filename) MP_FREE(proc->filename);

    if (proc->content_type) MP_FREE(proc->content_type);

    if (proc->charset) MP_FREE(proc->charset);

    if (proc->tmp_path) MP_FREE(proc->tmp_path);

    if (proc->field_value) MP_FREE(proc->field_value);

    /* Close any open file */
    if (proc->file_handle) {
        fclose(proc->file_handle);
    }

    for (size_t i = 0; i < proc->files_count; i++) {
        mp_file_info_free(&proc->files[i]);
    }

    if (proc->files) MP_FREE(proc->files);

    for (size_t i = 0; i < proc->fields_count; i++) {
        mp_field_info_free(&proc->fields[i]);
    }

    if (proc->fields) MP_FREE(proc->fields);

    if (proc->config.tmp_dir) MP_FREE(proc->config.tmp_dir);

    if (proc->error_message) MP_FREE(proc->error_message);

    MP_FREE(proc);
}
