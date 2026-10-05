#include <stddef.h>
#include "sdl3_3do.h"
#include "castle.h"

/* Direct C translation of misc.asm */
void
    resetlinebuf(dummy)
        uint32 *dummy;
{
    int i;
    if (!dummy)
        return;
    for (i = 0; i < 10; ++i)
        dummy[i] = 0xFFFFFFFFu;
}

int islinefull(dummy)
uint32 *dummy;
{
    int i;
    if (!dummy)
        return TRUE;
    for (i = 0; i < 10; ++i)
        if (dummy[i] != 0)
            return FALSE;
    return TRUE;
}

int testmarklinebuf(dummy, lx, rx)
uint32 *dummy;
register int32 lx, rx;
{
    uint32 lm, rm;
    int result = FALSE;

    if (!dummy)
        return FALSE;

    /* The assembly treats an empty interval as an immediate FALSE. */
    if (lx == rx)
        return FALSE;

    /* Factor in the MapCel artefact the right endpoint is at the
     * C level, so convert it to the pixel used by the assembler. */
    rx--;
    if (lx > rx)
    {
        int32 t = lx;
        lx = rx;
        rx = t;
    }

    if (lx >= 320 || rx < 0)
        return FALSE;

    if (lx < 0)
        lx = 0;
    if (rx >= 320)
        rx = 319;

    /* Same masks as leftmasks/rightmasks in misc.asm. */
    lm = 0xFFFFFFFFu << (lx & 31);
    rm = 0xFFFFFFFFu >> (31 - (rx & 31));
    lx >>= 5;
    rx >>= 5;

    if (lx == rx)
    {
        uint32 mask = lm & rm;
        uint32 tmp = dummy[lx];
        if (tmp & mask)
        {
            dummy[lx] = tmp & ~mask;
            return TRUE;
        }
        return FALSE;
    }

    {
        uint32 tmp = dummy[lx];
        if (tmp & lm)
            result = TRUE;
        dummy[lx] = tmp & ~lm;
    }

    while (++lx < rx)
    {
        if (dummy[lx])
            result = TRUE;
        dummy[lx] = 0;
    }

    {
        uint32 tmp = dummy[rx];
        if (tmp & rm)
            result = TRUE;
        dummy[rx] = tmp & ~rm;
    }

    return result;
}

void
    mkVertPtrs(verts, ptrArray, idx0, idx1, idx2, idx3)
        Vertex *verts;
Vertex **ptrArray;
int32 idx0, idx1, idx2, idx3;
{
    /* The original ARM computes idx * 3 * sizeof(LONG) */
    ptrArray[0] = verts + idx0;
    ptrArray[1] = verts + idx1;
    ptrArray[2] = verts + idx2;
    ptrArray[3] = verts + idx3;
}
