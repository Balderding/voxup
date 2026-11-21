/* nk_impl.c: the third-party code voxup's window uses, compiled once on its own (and without our warnings):
   Nuklear + its GDI backend (the window), stb_image / stb_image_write (PNG in and out). Versions: third_party\VERSIONS.txt */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "nk_conf.h"
#define NK_IMPLEMENTATION
#define NK_GDI_IMPLEMENTATION
#include "third_party/nuklear.h"
#include "third_party/nuklear_gdi.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

/* the backend keeps its drawing surface to itself; the off-screen self-test needs it */
HDC nk_gdi_memdc(void) { return gdi.memory_dc; }
HBITMAP nk_gdi_membitmap(void) { return gdi.bitmap; }
