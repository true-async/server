/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef REACTOR_POOL_TEST_H
#define REACTOR_POOL_TEST_H

/*
 * Test-only entry point for the reactor pool (#80). Registers the
 * `_http_server_reactor_pool_selftest()` PHP function used by the phpt
 * substrate test. Compiled in only when the extension is built with
 * -DHTTP_SERVER_TEST_HOOKS (--enable-http-server-test-hooks); without that
 * flag this is a no-op and the hook is absent from the build, so it never
 * ships in a release. Called unconditionally from MINIT.
 */
void reactor_pool_test_register(const int module_type);

#endif /* REACTOR_POOL_TEST_H */
