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

#include "php.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"
#include "php_http_server.h"
#include "formats/multipart_processor.h"

#include <sys/stat.h>
#include <errno.h>
#include "Zend/zend_virtual_cwd.h"   /* VCWD_UNLINK / _CHMOD / _MKDIR / _STAT */
#include "Zend/zend_compile.h"       /* zend_dirname */

#ifdef PHP_WIN32
# include <io.h>
# define mode_t int
#else
# include <unistd.h>
#endif

/* Use multipart processor error codes */
#define UPLOAD_ERR_OK MP_UPLOAD_ERR_OK

/* Include generated arginfo */
#include "../stubs/UploadedFile.php_arginfo.h"

/* UploadedFile class entry */
zend_class_entry *uploaded_file_ce;

/* UploadedFile object structure */
typedef struct {
    zend_string*        client_filename;       /* Original filename from client */
    zend_string*        client_media_type;     /* MIME type from client */
    zend_string*        client_charset;        /* Charset (may be NULL) */
    zend_string*        tmp_path;              /* Path to temp file */
    size_t              size;                  /* File size in bytes */
    int                 error;                 /* Error code (UPLOAD_ERR_*) */
    bool                is_ready;              /* File fully written and closed */
    bool                moved;                 /* File has been moved */
    zend_object         std;
} uploaded_file_object;

/* Get UploadedFile object from zend_object */
static inline uploaded_file_object* uploaded_file_from_obj(zend_object *obj)
{
    return (uploaded_file_object*)((char*)(obj) - XtOffsetOf(uploaded_file_object, std));
}

#define Z_UPLOADED_FILE_P(zv) uploaded_file_from_obj(Z_OBJ_P(zv))

/* Object handlers */
static zend_object_handlers uploaded_file_object_handlers;

/* Create UploadedFile object */
static zend_object* uploaded_file_create_object(zend_class_entry *ce)
{
    uploaded_file_object *intern = zend_object_alloc(sizeof(uploaded_file_object), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->std.handlers = &uploaded_file_object_handlers;
    intern->client_filename = NULL;
    intern->client_media_type = NULL;
    intern->client_charset = NULL;
    intern->tmp_path = NULL;
    intern->size = 0;
    intern->error = UPLOAD_ERR_OK;
    intern->is_ready = false;
    intern->moved = false;

    return &intern->std;
}

/* Free UploadedFile object */
static void uploaded_file_free_object(zend_object *object)
{
    uploaded_file_object *intern = uploaded_file_from_obj(object);

    if (intern->client_filename) {
        zend_string_release(intern->client_filename);
    }
    if (intern->client_media_type) {
        zend_string_release(intern->client_media_type);
    }
    if (intern->client_charset) {
        zend_string_release(intern->client_charset);
    }
    if (intern->tmp_path) {
        /* Delete temp file if not moved */
        if (!intern->moved && intern->is_ready) {
            VCWD_UNLINK(ZSTR_VAL(intern->tmp_path));
        }
        zend_string_release(intern->tmp_path);
    }

    zend_object_std_dtor(&intern->std);
}

/* Treat "mkdir failed because the dir already exists" the same on
 * POSIX and Windows. POSIX mkdir returns EEXIST in that case; Windows
 * MoveFile/CreateDirectoryW behind VCWD_MKDIR returns EACCES when the
 * walk hits a system-owned ancestor like `C:\Users\` (existing, not
 * writable by us, but already the right kind of node). Both should be
 * accepted as long as the path is genuinely an existing directory. */
static int mkdir_existing_ok(const char *path)
{
    zend_stat_t st;
    return (VCWD_STAT(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

/* Helper: Create directory recursively. Uses VCWD_MKDIR which maps to
 * mkdir on POSIX and php_win32_ioutil_mkdir on Windows. Treats both /
 * and \ as separators so the same code works on both. */
static int mkdir_recursive(const char *path, mode_t mode)
{
    char *p;
    char *path_copy = estrdup(path);
    int ret = 0;

    for (p = path_copy + 1; *p; p++) {
#ifdef PHP_WIN32
        if (*p == '/' || *p == '\\') {
#else
        if (*p == '/') {
#endif
            const char saved = *p;
            *p = '\0';
            if (VCWD_MKDIR(path_copy, mode) != 0
                && errno != EEXIST
                && mkdir_existing_ok(path_copy) != 0) {
                ret = -1;
                *p = saved;
                break;
            }
            *p = saved;
        }
    }
    if (ret == 0
        && VCWD_MKDIR(path_copy, mode) != 0
        && errno != EEXIST
        && mkdir_existing_ok(path_copy) != 0) {
        ret = -1;
    }

    efree(path_copy);
    return ret;
}

/* Helper: Copy file */
static int copy_file(const char *src, const char *dst)
{
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) {
        return -1;
    }

    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fclose(fsrc);
        return -1;
    }

    char buf[8192];
    size_t n;
    int ret = 0;

    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) {
            ret = -1;
            break;
        }
    }

    if (ferror(fsrc)) {
        ret = -1;
    }

    fclose(fsrc);
    fclose(fdst);

    return ret;
}

/* Methods implementation */

ZEND_METHOD(TrueAsync_UploadedFile, getStream)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->moved) {
        zend_throw_exception(spl_ce_RuntimeException,
            "Cannot get stream: file has already been moved", 0);
        RETURN_THROWS();
    }

    if (intern->error != UPLOAD_ERR_OK) {
        RETURN_NULL();
    }

    if (!intern->tmp_path) {
        RETURN_NULL();
    }

    php_stream *stream = php_stream_open_wrapper(
        ZSTR_VAL(intern->tmp_path), "rb",
        REPORT_ERRORS, NULL
    );

    if (!stream) {
        RETURN_NULL();
    }

    php_stream_to_zval(stream, return_value);
}

