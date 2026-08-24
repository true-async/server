/* Unit tests for what a response says about its own framing — the
 * Content-Length rule and the HTTP/2/HTTP/3 header filter that carries its
 * answer out. Both read their arguments and nothing else, so the cases below
 * are the rule itself rather than a wire that happens to agree with it.
 *
 * The rule is the one #197, #198, #200, #202 and #235 kept touching, and until
 * now only phpt held it, one shape per test. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <php.h>
#include "php_http_server.h"

/* A buffered 200 answering a GET, which is the shape everything else varies. */
static http_response_length_inputs_t buffered_get(void)
{
    const http_response_length_inputs_t in = {
        .status          = 200,
        .declared_length = -1,
    };

    return in;
}

/* ---- statuses that answer before anything else is read ---- */

static void test_no_body_statuses(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    /* RFC 9110 §8.6: forbidden on 1xx and 204. */
    in.status = 100;
    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_OMIT);
    in.status = 204;
    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_OMIT);

    /* Permitted on a 304, where it describes what a 200 would have carried, so
     * whatever the handler set survives. */
    in.status = 304;
    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_KEEP);

    /* RFC 9110 §15.3.6 forbids content in a 205 while a receiver still looks
     * for framing, so it is the one status that states a zero. */
    in.status = 205;
    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_ZERO);
}

/* A 205 states zero even when the handler is streaming: the status answers
 * before the mode is read. */
static void test_zero_length_status_outranks_the_mode(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    in.status          = 205;
    in.streaming       = true;
    in.declared_length = 4096;

    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_ZERO);
}

/* ---- a body the buffer holds ---- */

static void test_buffered_body_is_measured(void **state)
{
    (void)state;
    const http_response_length_inputs_t in = buffered_get();

    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_FROM_BODY);
}

/* The static engine writes a file size into the table while the buffer stays
 * empty, so a measured count would answer zero over a body of 4096 bytes. */
static void test_a_count_the_server_stated_is_kept(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    in.length_stated = true;

    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_KEEP);
}

/* ---- a stream, which has no buffer to measure ---- */

static void test_declared_stream_keeps_its_count(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    in.streaming       = true;
    in.declared_length = 1024;

    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_KEEP);
}

static void test_undeclared_stream_states_nothing(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    in.streaming = true;

    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_OMIT);
}

/* ---- HEAD, where the buffer holds the body a GET would have returned ---- */

static void test_head_measures_the_body_it_will_not_send(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    in.is_head = true;

    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_FROM_BODY);
}

/* A HEAD whose handler streamed had its chunks dropped, so the empty buffer
 * measures nothing and only a field already in the table can answer. */
static void test_head_that_streamed_states_only_what_it_holds(void **state)
{
    (void)state;
    http_response_length_inputs_t in = buffered_get();

    in.is_head       = true;
    in.head_streamed = true;
    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_OMIT);

    in.table_has_length = true;
    assert_int_equal(http_response_length_action_for(&in), HTTP_RESPONSE_LENGTH_KEEP);
}

/* ---- the filter that carries the answer onto an HTTP/2 or HTTP/3 wire ---- */

static void test_hop_by_hop_names_are_dropped(void **state)
{
    (void)state;

    assert_false(http_response_header_allowed_h2h3("connection", 10, true));
    assert_false(http_response_header_allowed_h2h3("keep-alive", 10, true));
    assert_false(http_response_header_allowed_h2h3("upgrade", 7, true));
    assert_false(http_response_header_allowed_h2h3("transfer-encoding", 17, true));

    /* Neighbours of the same length are not the name and go out. */
    assert_true(http_response_header_allowed_h2h3("connectio1", 10, true));
    assert_true(http_response_header_allowed_h2h3("content-type", 12, true));
}

static void test_content_length_follows_the_framing_answer(void **state)
{
    (void)state;

    /* DATA frames bound the body, so the field is dropped unless the response
     * is stating a count of its own. */
    assert_false(http_response_header_allowed_h2h3("content-length", 14, false));
    assert_true(http_response_header_allowed_h2h3("content-length", 14, true));
}

/* Header names reach the filter as the handler wrote them. */
static void test_names_are_matched_case_insensitively(void **state)
{
    (void)state;

    assert_false(http_response_header_allowed_h2h3("Connection", 10, true));
    assert_false(http_response_header_allowed_h2h3("Transfer-Encoding", 17, true));
    assert_false(http_response_header_allowed_h2h3("Content-Length", 14, false));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_no_body_statuses),
        cmocka_unit_test(test_zero_length_status_outranks_the_mode),
        cmocka_unit_test(test_buffered_body_is_measured),
        cmocka_unit_test(test_a_count_the_server_stated_is_kept),
        cmocka_unit_test(test_declared_stream_keeps_its_count),
        cmocka_unit_test(test_undeclared_stream_states_nothing),
        cmocka_unit_test(test_head_measures_the_body_it_will_not_send),
        cmocka_unit_test(test_head_that_streamed_states_only_what_it_holds),
        cmocka_unit_test(test_hop_by_hop_names_are_dropped),
        cmocka_unit_test(test_content_length_follows_the_framing_answer),
        cmocka_unit_test(test_names_are_matched_case_insensitively),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
