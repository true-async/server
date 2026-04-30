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

#include "formats/multipart_parser.h"

#include <stdlib.h>
#include <string.h>

/* Use PHP memory functions if available */
#ifdef PHP_WIN32
# include "php.h"
# define MP_MALLOC(size)     emalloc(size)
# define MP_CALLOC(n, size)  ecalloc(n, size)
# define MP_FREE(ptr)        efree(ptr)
#else
# ifdef HAVE_PHP_H
#  include "php.h"
#  define MP_MALLOC(size)     emalloc(size)
#  define MP_CALLOC(n, size)  ecalloc(n, size)
#  define MP_FREE(ptr)        efree(ptr)
# else
#  define MP_MALLOC(size)     malloc(size)
#  define MP_CALLOC(n, size)  calloc(n, size)
#  define MP_FREE(ptr)        free(ptr)
# endif
#endif

/* Helper macros */
#define MP_CALLBACK(NAME) \
    do { \
        if (parser->callbacks.NAME) { \
            if (parser->callbacks.NAME(parser) != 0) { \
                parser->state = MP_STATE_ERROR; \
                parser->error_reason = "callback " #NAME " returned error"; \
                return -1; \
            } \
        } \
    } while (0)

#define CALLBACK_DATA(NAME, P, L) \
    do { \
        if (parser->callbacks.NAME && (L) > 0) { \
            if (parser->callbacks.NAME(parser, P, L) != 0) { \
                parser->state = MP_STATE_ERROR; \
                parser->error_reason = "callback " #NAME " returned error"; \
                return -1; \
            } \
        } \
    } while (0)

#define IS_CR(c)    ((c) == '\r')
#define IS_LF(c)    ((c) == '\n')
#define IS_DASH(c)  ((c) == '-')
#define IS_COLON(c) ((c) == ':')
#define IS_SPACE(c) ((c) == ' ' || (c) == '\t')

/* Create parser */
multipart_parser_t* multipart_parser_create(const char* boundary)
{
    multipart_parser_t* parser = MP_CALLOC(1, sizeof(multipart_parser_t));
    if (!parser) {
        return NULL;
    }

    if (multipart_parser_init(parser, boundary) != 0) {
        MP_FREE(parser);
        return NULL;
    }

    return parser;
}

/* Initialize parser */
int multipart_parser_init(multipart_parser_t* parser, const char* boundary)
{
    size_t boundary_len;

    if (!parser || !boundary) {
        return -1;
    }

    boundary_len = strlen(boundary);
    if (boundary_len == 0 || boundary_len > MULTIPART_MAX_BOUNDARY_LEN) {
        return -1;
    }

    memset(parser, 0, sizeof(multipart_parser_t));

    memcpy(parser->boundary, boundary, boundary_len);
    parser->boundary[boundary_len] = '\0';
    parser->boundary_len = (uint8_t)boundary_len;

    parser->state = MP_STATE_START;

    return 0;
}

/* Set callbacks */
void multipart_parser_set_callbacks(multipart_parser_t* parser, const multipart_callbacks_t* callbacks)
{
    if (parser && callbacks) {
        parser->callbacks = *callbacks;
    }
}

/* Set user data */
void multipart_parser_set_data(multipart_parser_t* parser, void* data)
{
    if (parser) {
        parser->data = data;
    }
}

/* Get user data */
void* multipart_parser_get_data(const multipart_parser_t* parser)
{
    return parser ? parser->data : NULL;
}

/* Flush lookbehind buffer as data */
static int flush_lookbehind(multipart_parser_t* parser)
{
    if (parser->lookbehind_len > 0) {
        CALLBACK_DATA(on_part_data, parser->lookbehind, parser->lookbehind_len);
        parser->lookbehind_len = 0;
    }
    return 0;
}

