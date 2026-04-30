/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef MULTIPART_PARSER_H
#define MULTIPART_PARSER_H

#include <stddef.h>
#include <stdint.h>
#ifdef PHP_WIN32
# include <BaseTsd.h>
# ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#  define _SSIZE_T_DEFINED
# endif
#else
# include <sys/types.h>  /* for ssize_t */
#endif

/**
 * Streaming multipart/form-data parser
 *
 * Implements RFC 7578 (multipart/form-data) with streaming callbacks.
 * Zero-copy design: callbacks receive pointers to original buffer.
 *
 * Usage:
 *   1. Create parser with boundary string
 *   2. Set callbacks
 *   3. Feed data chunks via multipart_parser_execute()
 *   4. Callbacks are invoked as data is parsed
 *   5. Destroy parser when done
 */

/* Maximum boundary length per RFC 2046 */
#define MULTIPART_MAX_BOUNDARY_LEN 70

/* Parser states */
typedef enum {
    MP_STATE_START,                    /* Initial state, looking for first boundary */
    MP_STATE_BOUNDARY_START,           /* Found '--', matching boundary */
    MP_STATE_BOUNDARY,                 /* Inside boundary string */
    MP_STATE_BOUNDARY_ALMOST_DONE,     /* CR after boundary */
    MP_STATE_BOUNDARY_DONE,            /* LF after CR (boundary complete) */
    MP_STATE_HEADER_FIELD_START,       /* Start of header name */
    MP_STATE_HEADER_FIELD,             /* Inside header name */
    MP_STATE_HEADER_VALUE_START,       /* Start of header value (after ':') */
    MP_STATE_HEADER_VALUE,             /* Inside header value */
    MP_STATE_HEADER_VALUE_ALMOST_DONE, /* CR after header value */
    MP_STATE_HEADERS_ALMOST_DONE,      /* CR after empty line (end of headers) */
    MP_STATE_HEADERS_DONE,             /* LF after CR (headers complete) */
    MP_STATE_PART_DATA,                /* Inside part body data */
    MP_STATE_PART_DATA_CR,             /* CR in part data (possible boundary) */
    MP_STATE_PART_DATA_LF,             /* LF after CR (possible boundary) */
    MP_STATE_PART_DATA_BOUNDARY_START, /* First '-' after CRLF */
    MP_STATE_PART_DATA_BOUNDARY_DASH,  /* Second '-' after CRLF */
    MP_STATE_PART_DATA_BOUNDARY,       /* Matching boundary in data */
    MP_STATE_PART_DATA_END,            /* First '-' after boundary (possible end) */
    MP_STATE_BODY_END,                 /* Second '-' after boundary (end of body) */
    MP_STATE_END,                      /* Parsing complete */
    MP_STATE_ERROR                     /* Parse error */
} multipart_state_t;

/* Forward declaration */
typedef struct multipart_parser_t multipart_parser_t;

/* Callback types */
typedef int (*multipart_cb)(multipart_parser_t* parser);
typedef int (*multipart_data_cb)(multipart_parser_t* parser, const char* at, size_t length);

/* Callbacks structure */
typedef struct {
    multipart_cb      on_part_begin;       /* New part started */
    multipart_data_cb on_header_field;     /* Header name chunk */
    multipart_data_cb on_header_value;     /* Header value chunk */
    multipart_cb      on_headers_complete; /* All headers for current part parsed */
    multipart_data_cb on_part_data;        /* Part body data chunk (STREAMING!) */
    multipart_cb      on_part_end;         /* Current part ended */
    multipart_cb      on_body_end;         /* All parts parsed, body complete */
} multipart_callbacks_t;

/* Parser structure */
struct multipart_parser_t {
    /* Boundary (without leading '--') */
    char              boundary[MULTIPART_MAX_BOUNDARY_LEN + 1];
    uint8_t           boundary_len;

    /* State machine */
    multipart_state_t state;
    uint8_t           boundary_index;      /* Current position in boundary matching */

    /* Lookbehind buffer for boundary matching across chunks */
    char              lookbehind[MULTIPART_MAX_BOUNDARY_LEN + 4]; /* \r\n--boundary */
    uint8_t           lookbehind_len;

    /* Flags */
    uint8_t           part_started;         /* Set when on_part_begin called */

    /* Callbacks */
    multipart_callbacks_t callbacks;

    /* User data pointer */
    void*             data;

    /* Error info */
    const char*       error_reason;
};

/**
 * Create a new multipart parser.
 *
 * @param boundary The boundary string (without leading '--')
 * @return New parser instance or NULL on error
 */
multipart_parser_t* multipart_parser_create(const char* boundary);

/**
 * Initialize an existing parser structure.
 *
 * @param parser Parser to initialize
 * @param boundary The boundary string (without leading '--')
 * @return 0 on success, -1 on error
 */
int multipart_parser_init(multipart_parser_t* parser, const char* boundary);

/**
 * Set callbacks for the parser.
 *
 * @param parser Parser instance
 * @param callbacks Callbacks structure
 */
void multipart_parser_set_callbacks(multipart_parser_t* parser, const multipart_callbacks_t* callbacks);

/**
 * Set user data pointer.
 *
 * @param parser Parser instance
 * @param data User data pointer
 */
void multipart_parser_set_data(multipart_parser_t* parser, void* data);

/**
 * Get user data pointer.
 *
 * @param parser Parser instance
 * @return User data pointer
 */
void* multipart_parser_get_data(const multipart_parser_t* parser);

/**
 * Execute parser on data chunk.
 *
 * @param parser Parser instance
 * @param data Data to parse
 * @param len Length of data
 * @return Number of bytes parsed, or -1 on error
 */
ssize_t multipart_parser_execute(multipart_parser_t* parser, const char* data, size_t len);

/**
 * Check if parsing is complete.
 *
 * @param parser Parser instance
 * @return 1 if complete, 0 otherwise
 */
int multipart_parser_is_complete(const multipart_parser_t* parser);

/**
 * Check if parser is in error state.
 *
 * @param parser Parser instance
 * @return 1 if error, 0 otherwise
 */
int multipart_parser_has_error(const multipart_parser_t* parser);

/**
 * Get error reason string.
 *
 * @param parser Parser instance
 * @return Error reason or NULL if no error
 */
const char* multipart_parser_get_error(const multipart_parser_t* parser);

/**
 * Reset parser for reuse with same boundary.
 *
 * @param parser Parser instance
 */
void multipart_parser_reset(multipart_parser_t* parser);

/**
 * Destroy parser and free memory.
 *
 * @param parser Parser instance
 */
void multipart_parser_destroy(multipart_parser_t* parser);

#endif /* MULTIPART_PARSER_H */
