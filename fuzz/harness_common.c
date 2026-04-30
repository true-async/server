/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

/*
 * Shared libFuzzer bootstrap for harnesses that need a live Zend
 * runtime (emalloc / zend_string / HashTable). Uses the php_embed
 * SAPI to bring PHP up once in LLVMFuzzerInitialize; subsequent
 * TestOneInput invocations run in a fully-warm interpreter.
 *
 * libFuzzer's API: Initialize runs once per process before the fuzz
 * loop; TestOneInput runs millions of times in the same process. We
 * match that lifetime: php_embed_init once, php_embed_shutdown at
 * exit (registered via atexit so libFuzzer's own exit path triggers
 * it).
 *
 * Harnesses link against this TU + libphp. Keep it independent of
 * our extension's own module registration — individual harnesses
 * can touch any of our parser/state-machine APIs directly, as long
 * as they don't require the HTTP extension to be MINIT'd (no
 * http_exception_ce needed at parser level).
 */

#include "harness_common.h"

#include <sapi/embed/php_embed.h>
#include <stdlib.h>

/* Pull in the http_server module-globals struct definition + the
 * ts_rsrc_id declaration. The fuzz_stubs.c TU defines the id symbol;
 * here we register the slot with TSRM after php_embed_init so that
 * HTTP_SERVER_G(...) reads land in a properly-allocated zeroed struct
 * instead of an uninitialised slot adjacent to TSRM bookkeeping (the
 * h2 data-chunk path reads parser_pool.max_body_size — caught by
 * ASAN as heap-buffer-overflow during fuzzing). */
#include "php_http_server.h"

static bool php_is_up = false;

static void fuzz_atexit_shutdown(void)
{
    if (php_is_up) {
        php_embed_shutdown();
        php_is_up = false;
    }
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;

    if (!php_is_up) {
        /* Raise memory_limit so the Zend allocator doesn't bail mid-
         * fuzz on cumulative allocation across millions of iterations.
         * LeakSanitizer still catches real per-iteration leaks at exit;
         * the limit here is only about not terminating a productive
         * fuzz session early. Set via INI override before php_embed_init
         * reads the default. */
        putenv("PHP_INI_OVERRIDE=memory_limit=-1");
        php_embed_init(0, NULL);
        /* Belt-and-braces: also set at runtime in case the env var path
         * didn't take (php_embed reads main INI first). */
        zend_alter_ini_entry_chars(
            zend_string_init("memory_limit", sizeof("memory_limit") - 1, 0),
            "-1", 2,
            ZEND_INI_SYSTEM, ZEND_INI_STAGE_ACTIVATE);
        php_is_up = true;
        atexit(fuzz_atexit_shutdown);

        /* Register the http_server module-globals slot. Without this
         * the H2 session reads HTTP_SERVER_G(parser_pool) off slot 0
         * (the weak fuzz_stubs.c default), which is TSRM internal —
         * ASAN flags as heap-buffer-overflow. Allocate properly so
         * the slot is a zeroed zend_http_server_globals instead. */
#ifdef ZTS
        ts_allocate_id(&http_server_globals_id,
                       sizeof(zend_http_server_globals),
                       NULL, NULL);
#endif
    }
    return 0;
}
