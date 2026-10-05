#include "sdl3_3do.h"
#include "castle.h"
#include "app_proto.h"

JoyData jd;
int32 joytrigger;
int32 oldjoybits=0;

void joythreadfunc ()
{
    /* The original ran a 60Hz kernel thread. SDL3 drives the same accumulator
       from the main-thread VBlank shim, preserving the game's frame-count API. */
}

void resetjoydata ()
{
    jd.jd_DX=jd.jd_DZ=jd.jd_DAng=0;
    jd.jd_ADown=jd.jd_BDown=jd.jd_CDown=jd.jd_XDown=jd.jd_StartDown=0;
    jd.jd_FrameCount=0;
    joytrigger=0;
}
