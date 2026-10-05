#include "sdl3_3do.h"
/* El Kabong is a 3DO filesystem quirk workaround. SDL3 uses the host filesystem directly. */
int32 performElKabong(void *buf) { (void)buf; return 0; }
int32 initElKabong(char *filename) { (void)filename; return 0; }
void closeElKabong(void) { }
int elKabongRequired(void) { return 0; }
