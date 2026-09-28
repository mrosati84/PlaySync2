#include "json_mut.h"

#include <stdlib.h>

char *jm_print(const cJSON *node)
{
    if (node == NULL)
        return NULL;
    return cJSON_PrintUnformatted(node);
}

void jm_free(char *s)
{
    free(s);
}
