#ifndef FUZZ_HARNESS_COMMON_H
#define FUZZ_HARNESS_COMMON_H

#include <stdbool.h>

/* libFuzzer entry points. Defined in harness_common.c for
 * Zend-dependent harnesses; individual parser harnesses provide
 * LLVMFuzzerTestOneInput themselves. */
int LLVMFuzzerInitialize(int *argc, char ***argv);

#endif
