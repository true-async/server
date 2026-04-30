/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef MULTIPART_PROCESSOR_H
#define MULTIPART_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "formats/multipart_parser.h"

/**
 * High-level multipart/form-data processor
 *
 * Sits on top of multipart_parser and handles:
 * - Content-Disposition parsing (name, filename)
 * - Content-Type parsing (media type, charset)
 * - File streaming to disk
 * - Form field accumulation in memory
 * - PHP-style array field names ([], [key])
 *
 * Output:
 * - Array of form fields (name => value)
 * - Array of uploaded files (name => file_info)
 */

/* Limits */
#define MP_MAX_FIELD_NAME_LEN    256
#define MP_MAX_FILENAME_LEN      4096
#define MP_MAX_CONTENT_TYPE_LEN  256
#define MP_MAX_CHARSET_LEN       64
#define MP_MAX_FIELD_VALUE_LEN   (1 * 1024 * 1024)   /* 1MB per field */
#define MP_MAX_FILE_SIZE         (100 * 1024 * 1024) /* 100MB per file */
#define MP_MAX_FILES             20
#define MP_MAX_FIELDS            100

/* Upload error codes (compatible with PHP) */
#define MP_UPLOAD_ERR_OK              0   /* Success */
#define MP_UPLOAD_ERR_INI_SIZE        1   /* Exceeds upload_max_filesize */
#define MP_UPLOAD_ERR_FORM_SIZE       2   /* Exceeds MAX_FILE_SIZE in form */
#define MP_UPLOAD_ERR_PARTIAL         3   /* Partial upload */
#define MP_UPLOAD_ERR_NO_FILE         4   /* No file uploaded */
#define MP_UPLOAD_ERR_NO_TMP_DIR      6   /* No temp directory */
#define MP_UPLOAD_ERR_CANT_WRITE      7   /* Failed to write to disk */
#define MP_UPLOAD_ERR_EXTENSION       8   /* Extension stopped upload */
#define MP_UPLOAD_ERR_TOO_MANY_FILES  100 /* Too many files */
#define MP_UPLOAD_ERR_INVALID_NAME    101 /* Invalid filename */
#define MP_UPLOAD_ERR_TOO_LARGE       102 /* File too large */

/* Uploaded file info */
typedef struct {
    char*    field_name;       /* Form field name (e.g., "avatar" or "files[]") */
    char*    client_filename;  /* Original filename from client */
    char*    client_media_type;/* MIME type from client */
    char*    client_charset;   /* Charset from Content-Type (may be NULL) */
    char*    tmp_path;         /* Path to temp file */
    size_t   size;             /* File size in bytes */
    int      error;            /* Error code (MP_UPLOAD_ERR_*) */
    bool     is_ready;         /* File fully written and closed */
} mp_file_info_t;

/* Form field info */
typedef struct {
    char*    name;             /* Field name */
    char*    value;            /* Field value */
    size_t   value_len;        /* Value length */
} mp_field_info_t;

/* Forward declaration */
typedef struct mp_processor_t mp_processor_t;

/* Callback for temp file path generation */
typedef char* (*mp_tmp_path_generator_t)(mp_processor_t* proc, const char* original_filename);

/* Processor configuration */
typedef struct {
    size_t   max_file_size;    /* Max size per file (0 = default) */
    size_t   max_field_size;   /* Max size per field (0 = default) */
    size_t   max_files;        /* Max number of files (0 = default) */
    size_t   max_fields;       /* Max number of fields (0 = default) */
    char*    tmp_dir;          /* Temp directory (NULL = /tmp) */
    mp_tmp_path_generator_t tmp_path_generator; /* Custom temp path generator */
} mp_config_t;

/* Processor structure */
struct mp_processor_t {
    /* Low-level parser */
    multipart_parser_t* parser;

    /* Configuration */
    mp_config_t         config;

    /* Current part state */
    char*               current_header_name;
    size_t              current_header_name_len;
    size_t              current_header_name_cap;
    char*               current_header_value;
    size_t              current_header_value_len;
    size_t              current_header_value_cap;

    /* Parsed headers for current part */
    char*               field_name;        /* From Content-Disposition name="" */
    char*               filename;          /* From Content-Disposition filename="" */
    char*               content_type;      /* From Content-Type */
    char*               charset;           /* From Content-Type charset= */

    /* File handling */
    FILE*               file_handle;       /* Current file being written */
    char*               tmp_path;          /* Current temp file path */
    size_t              file_size;         /* Current file size */

    /* Field handling */
    char*               field_value;       /* Buffer for field value */
    size_t              field_value_len;   /* Current length */
    size_t              field_value_cap;   /* Buffer capacity */

    /* Results */
    mp_file_info_t*     files;             /* Array of uploaded files */
    size_t              files_count;
    size_t              files_capacity;

    mp_field_info_t*    fields;            /* Array of form fields */
    size_t              fields_count;
    size_t              fields_capacity;

    /* Error tracking */
    int                 current_error;     /* Error for current part */
    char*               error_message;     /* Human-readable error */

    /* User data */
    void*               user_data;

    /* Sink for parser-internal log records (PLAN_LOG.md). Cached
     * pointer set by the caller right after mp_processor_create.
     * NULL is safe — http_logf_* macros short-circuit. */
    struct http_log_state *log_state;
};

/**
 * Create a new multipart processor.
 *
 * @param boundary The boundary string (without leading '--')
 * @param config Configuration (NULL for defaults)
 * @return New processor or NULL on error
 */
mp_processor_t* mp_processor_create(const char* boundary, const mp_config_t* config);

/**
 * Feed data to the processor.
 *
 * @param proc Processor instance
 * @param data Data to process
 * @param len Length of data
 * @return Number of bytes processed, or -1 on error
 */
ssize_t mp_processor_feed(mp_processor_t* proc, const char* data, size_t len);

/**
 * Check if processing is complete.
 */
bool mp_processor_is_complete(const mp_processor_t* proc);

/**
 * Check if processor has error.
 */
bool mp_processor_has_error(const mp_processor_t* proc);

/**
 * Get error message.
 */
const char* mp_processor_get_error(const mp_processor_t* proc);

/**
 * Get uploaded files.
 *
 * @param proc Processor instance
 * @param count Output: number of files
 * @return Array of file info structures
 */
mp_file_info_t* mp_processor_get_files(const mp_processor_t* proc, size_t* count);

/**
 * Get form fields.
 *
 * @param proc Processor instance
 * @param count Output: number of fields
 * @return Array of field info structures
 */
mp_field_info_t* mp_processor_get_fields(const mp_processor_t* proc, size_t* count);

/**
 * Set user data pointer.
 */
void mp_processor_set_user_data(mp_processor_t* proc, void* data);

/**
 * Get user data pointer.
 */
void* mp_processor_get_user_data(const mp_processor_t* proc);

/**
 * Cleanup temp files.
 * Call this to delete temp files that weren't moved.
 */
void mp_processor_cleanup_temp_files(mp_processor_t* proc);

/**
 * Destroy processor and free all resources.
 * Does NOT delete temp files (call cleanup first if needed).
 */
void mp_processor_destroy(mp_processor_t* proc);

/**
 * Free a file info structure contents (not the struct itself).
 */
void mp_file_info_free(mp_file_info_t* info);

/**
 * Free a field info structure contents (not the struct itself).
 */
void mp_field_info_free(mp_field_info_t* info);

#endif /* MULTIPART_PROCESSOR_H */
