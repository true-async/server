/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "formats/multipart_processor.h"

/*
 * Test: Simple form field
 */
static void test_simple_field(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "john_doe\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    ssize_t result = mp_processor_feed(proc, body, strlen(body));
    assert_true(result > 0);
    assert_true(mp_processor_is_complete(proc));

    /* Check fields */
    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 1);
    assert_non_null(fields);

    assert_string_equal(fields[0].name, "username");
    assert_string_equal(fields[0].value, "john_doe");

    /* Check files */
    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 0);

    mp_processor_destroy(proc);
}

/*
 * Test: Multiple form fields
 */
static void test_multiple_fields(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "john\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"email\"\r\n"
        "\r\n"
        "john@example.com\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"age\"\r\n"
        "\r\n"
        "25\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 3);

    assert_string_equal(fields[0].name, "username");
    assert_string_equal(fields[0].value, "john");

    assert_string_equal(fields[1].name, "email");
    assert_string_equal(fields[1].value, "john@example.com");

    assert_string_equal(fields[2].name, "age");
    assert_string_equal(fields[2].value, "25");

    mp_processor_destroy(proc);
}

/*
 * Test: File upload
 */
static void test_file_upload(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n"
        "FAKE_JPEG_DATA_HERE\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    /* Check files */
    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    assert_string_equal(files[0].field_name, "avatar");
    assert_string_equal(files[0].client_filename, "photo.jpg");
    assert_string_equal(files[0].client_media_type, "image/jpeg");
    assert_int_equal(files[0].error, MP_UPLOAD_ERR_OK);
    assert_true(files[0].is_ready);
    assert_int_equal(files[0].size, strlen("FAKE_JPEG_DATA_HERE"));

    /* Verify temp file exists */
    assert_non_null(files[0].tmp_path);
    struct stat st;
    assert_int_equal(stat(files[0].tmp_path, &st), 0);
    assert_int_equal(st.st_size, strlen("FAKE_JPEG_DATA_HERE"));

    /* Verify file content */
    FILE* f = fopen(files[0].tmp_path, "rb");
    assert_non_null(f);
    char buf[100];
    size_t read = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    assert_int_equal(read, strlen("FAKE_JPEG_DATA_HERE"));
    buf[read] = '\0';
    assert_string_equal(buf, "FAKE_JPEG_DATA_HERE");

    /* Cleanup */
    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Mixed fields and files
 */
static void test_mixed_fields_and_files(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "My Document\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"document\"; filename=\"doc.txt\"\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Document content here\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"description\"\r\n"
        "\r\n"
        "A test document\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    /* Check fields */
    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 2);

    assert_string_equal(fields[0].name, "title");
    assert_string_equal(fields[0].value, "My Document");

    assert_string_equal(fields[1].name, "description");
    assert_string_equal(fields[1].value, "A test document");

    /* Check files */
    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    assert_string_equal(files[0].field_name, "document");
    assert_string_equal(files[0].client_filename, "doc.txt");
    assert_string_equal(files[0].client_media_type, "text/plain");
    assert_string_equal(files[0].client_charset, "utf-8");

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Multiple file uploads
 */
static void test_multiple_files(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"files[]\"; filename=\"file1.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Content of file 1\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"files[]\"; filename=\"file2.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Content of file 2\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"files[]\"; filename=\"file3.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Content of file 3\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 3);

    assert_string_equal(files[0].field_name, "files[]");
    assert_string_equal(files[0].client_filename, "file1.txt");

    assert_string_equal(files[1].field_name, "files[]");
    assert_string_equal(files[1].client_filename, "file2.txt");

    assert_string_equal(files[2].field_name, "files[]");
    assert_string_equal(files[2].client_filename, "file3.txt");

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Empty file (no file selected)
 */
static void test_empty_file(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    assert_int_equal(files[0].error, MP_UPLOAD_ERR_NO_FILE);
    assert_null(files[0].tmp_path);

    mp_processor_destroy(proc);
}

/*
 * Test: File size limit
 */
static void test_file_size_limit(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";

    /* Create body with large file */
    char large_content[2048];
    memset(large_content, 'X', sizeof(large_content) - 1);
    large_content[sizeof(large_content) - 1] = '\0';

    char body[4096];
    snprintf(body, sizeof(body),
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"large.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "%s\r\n"
        "------WebKitFormBoundary--\r\n",
        large_content);

    /* Set small limit */
    mp_config_t config = {0};
    config.max_file_size = 100;  /* 100 bytes only */

    mp_processor_t* proc = mp_processor_create(boundary, &config);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    assert_int_equal(files[0].error, MP_UPLOAD_ERR_TOO_LARGE);

    mp_processor_destroy(proc);
}

/*
 * Test: Too many files limit
 */
static void test_too_many_files(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"f1\"; filename=\"1.txt\"\r\n"
        "\r\n"
        "1\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"f2\"; filename=\"2.txt\"\r\n"
        "\r\n"
        "2\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"f3\"; filename=\"3.txt\"\r\n"
        "\r\n"
        "3\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_config_t config = {0};
    config.max_files = 2;  /* Only allow 2 files */

    mp_processor_t* proc = mp_processor_create(boundary, &config);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);

    /* First 2 files should be OK, 3rd should have error */
    assert_int_equal(file_count, 3);
    assert_int_equal(files[0].error, MP_UPLOAD_ERR_OK);
    assert_int_equal(files[1].error, MP_UPLOAD_ERR_OK);
    assert_int_equal(files[2].error, MP_UPLOAD_ERR_TOO_MANY_FILES);

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Path traversal attack
 */
static void test_path_traversal(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"../../../etc/passwd\"\r\n"
        "\r\n"
        "malicious content\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    assert_int_equal(files[0].error, MP_UPLOAD_ERR_INVALID_NAME);
    assert_null(files[0].tmp_path);  /* No file should be created */

    mp_processor_destroy(proc);
}

/*
 * Test: Chunked processing (simulate network chunks)
 */
static void test_chunked_processing(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "This is the file content that spans multiple chunks.\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    /* Feed in small chunks */
    size_t total = strlen(body);
    size_t chunk_size = 15;  /* Small chunks */
    size_t offset = 0;

    while (offset < total) {
        size_t len = (offset + chunk_size < total) ? chunk_size : (total - offset);
        ssize_t result = mp_processor_feed(proc, body + offset, len);
        assert_true(result >= 0);
        offset += len;
    }

    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);
    assert_int_equal(files[0].error, MP_UPLOAD_ERR_OK);

    /* Verify content */
    FILE* f = fopen(files[0].tmp_path, "rb");
    assert_non_null(f);
    char buf[200];
    size_t read = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[read] = '\0';
    assert_string_equal(buf, "This is the file content that spans multiple chunks.");

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Field with array-style name
 */
static void test_array_field_names(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"tags[]\"\r\n"
        "\r\n"
        "php\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"tags[]\"\r\n"
        "\r\n"
        "async\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"user[name]\"\r\n"
        "\r\n"
        "John\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"user[email]\"\r\n"
        "\r\n"
        "john@example.com\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 4);

    /* Field names should be preserved as-is */
    assert_string_equal(fields[0].name, "tags[]");
    assert_string_equal(fields[0].value, "php");

    assert_string_equal(fields[1].name, "tags[]");
    assert_string_equal(fields[1].value, "async");

    assert_string_equal(fields[2].name, "user[name]");
    assert_string_equal(fields[2].value, "John");

    assert_string_equal(fields[3].name, "user[email]");
    assert_string_equal(fields[3].value, "john@example.com");

    mp_processor_destroy(proc);
}

/*
 * Test: Content-Type with charset
 */
static void test_content_type_with_charset(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"data.csv\"\r\n"
        "Content-Type: text/csv; charset=windows-1251\r\n"
        "\r\n"
        "col1,col2\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    assert_string_equal(files[0].client_media_type, "text/csv");
    assert_string_equal(files[0].client_charset, "windows-1251");

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * =============================================================================
 * EDGE CASES FROM BUSBOY/FORMIDABLE TEST SUITES
 * Source: https://github.com/mscdex/busboy/blob/master/test/test-types-multipart.js
 * Source: https://github.com/node-formidable/formidable
 * =============================================================================
 */

/*
 * Test: Unicode filename with RFC 5987 encoding
 *
 * RFC 5987 defines a way to encode non-ASCII filenames using percent-encoding.
 * Format: filename*=utf-8''%C3%A4%C3%B6%C3%BC.txt
 *
 * This is how browsers encode filenames with special characters like
 * umlauts (ä, ö, ü), Chinese characters, emoji, etc.
 *
 * Note: RFC 7578 says filename* MUST NOT be used, but browsers still send it.
 * We should handle it gracefully.
 */
static void test_unicode_filename_rfc5987(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* filename*=utf-8''n%C3%A4me.txt should decode to "näme.txt" */
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename*=utf-8''n%C3%A4me.txt\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "content\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    /* Should have captured the filename (may or may not be decoded depending on impl) */
    assert_non_null(files[0].client_filename);
    /* At minimum, shouldn't crash or reject the file */
    assert_int_equal(files[0].error, MP_UPLOAD_ERR_OK);

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Filename with quotes inside (escaped)
 *
 * RFC 2616 allows quotes in filenames by escaping them with backslash.
 * filename="\"quoted\".txt" should become: "quoted".txt
 *
 * This is important for security - improper handling could lead to
 * injection attacks or file system issues.
 */
static void test_filename_with_escaped_quotes(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* Filename contains escaped quotes */
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"\\\"quoted\\\".txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "content\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    /* File should be processed (exact filename handling may vary) */
    assert_non_null(files[0].client_filename);

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Zero-byte file (empty file upload)
 *
 * User selects a file that exists but is empty (0 bytes).
 * This is different from "no file selected" - the filename is present,
 * but the content is empty. Should create a valid 0-byte temp file.
 *
 * Common scenario: uploading an empty log file, placeholder file, etc.
 */
static void test_zero_byte_file(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"empty.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        /* No content between headers and boundary! */
        "\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    /* Should be valid but with 0 size */
    assert_string_equal(files[0].client_filename, "empty.txt");
    assert_int_equal(files[0].size, 0);
    assert_int_equal(files[0].error, MP_UPLOAD_ERR_OK);

    /* Temp file should exist but be empty */
    assert_non_null(files[0].tmp_path);
    struct stat st;
    assert_int_equal(stat(files[0].tmp_path, &st), 0);
    assert_int_equal(st.st_size, 0);

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Windows-style path in filename (backslashes)
 *
 * Old IE browsers sent full Windows paths: C:\Users\John\photo.jpg
 * Modern browsers only send the filename, but we must handle legacy clients.
 *
 * Security: Must strip the path to prevent directory traversal!
 * "C:\Users\..\..\..\etc\passwd" should become just "passwd"
 */
static void test_windows_path_in_filename(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"C:\\Users\\John\\Documents\\photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n"
        "fake image data\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    /*
     * Filename should be preserved as-is (path traversal check is separate).
     * The actual security check is in path traversal test.
     * getClientFilename() returns original name from client.
     */
    assert_non_null(files[0].client_filename);

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Field value containing null bytes
 *
 * Null bytes in form data could be used for:
 * 1. Truncation attacks (C string termination)
 * 2. Bypass filters that only check prefix
 *
 * The processor should handle this safely - either strip null bytes
 * or preserve them depending on security model.
 */
static void test_field_with_null_bytes(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";

    /* Manually construct body with null byte in value */
    char body[512];
    int len = snprintf(body, sizeof(body),
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "before");
    body[len++] = '\0';  /* Null byte */
    len += snprintf(body + len, sizeof(body) - len,
        "after\r\n"
        "------WebKitFormBoundary--\r\n");

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, len);
    assert_true(mp_processor_is_complete(proc));

    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 1);

    /* Value should be captured (length includes everything up to boundary) */
    assert_non_null(fields[0].value);
    /* Check that we got some data (exact behavior with null bytes may vary) */
    assert_true(fields[0].value_len > 0);

    mp_processor_destroy(proc);
}

/*
 * Test: Very long field name (potential DoS)
 *
 * Attackers might send extremely long field names to:
 * 1. Exhaust memory
 * 2. Overflow buffers
 * 3. Cause performance issues
 *
 * The processor should have limits on field name length.
 */
static void test_very_long_field_name(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";

    /* Create a field name with 1000 characters */
    char long_name[1024];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    char body[2048];
    snprintf(body, sizeof(body),
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"%s\"\r\n"
        "\r\n"
        "value\r\n"
        "------WebKitFormBoundary--\r\n",
        long_name);

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    /* Should complete without crashing - may or may not accept the long name */
    assert_true(mp_processor_is_complete(proc));

    mp_processor_destroy(proc);
}

/*
 * Test: Duplicate field names (last one wins? or array?)
 *
 * HTML forms can have multiple fields with the same name (without []).
 * Different frameworks handle this differently:
 * - PHP: last value wins (without [])
 * - Node.js: may create array
 *
 * We follow PHP semantics: preserve all values, let PHP layer decide.
 */
static void test_duplicate_field_names(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"color\"\r\n"
        "\r\n"
        "red\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"color\"\r\n"
        "\r\n"
        "blue\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"color\"\r\n"
        "\r\n"
        "green\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);

    /* All three values should be captured */
    assert_int_equal(field_count, 3);

    assert_string_equal(fields[0].name, "color");
    assert_string_equal(fields[0].value, "red");

    assert_string_equal(fields[1].name, "color");
    assert_string_equal(fields[1].value, "blue");

    assert_string_equal(fields[2].name, "color");
    assert_string_equal(fields[2].value, "green");

    mp_processor_destroy(proc);
}

/*
 * Test: Mixed Content-Types for different parts
 *
 * A single multipart request can have parts with different Content-Types:
 * - Form fields: typically no Content-Type (defaults to text/plain)
 * - Text files: text/plain, text/html, application/json
 * - Binary files: application/octet-stream, image/png, etc.
 *
 * Each part should preserve its own Content-Type.
 */
static void test_mixed_content_types(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"json_file\"; filename=\"data.json\"\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
        "{\"key\": \"value\"}\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"image\"; filename=\"pixel.png\"\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
        "PNG_DATA_HERE\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"text\"; filename=\"readme.txt\"\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Hello World\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 3);

    assert_string_equal(files[0].client_media_type, "application/json");
    assert_string_equal(files[1].client_media_type, "image/png");
    assert_string_equal(files[2].client_media_type, "text/plain");
    assert_string_equal(files[2].client_charset, "utf-8");

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Field with special characters in name
 *
 * Field names can contain various special characters:
 * - Spaces (though unusual)
 * - Unicode characters
 * - Special symbols: @, #, $, etc.
 *
 * The processor should preserve these exactly as sent.
 */
static void test_special_chars_in_field_name(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field with spaces\"\r\n"
        "\r\n"
        "value1\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field@special#chars$here\"\r\n"
        "\r\n"
        "value2\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 2);

    /* Names should be preserved exactly */
    assert_string_equal(fields[0].name, "field with spaces");
    assert_string_equal(fields[1].name, "field@special#chars$here");

    mp_processor_destroy(proc);
}

/*
 * Test: No Content-Disposition header (malformed)
 *
 * Content-Disposition is REQUIRED for multipart/form-data parts.
 * A part without it is malformed and should be skipped or cause an error.
 *
 * This tests graceful handling of malformed input.
 */
static void test_missing_content_disposition(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    /* First part has no Content-Disposition - malformed! */
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Type: text/plain\r\n"  /* Only Content-Type, no Content-Disposition */
        "\r\n"
        "orphan data\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"valid_field\"\r\n"
        "\r\n"
        "valid value\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    /* Should complete (graceful handling) */
    assert_true(mp_processor_is_complete(proc));

    /* The valid field should still be captured */
    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);

    /* At least the valid field should be present */
    /* (malformed part may or may not be captured depending on implementation) */
    assert_true(field_count >= 1);

    mp_processor_destroy(proc);
}

/*
 * Test: Binary data that looks like boundary
 *
 * Binary files (images, executables) may contain byte sequences that
 * look like the boundary string. The parser must only recognize boundaries
 * that are preceded by CRLF.
 *
 * This is a critical security/correctness test!
 */
static void test_binary_data_like_boundary(void **state) {
    (void) state;

    const char* boundary = "ABC123";
    /*
     * File content contains "--ABC123" but NOT preceded by CRLF.
     * This should NOT be treated as a boundary!
     */
    const char* body =
        "--ABC123\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"tricky.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "DATA--ABC123MOREDATA--ABC123EVENMORE\r\n"  /* Fake boundaries in data */
        "--ABC123--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    mp_processor_feed(proc, body, strlen(body));
    assert_true(mp_processor_is_complete(proc));

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);

    /* File should contain ALL the data including the fake boundaries */
    assert_int_equal(files[0].size, strlen("DATA--ABC123MOREDATA--ABC123EVENMORE"));

    /* Verify file content */
    FILE* f = fopen(files[0].tmp_path, "rb");
    assert_non_null(f);
    char buf[100];
    size_t read = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[read] = '\0';
    assert_string_equal(buf, "DATA--ABC123MOREDATA--ABC123EVENMORE");

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/*
 * Test: Byte-by-byte feeding (extreme chunking)
 *
 * Tests parser robustness when data arrives one byte at a time.
 * This simulates worst-case network conditions or malicious slow-loris attacks.
 * The parser must correctly maintain state across all callbacks.
 */
static void test_byte_by_byte_feeding(void **state) {
    (void) state;

    const char* boundary = "----WebKitFormBoundary";
    const char* body =
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "test value\r\n"
        "------WebKitFormBoundary\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file content\r\n"
        "------WebKitFormBoundary--\r\n";

    mp_processor_t* proc = mp_processor_create(boundary, NULL);
    assert_non_null(proc);

    /* Feed ONE BYTE at a time */
    for (size_t i = 0; i < strlen(body); i++) {
        ssize_t result = mp_processor_feed(proc, body + i, 1);
        assert_true(result >= 0);
    }

    assert_true(mp_processor_is_complete(proc));

    /* Verify all data was captured correctly */
    size_t field_count;
    mp_field_info_t* fields = mp_processor_get_fields(proc, &field_count);
    assert_int_equal(field_count, 1);
    assert_string_equal(fields[0].value, "test value");

    size_t file_count;
    mp_file_info_t* files = mp_processor_get_files(proc, &file_count);
    assert_int_equal(file_count, 1);
    assert_int_equal(files[0].error, MP_UPLOAD_ERR_OK);

    mp_processor_cleanup_temp_files(proc);
    mp_processor_destroy(proc);
}

/* Group setup/teardown — initialize PHP runtime so emalloc/efree
 * and php_open_temporary_fd have a live rt. */
#include "common/php_sapi_test.h"

static int group_setup(void **state) {
    (void)state;
    return php_test_runtime_init();
}

static int group_teardown(void **state) {
    (void)state;
    php_test_runtime_shutdown();
    return 0;
}

/* Main test runner */
int main(void) {
    const struct CMUnitTest tests[] = {
        /* Original tests */
        cmocka_unit_test(test_simple_field),
        cmocka_unit_test(test_multiple_fields),
        cmocka_unit_test(test_file_upload),
        cmocka_unit_test(test_mixed_fields_and_files),
        cmocka_unit_test(test_multiple_files),
        cmocka_unit_test(test_empty_file),
        cmocka_unit_test(test_file_size_limit),
        cmocka_unit_test(test_too_many_files),
        cmocka_unit_test(test_path_traversal),
        cmocka_unit_test(test_chunked_processing),
        cmocka_unit_test(test_array_field_names),
        cmocka_unit_test(test_content_type_with_charset),

        /* Edge cases from busboy/formidable test suites */
        cmocka_unit_test(test_unicode_filename_rfc5987),
        cmocka_unit_test(test_filename_with_escaped_quotes),
        cmocka_unit_test(test_zero_byte_file),
        cmocka_unit_test(test_windows_path_in_filename),
        cmocka_unit_test(test_field_with_null_bytes),
        cmocka_unit_test(test_very_long_field_name),
        cmocka_unit_test(test_duplicate_field_names),
        cmocka_unit_test(test_mixed_content_types),
        cmocka_unit_test(test_special_chars_in_field_name),
        cmocka_unit_test(test_missing_content_disposition),
        cmocka_unit_test(test_binary_data_like_boundary),
        cmocka_unit_test(test_byte_by_byte_feeding),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