ZEND_METHOD(TrueAsync_UploadedFile, moveTo)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    char *target_path;
    size_t target_path_len;
    zend_long mode = 0644;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STRING(target_path, target_path_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    if (intern->moved) {
        zend_throw_exception(spl_ce_RuntimeException,
            "Cannot move file: file has already been moved", 0);
        RETURN_THROWS();
    }

    if (intern->error != UPLOAD_ERR_OK) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Cannot move file: upload error %d", intern->error);
        RETURN_THROWS();
    }

    if (!intern->tmp_path) {
        zend_throw_exception(spl_ce_RuntimeException,
            "Cannot move file: no temporary file available", 0);
        RETURN_THROWS();
    }

    /* Create parent directory if needed. zend_dirname mutates the buffer
     * in place and returns the new length; works identically on POSIX
     * and Windows (handles both separators). */
    char *dir_copy = estrdup(target_path);
    zend_dirname(dir_copy, (size_t)target_path_len);
    zend_stat_t st;

    if (VCWD_STAT(dir_copy, &st) != 0) {
        if (mkdir_recursive(dir_copy, 0755) != 0) {
            efree(dir_copy);
            zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                "Cannot create directory: %s", strerror(errno));
            RETURN_THROWS();
        }
    }
    efree(dir_copy);

    /* Try atomic rename first. VCWD_RENAME handles Windows UTF-8 paths. */
    if (VCWD_RENAME(ZSTR_VAL(intern->tmp_path), target_path) == 0) {
        VCWD_CHMOD(target_path, (mode_t)mode);
        intern->moved = true;
        return;
    }

    /* Cross-filesystem fallback: copy + unlink. EXDEV is POSIX-only;
     * Windows rename returns other errors for cross-volume but the
     * fallback still helps. */
#ifdef EXDEV
    if (errno == EXDEV) {
#else
    {
#endif
        if (copy_file(ZSTR_VAL(intern->tmp_path), target_path) == 0) {
            VCWD_UNLINK(ZSTR_VAL(intern->tmp_path));
            VCWD_CHMOD(target_path, (mode_t)mode);
            intern->moved = true;
            return;
        }
    }

    zend_throw_exception_ex(spl_ce_RuntimeException, 0,
        "Failed to move file: %s", strerror(errno));
}

ZEND_METHOD(TrueAsync_UploadedFile, getSize)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->error != UPLOAD_ERR_OK) {
        RETURN_NULL();
    }

    RETURN_LONG((zend_long)intern->size);
}

ZEND_METHOD(TrueAsync_UploadedFile, getError)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(intern->error);
}

ZEND_METHOD(TrueAsync_UploadedFile, getClientFilename)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->client_filename) {
        RETURN_STR_COPY(intern->client_filename);
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_UploadedFile, getClientMediaType)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->client_media_type) {
        RETURN_STR_COPY(intern->client_media_type);
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_UploadedFile, getClientCharset)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    if (intern->client_charset) {
        RETURN_STR_COPY(intern->client_charset);
    }

    RETURN_NULL();
}

ZEND_METHOD(TrueAsync_UploadedFile, isReady)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(intern->is_ready);
}

ZEND_METHOD(TrueAsync_UploadedFile, isValid)
{
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(ZEND_THIS);
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(intern->error == UPLOAD_ERR_OK);
}

/* Register UploadedFile class. Class entry + methods + FINAL flag
 * come from register_class_TrueAsync_UploadedFile() in the arginfo
 * header (canonical modern-PHP-ext path). We layer on object
 * handlers + NO_DYNAMIC_PROPERTIES + create_object since the stub
 * generator can't express those. */
void uploaded_file_class_register(void)
{
    uploaded_file_ce = register_class_TrueAsync_UploadedFile();
    uploaded_file_ce->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    uploaded_file_ce->create_object = uploaded_file_create_object;

    memcpy(&uploaded_file_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    uploaded_file_object_handlers.offset = XtOffsetOf(uploaded_file_object, std);
    uploaded_file_object_handlers.free_obj = uploaded_file_free_object;
    uploaded_file_object_handlers.clone_obj = NULL;  /* No cloning */
}

/* Helper: Create UploadedFile object from mp_file_info_t */
zval* uploaded_file_create_from_info(mp_file_info_t *info)
{
    zval *obj = emalloc(sizeof(zval));

    object_init_ex(obj, uploaded_file_ce);
    uploaded_file_object *intern = Z_UPLOADED_FILE_P(obj);

    if (info->client_filename) {
        intern->client_filename = zend_string_init(info->client_filename, strlen(info->client_filename), 0);
    }
    if (info->client_media_type) {
        intern->client_media_type = zend_string_init(info->client_media_type, strlen(info->client_media_type), 0);
    }
    if (info->client_charset) {
        intern->client_charset = zend_string_init(info->client_charset, strlen(info->client_charset), 0);
    }
    if (info->tmp_path) {
        intern->tmp_path = zend_string_init(info->tmp_path, strlen(info->tmp_path), 0);
    }

    intern->size = info->size;
    intern->error = info->error;
    intern->is_ready = info->is_ready;
    intern->moved = false;

    return obj;
}
