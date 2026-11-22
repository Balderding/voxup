/* embed.c: the model list (models\models.txt) and its ComfyUI graphs (models\ID.json), inside voxup.exe, so it
   needs no models folder. A models folder beside the exe (or voxup.ini models =) still wins, file by file.
   A new graph in models\ is listed here too. C23 #embed (clang 19+). */
#include <stddef.h>
#include <string.h>
#include "embed.h"

static const char E0[] = {
#embed "../models/boogu.json" suffix(, 0)
};
static const char E1[] = {
#embed "../models/joyai.json" suffix(, 0)
};
static const char E2[] = {
#embed "../models/klein.json" suffix(, 0)
};
static const char E3[] = {
#embed "../models/klein4f.json" suffix(, 0)
};
static const char E4[] = {
#embed "../models/klein9kv.json" suffix(, 0)
};
static const char E5[] = {
#embed "../models/kontext.json" suffix(, 0)
};
static const char E6[] = {
#embed "../models/longcat.json" suffix(, 0)
};
static const char E7[] = {
#embed "../models/mageturbo.json" suffix(, 0)
};
static const char E8[] = {
#embed "../models/models.txt" suffix(, 0)
};
static const char E9[] = {
#embed "../models/omnigen2.json" suffix(, 0)
};
static const char E10[] = {
#embed "../models/qwen2511.json" suffix(, 0)
};
static const char E11[] = {
#embed "../models/qwen2511f.json" suffix(, 0)
};
static const char E12[] = {
#embed "../models/sdxlpix.json" suffix(, 0)
};
static const struct { const char *name; const char *data; size_t n; } FILES[] = {
    { "boogu.json", E0, sizeof E0 - 1 },
    { "joyai.json", E1, sizeof E1 - 1 },
    { "klein.json", E2, sizeof E2 - 1 },
    { "klein4f.json", E3, sizeof E3 - 1 },
    { "klein9kv.json", E4, sizeof E4 - 1 },
    { "kontext.json", E5, sizeof E5 - 1 },
    { "longcat.json", E6, sizeof E6 - 1 },
    { "mageturbo.json", E7, sizeof E7 - 1 },
    { "models.txt", E8, sizeof E8 - 1 },
    { "omnigen2.json", E9, sizeof E9 - 1 },
    { "qwen2511.json", E10, sizeof E10 - 1 },
    { "qwen2511f.json", E11, sizeof E11 - 1 },
    { "sdxlpix.json", E12, sizeof E12 - 1 },
};
const char *embed_get(const char *name, size_t *n) {
    size_t i;
    for (i = 0; i < sizeof FILES / sizeof FILES[0]; i++) if (!strcmp(FILES[i].name, name)) { if (n) *n = FILES[i].n; return FILES[i].data; }
    return NULL;
}
int embed_count(void) { return (int)(sizeof FILES / sizeof FILES[0]); }
