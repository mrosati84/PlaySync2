#ifndef PS_JSON_MUT_H
#define PS_JSON_MUT_H

#include "cJSON.h"

/* Minified JSON writer: one line, no whitespace, no literal newline. */
char *jm_print(const cJSON *node);
void jm_free(char *s);

#endif
