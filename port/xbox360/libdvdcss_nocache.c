#include <stdlib.h>
#include <string.h>

/* Keep libdvdcss's in-session keys, but don't load or save title keys on disk. */
static char *dvd360_css_getenv(const char *name)
{
    static char cache_off[] = "off";
    return strcmp(name, "DVDCSS_CACHE") == 0 ? cache_off : getenv(name);
}

#define getenv dvd360_css_getenv
#include "../../deps/libdvdcss/src/libdvdcss.c"
#undef getenv
