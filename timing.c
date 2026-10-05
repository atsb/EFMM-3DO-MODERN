#include <stdio.h>
#include <string.h>
#include "sdl3_3do.h"
#include "time.h"
#include "castle.h"
#include "app_proto.h"

void drawnumxy (bmi, num, x, y)
Item bmi;
int32 num, x, y;
{
    GrafCon gc;
    char str[64];
    (void)x; (void)y;
    sprintf(str, "%ld", (long)num);
    MoveTo(&gc,x,y);
    DrawText8(&gc,bmi,str);
}

void opentimer () { }

void gettime (tv)
struct timeval *tv;
{
    uint64 t;
    t=SDL_GetTicks();
    tv->tv_sec=(int32)(t/1000);
    tv->tv_usec=(int32)((t%1000)*1000);
}

int32 subtime (tv1,tv2)
register struct timeval *tv1,*tv2;
{
    return (tv1->tv_sec-tv2->tv_sec)*1000000 + tv1->tv_usec-tv2->tv_usec;
}