/* Execute parser */
ssize_t multipart_parser_execute(multipart_parser_t* parser, const char* data, size_t len)
{
    const char* p = data;
    const char* end = data + len;
    const char* mark = NULL;  /* Start of current data segment */
    char c;

    if (!parser || !data) {
        return -1;
    }

    if (parser->state == MP_STATE_ERROR) {
        return -1;
    }

    if (parser->state == MP_STATE_END) {
        return 0;
    }

    while (p < end) {
        c = *p;

        /* Initialize mark for states that accumulate data */
        if (mark == NULL) {
            switch (parser->state) {
            case MP_STATE_HEADER_FIELD:
            case MP_STATE_HEADER_VALUE:
            case MP_STATE_PART_DATA:
                mark = p;
                break;
            default:
                break;
            }
        }

        switch (parser->state) {

        case MP_STATE_START:
            /* Looking for initial '--' */
            if (IS_DASH(c)) {
                parser->state = MP_STATE_BOUNDARY_START;
            } else if (!IS_CR(c) && !IS_LF(c)) {
                /* Skip any preamble data (rare but allowed by RFC) */
            }
            break;

        case MP_STATE_BOUNDARY_START:
            /* Got first '-', looking for second '-' */
            if (IS_DASH(c)) {
                parser->boundary_index = 0;
                parser->state = MP_STATE_BOUNDARY;
            } else {
                /* Not a boundary, back to start */
                parser->state = MP_STATE_START;
            }
            break;

        case MP_STATE_BOUNDARY:
            /* Matching boundary string */
            if (parser->boundary_index < parser->boundary_len) {
                if (c == parser->boundary[parser->boundary_index]) {
                    parser->boundary_index++;
                } else {
                    /* Mismatch - not a boundary */
                    parser->state = MP_STATE_START;
                }
            }
            if (parser->boundary_index == parser->boundary_len) {
                /* Boundary matched completely */
                parser->state = MP_STATE_BOUNDARY_ALMOST_DONE;
            }
            break;

        case MP_STATE_BOUNDARY_ALMOST_DONE:
            /* After boundary, expecting CR, LF, or '--' for end */
            if (IS_CR(c)) {
                parser->state = MP_STATE_BOUNDARY_DONE;
            } else if (IS_DASH(c)) {
                /* Could be closing '--' for empty form or end of last part */
                parser->state = MP_STATE_BODY_END;
            } else if (IS_LF(c)) {
                /* Some servers send just LF */
                MP_CALLBACK(on_part_begin);
                parser->part_started = 1;
                parser->state = MP_STATE_HEADER_FIELD_START;
            } else {
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "expected CR or LF after boundary";
                return -1;
            }
            break;

        case MP_STATE_BOUNDARY_DONE:
            /* After CR, expecting LF */
            if (IS_LF(c)) {
                MP_CALLBACK(on_part_begin);
                parser->part_started = 1;
                parser->state = MP_STATE_HEADER_FIELD_START;
            } else {
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "expected LF after CR";
                return -1;
            }
            break;

        case MP_STATE_HEADER_FIELD_START:
            /* Start of header line - could be header name or empty line */
            mark = p;
            if (IS_CR(c)) {
                parser->state = MP_STATE_HEADERS_ALMOST_DONE;
            } else if (IS_LF(c)) {
                /* Empty line with just LF */
                MP_CALLBACK(on_headers_complete);
                parser->state = MP_STATE_PART_DATA;
                mark = p + 1;
            } else {
                parser->state = MP_STATE_HEADER_FIELD;
            }
            break;

        case MP_STATE_HEADER_FIELD:
            /* Inside header name */
            if (IS_COLON(c)) {
                /* End of header name */
                CALLBACK_DATA(on_header_field, mark, p - mark);
                parser->state = MP_STATE_HEADER_VALUE_START;
                mark = NULL;
            } else if (IS_CR(c) || IS_LF(c)) {
                /* Unexpected end of line in header name */
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "unexpected end of line in header name";
                return -1;
            }
            break;

        case MP_STATE_HEADER_VALUE_START:
            /* After ':', skip optional whitespace */
            if (IS_SPACE(c)) {
                /* Skip leading whitespace */
            } else {
                mark = p;
                parser->state = MP_STATE_HEADER_VALUE;
                /* Don't consume this character, re-process it */
                continue;
            }
            break;

        case MP_STATE_HEADER_VALUE:
            /* Inside header value */
            if (IS_CR(c)) {
                CALLBACK_DATA(on_header_value, mark, p - mark);
                parser->state = MP_STATE_HEADER_VALUE_ALMOST_DONE;
                mark = NULL;
            } else if (IS_LF(c)) {
                /* LF without CR */
                CALLBACK_DATA(on_header_value, mark, p - mark);
                parser->state = MP_STATE_HEADER_FIELD_START;
                mark = NULL;
            }
            break;

        case MP_STATE_HEADER_VALUE_ALMOST_DONE:
            /* After CR in header value, expecting LF */
            if (IS_LF(c)) {
                parser->state = MP_STATE_HEADER_FIELD_START;
            } else {
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "expected LF after CR in header";
                return -1;
            }
            break;

        case MP_STATE_HEADERS_ALMOST_DONE:
            /* After CR on empty line, expecting LF */
            if (IS_LF(c)) {
                MP_CALLBACK(on_headers_complete);
                parser->state = MP_STATE_PART_DATA;
                mark = p + 1;
            } else {
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "expected LF after CR";
                return -1;
            }
            break;

        case MP_STATE_PART_DATA:
            /* Inside part body - look for boundary */
            if (mark == NULL) {
                mark = p;
            }
            if (IS_CR(c)) {
                /* Possible start of boundary: \r\n--boundary */
                if (p > mark) {
                    CALLBACK_DATA(on_part_data, mark, p - mark);
                }
                parser->lookbehind[0] = '\r';
                parser->lookbehind_len = 1;
                parser->state = MP_STATE_PART_DATA_CR;
                mark = NULL;
            } else if (IS_LF(c)) {
                /* LF-only line endings: \n--boundary */
                if (p > mark) {
                    CALLBACK_DATA(on_part_data, mark, p - mark);
                }
                parser->lookbehind[0] = '\n';
                parser->lookbehind_len = 1;
                parser->state = MP_STATE_PART_DATA_LF;
                mark = NULL;
            }
            break;

        case MP_STATE_PART_DATA_CR:
            /* Got CR, looking for LF */
            if (IS_LF(c)) {
                parser->lookbehind[parser->lookbehind_len++] = '\n';
                parser->state = MP_STATE_PART_DATA_LF;
            } else {
                /* Not a boundary, flush lookbehind and continue */
                if (flush_lookbehind(parser) != 0) return -1;
                parser->state = MP_STATE_PART_DATA;
                /* Re-process this character */
                continue;
            }
            break;

        case MP_STATE_PART_DATA_LF:
            /* Got CRLF, looking for first '-' */
            if (IS_DASH(c)) {
                parser->lookbehind[parser->lookbehind_len++] = '-';
                parser->state = MP_STATE_PART_DATA_BOUNDARY_START;
            } else {
                /* Not a boundary */
                if (flush_lookbehind(parser) != 0) return -1;
                parser->state = MP_STATE_PART_DATA;
                continue;
            }
            break;

        case MP_STATE_PART_DATA_BOUNDARY_START:
            /* Got CRLF-, looking for second '-' */
            if (IS_DASH(c)) {
                parser->lookbehind[parser->lookbehind_len++] = '-';
                parser->boundary_index = 0;
                parser->state = MP_STATE_PART_DATA_BOUNDARY;
            } else {
                /* Not a boundary */
                if (flush_lookbehind(parser) != 0) return -1;
                parser->state = MP_STATE_PART_DATA;
                continue;
            }
            break;

        case MP_STATE_PART_DATA_BOUNDARY:
            /* Matching boundary in data */
            if (parser->boundary_index < parser->boundary_len) {
                if (c == parser->boundary[parser->boundary_index]) {
                    parser->lookbehind[parser->lookbehind_len++] = c;
                    parser->boundary_index++;
                } else {
                    /* Mismatch - not a boundary, flush and continue */
                    if (flush_lookbehind(parser) != 0) return -1;
                    parser->state = MP_STATE_PART_DATA;
                    continue;
                }
            }
            if (parser->boundary_index == parser->boundary_len) {
                /* Full boundary matched! */
                parser->lookbehind_len = 0;  /* Discard lookbehind (it was boundary) */
                parser->state = MP_STATE_PART_DATA_END;
            }
            break;

        case MP_STATE_PART_DATA_END:
            /* After boundary in data, check for '--' (end) or CRLF (next part) */
            if (IS_DASH(c)) {
                parser->state = MP_STATE_BODY_END;
            } else if (IS_CR(c)) {
                /* End current part, start next */
                MP_CALLBACK(on_part_end);
                parser->part_started = 0;
                parser->state = MP_STATE_BOUNDARY_DONE;
            } else if (IS_LF(c)) {
                /* LF without CR */
                MP_CALLBACK(on_part_end);
                parser->part_started = 0;
                MP_CALLBACK(on_part_begin);
                parser->part_started = 1;
                parser->state = MP_STATE_HEADER_FIELD_START;
            } else {
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "expected CRLF or '--' after boundary";
                return -1;
            }
            break;

        case MP_STATE_BODY_END:
            /* Got '-' after boundary, expecting second '-' for end */
            if (IS_DASH(c)) {
                /* Only call on_part_end if a part was actually started */
                if (parser->part_started) {
                    MP_CALLBACK(on_part_end);
                    parser->part_started = 0;
                }
                MP_CALLBACK(on_body_end);
                parser->state = MP_STATE_END;
            } else {
                parser->state = MP_STATE_ERROR;
                parser->error_reason = "expected '--' at end of body";
                return -1;
            }
            break;

        case MP_STATE_END:
            /* Parsing complete, ignore any trailing data (epilogue) */
            break;

        case MP_STATE_ERROR:
            return -1;

        default:
            parser->state = MP_STATE_ERROR;
            parser->error_reason = "unknown state";
            return -1;
        }

        p++;
    }

    /* Flush any remaining data at end of chunk */
    if (mark && p > mark) {
        switch (parser->state) {
        case MP_STATE_HEADER_FIELD:
            CALLBACK_DATA(on_header_field, mark, p - mark);
            break;
        case MP_STATE_HEADER_VALUE:
            CALLBACK_DATA(on_header_value, mark, p - mark);
            break;
        case MP_STATE_PART_DATA:
            CALLBACK_DATA(on_part_data, mark, p - mark);
            break;
        default:
            break;
        }
    }

    return (ssize_t)(p - data);
}

/* Check if complete */
int multipart_parser_is_complete(const multipart_parser_t* parser)
{
    return parser && parser->state == MP_STATE_END;
}

/* Check if error */
int multipart_parser_has_error(const multipart_parser_t* parser)
{
    return parser && parser->state == MP_STATE_ERROR;
}

/* Get error reason */
const char* multipart_parser_get_error(const multipart_parser_t* parser)
{
    if (parser && parser->state == MP_STATE_ERROR) {
        return parser->error_reason;
    }
    return NULL;
}

/* Reset parser */
void multipart_parser_reset(multipart_parser_t* parser)
{
    if (parser) {
        /* Keep boundary and callbacks, reset state */
        parser->state = MP_STATE_START;
        parser->boundary_index = 0;
        parser->lookbehind_len = 0;
        parser->part_started = 0;
        parser->error_reason = NULL;
    }
}

/* Destroy parser */
void multipart_parser_destroy(multipart_parser_t* parser)
{
    if (parser) {
        MP_FREE(parser);
    }
}
