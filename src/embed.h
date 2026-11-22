/* embed.h: files built into voxup.exe (embed.c) */
#ifndef EMBED_H
#define EMBED_H
#include <stddef.h>
const char *embed_get(const char *name, size_t *n);   /* NULL when not built in; the text ends in a 0 */
int embed_count(void);
#endif
