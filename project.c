#include <stdint.h>
#include <sdl3_3do.h>
#include "castle.h"

/* C translation of project.asm */
static int32 arm_add32(int32 a, int32 b)
{
    return (int32)((uint32)a + (uint32)b);
}

static int32 arm_mul32(int32 a, int32 b)
{
    return (int32)((uint32)a * (uint32)b);
}

static int32 arm_asr32(int32 v, unsigned shift)
{
    uint32 u;
    if (shift >= 32)
        return v < 0 ? -1 : 0;
    u = (uint32)v;
    if (shift == 0)
        return v;
    if (v >= 0)
        return (int32)(u >> shift);
    return (int32)(~((~u) >> shift));
}

static int32 arm_lsl32(int32 v, unsigned shift)
{
    if (shift >= 32)
        return 0;
    return (int32)((uint32)v << shift);
}

void
    project(src, dest, magic, zpull, cx, cy, npoints)
        Vertex *src;
Vertex *dest;
int32 magic, zpull, cx, cy, npoints;
{
    int32 i;
    int32 magic15 = arm_lsl32(magic, 15);

    for (i = 0; i < npoints; ++i)
    {
        int32 x = src[i].X;
        int32 y = src[i].Y;
        int32 z = arm_add32(src[i].Z, zpull);
        int32 zr;
        int32 scale;
        int32 mx, my;

        /* ADDS z,z,zpull; MOVLE z,#0x80000000; BLE writexyz */
        if (z <= 0)
        {
            dest[i].X = x;
            dest[i].Y = y;
            dest[i].Z = (int32)0x80000000u;
            continue;
        }

        /* ARM discards the low 8 fractional bits
         * before the signed divide. */
        zr = arm_asr32(z, 8);
        if (zr <= 0)
        {
            dest[i].X = (int32)0x80000000u;
            dest[i].Y = (int32)0x80000000u;
            dest[i].Z = (int32)0x80000000u;
            continue;
        }

        /* __rt_sdiv(magic<<15, z)*/
        scale = magic15 / zr;

        /* ARM MUL keeps 32 bits*/
        mx = arm_asr32(arm_mul32(scale, arm_asr32(x, 8)), 15);
        my = arm_asr32(arm_mul32(scale, arm_asr32(y, 8)), 15);
        dest[i].X = arm_add32(cx, mx);
        dest[i].Y = arm_add32(cy, my);
        dest[i].Z = zr;
    }
}
