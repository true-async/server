/*
  +----------------------------------------------------------------------+
  | Copyright (c) TrueAsync                                              |
  +----------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0                       |
  +----------------------------------------------------------------------+
*/

#ifndef FAULT_HOOKS_H
#define FAULT_HOOKS_H

/*
 * PHP control over the libfiu fault points compiled into the extension.
 * Registers `_http_fault_enable()`, `_http_fault_disable()`,
 * `_http_fault_hits()` and `_http_fault_fail()` when the extension is built
 * with FIU_ENABLE (--enable-fault-injection); without that flag this is a
 * no-op, the points compile to nothing and no libfiu symbol is linked. Called
 * unconditionally from MINIT.
 */
void fault_hooks_register(const int module_type);

#endif /* FAULT_HOOKS_H */
