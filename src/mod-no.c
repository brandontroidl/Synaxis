/* mod-no.c — Null module stub.
 * Copyright (c) Cathexis Development
 */
#include <stddef.h>
const char *no_module_deps[] = { NULL };
int no_init(void) { return 1; }
int no_finalize(void) { return 1; }
