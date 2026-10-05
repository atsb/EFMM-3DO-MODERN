#include <types.h>
#include <mem.h>
#include <filestream.h>
#include <debug.h>
#include <string.h>
#include <stdlib.h>

#include "castle.h"
#include "loaf.h"
#include "app_proto.h"

#define VERSION 2

extern uint32 ccbextra;

static uint16 be16(const ubyte* p)
{
    return (uint16)(((uint16)p[0] << 8) | p[1]);
}

static uint32 be32(const ubyte* p)
{
    return ((uint32)p[0] << 24) |
        ((uint32)p[1] << 16) |
        ((uint32)p[2] << 8) |
        (uint32)p[3];
}

#define DISK_IMAGE_ENTRY_SIZE 12
#define DISK_SHORT_CCB_SIZE   24
#define DISK_ANIM_ENTRY_SIZE  12

struct ImageEnv* loadloaf(filename)
    char* filename;
{
    ubyte* buf;
    int32 len;
    intptr_t errlen;
    uint32 off, nimages, nccbs, npluts;
    uint32 i;
    ImageEnv* iev;
    uint32* animBaseIdx;
    uint16* animFrames;
    uint16* animFPS;
    uint32* imagePLUTIdx;
    uint32 pdatTotal;
    uint32 plutTotal;

    if (!(buf = (ubyte*)allocloadfile(filename, MEMTYPE_CEL, &errlen)))
        return NULL;

    len = (int32)errlen;
    animBaseIdx = NULL;
    animFrames = NULL;
    animFPS = NULL;
    imagePLUTIdx = NULL;
    iev = NULL;

    /* LOAF header: 'LOAF', version, number of image entries. */
    if (len < 12)
        goto bad;
    if (be32(buf) != 0x4c4f4146u)
        goto bad;
    if (be32(buf + 4) > VERSION)
        goto bad;

    nimages = be32(buf + 8);
    if (nimages > 4096u)
        goto bad;

    off = 12;
    if (off + nimages * DISK_IMAGE_ENTRY_SIZE > (uint32)len)
        goto bad;

    iev = (ImageEnv*)AllocMem(sizeof(ImageEnv), MEMTYPE_FILL);
    if (!iev)
        goto bad;

    iev->iev_NImageEntries = (int32)nimages;

    if (nimages) {
        iev->iev_ImageEntries = (ImageEntry*)
            AllocMem(sizeof(ImageEntry) * nimages, MEMTYPE_FILL);
        if (!iev->iev_ImageEntries)
            goto bad;

        imagePLUTIdx = (uint32*)
            AllocMem(sizeof(uint32) * nimages, MEMTYPE_FILL);
        if (!imagePLUTIdx)
            goto bad;

        for (i = 0; i < nimages; ++i) {
            uint32 sccbIdx = be32(buf + off + 0);
            uint32 plutIdx = be32(buf + off + 4);
            uint32 ppmPC = be32(buf + off + 8);

            iev->iev_ImageEntries[i].ie_SCCB = NULL;
            iev->iev_ImageEntries[i].ie_PLUT = NULL;
            iev->iev_ImageEntries[i].ie_PPMPC = ppmPC;
            imagePLUTIdx[i] = plutIdx;

            if (sccbIdx == 0xffffffffu) {
                ++iev->iev_NAnimEntries;
            }
            off += DISK_IMAGE_ENTRY_SIZE;
        }

        if (iev->iev_NAnimEntries) {
            iev->iev_AnimEntries = (AnimEntry*)
                AllocMem(sizeof(AnimEntry) * iev->iev_NAnimEntries, MEMTYPE_FILL);
            animBaseIdx = (uint32*)
                AllocMem(sizeof(uint32) * iev->iev_NAnimEntries, MEMTYPE_FILL);
            animFrames = (uint16*)
                AllocMem(sizeof(uint16) * iev->iev_NAnimEntries, MEMTYPE_FILL);
            animFPS = (uint16*)
                AllocMem(sizeof(uint16) * iev->iev_NAnimEntries, MEMTYPE_FILL);

            if (!iev->iev_AnimEntries || !animBaseIdx || !animFrames || !animFPS)
                goto bad;

            {
                uint32 ai = 0;
                for (i = 0; i < nimages; ++i) {
                    uint32 pos = 12 + i * DISK_IMAGE_ENTRY_SIZE;
                    if (be32(buf + pos) == 0xffffffffu) {
                        animBaseIdx[ai] = be32(buf + pos + 4);
                        animFrames[ai] = be16(buf + pos + 8);
                        animFPS[ai] = be16(buf + pos + 10);
                        iev->iev_AnimEntries[ai].ae_Target =
                            iev->iev_ImageEntries + i;
                        ++ai;
                    }
                }
            }
        }
    }

    /* Number of unique ShortCCBs. */
    if (off + 4 > (uint32)len)
        goto bad;
    nccbs = be32(buf + off);
    off += 4;
    if (nccbs > 4096u)
        goto bad;
    if (off + nccbs * DISK_SHORT_CCB_SIZE > (uint32)len)
        goto bad;

    iev->iev_NSCCBs = (int32)nccbs;
    pdatTotal = 0;

    if (nccbs) {
        iev->iev_SCCBs = (ShortCCB*)
            AllocMem(sizeof(ShortCCB) * nccbs, MEMTYPE_FILL);
        if (!iev->iev_SCCBs)
            goto bad;

        for (i = 0; i < nccbs; ++i) {
            const ubyte* src = buf + off + i * DISK_SHORT_CCB_SIZE;
            uint32 size = be32(src + 20);
            ShortCCB* dst = &iev->iev_SCCBs[i];

            dst->sccb_Flags = be32(src + 0);
            dst->sccb_PRE0 = be32(src + 4);
            dst->sccb_PRE1 = be32(src + 8);
            dst->sccb_Width = (int32)be32(src + 12);
            dst->sccb_Height = (int32)be32(src + 16);
            dst->sccb_PDAT = NULL;

            if (size > 0x01000000u || pdatTotal > 0x7fffffffu - size)
                goto bad;
            pdatTotal += size;
        }

        off += nccbs * DISK_SHORT_CCB_SIZE;
    }

    /* Copy the packed pixel data exactly as produced by loafit. */
    if (pdatTotal) {
        if (off + pdatTotal > (uint32)len)
            goto bad;

        iev->iev_PDATBufSiz = (int32)pdatTotal;
        iev->iev_PDATBuf = (ubyte*)AllocMem(pdatTotal, MEMTYPE_CEL);
        if (!iev->iev_PDATBuf)
            goto bad;

        {
            ubyte* dst = iev->iev_PDATBuf;
            for (i = 0; i < nccbs; ++i) {
                uint32 size = be32(buf + 12 + nimages * DISK_IMAGE_ENTRY_SIZE + 4 +
                    i * DISK_SHORT_CCB_SIZE + 20);

                memcpy(dst, buf + off, size);
                iev->iev_SCCBs[i].sccb_PDAT = dst;
                dst += size;
                off += size;

                iev->iev_SCCBs[i].sccb_Flags |=
                    CCB_NPABS | CCB_SPABS | CCB_PPABS |
                    CCB_LDSIZE | CCB_LDPRS | CCB_LDPPMP |
                    CCB_YOXY | CCB_ACW | CCB_ACCW | ccbextra;
                iev->iev_SCCBs[i].sccb_Flags &= ~CCB_TWD;
                iev->iev_SCCBs[i].sccb_Width = cvt2power(iev->iev_SCCBs[i].sccb_Width);
                iev->iev_SCCBs[i].sccb_Height = cvt2power(iev->iev_SCCBs[i].sccb_Height);
            }
        }
    }

    /* Resolve ImageEntry -> ShortCCB links. */
    for (i = 0; i < nimages; ++i) {
        uint32 sccbIdx = be32(buf + 12 + i * DISK_IMAGE_ENTRY_SIZE);
        if (sccbIdx != 0xffffffffu) {
            if (sccbIdx >= nccbs)
                goto bad;
            iev->iev_ImageEntries[i].ie_SCCB = &iev->iev_SCCBs[sccbIdx];
        }
    }

    /* Unique PLUT count and total payload size. */
    if (off + 8 > (uint32)len)
        goto bad;
    npluts = be32(buf + off);
    off += 4;
    plutTotal = be32(buf + off);
    off += 4;
    if (npluts > 4096u)
        goto bad;
    if (off + plutTotal > (uint32)len)
        goto bad;

    iev->iev_PLUTBufSiz = (int32)plutTotal;
    if (plutTotal) {
        iev->iev_PLUTBuf = (uint16*)AllocMem(plutTotal, MEMTYPE_CEL);
        if (!iev->iev_PLUTBuf)
            goto bad;
        memcpy(iev->iev_PLUTBuf, buf + off, plutTotal);
    }
    off += plutTotal;

    /* The final PLUT size table provides one uint32 per unique PLUT. */
    if (off + npluts * 4u > (uint32)len)
        goto bad;

    for (i = 0; i < nimages; ++i) {
        uint32 sccbIdx = be32(buf + 12 + i * DISK_IMAGE_ENTRY_SIZE);
        uint32 idx = imagePLUTIdx ? imagePLUTIdx[i] : 0xffffffffu;

        /*
         * Animation marker entries deliberately overlay the normal
         * DiskImageEntry layout:
         *
         *   die_ShortCCBIdx = ~0
         *   die_PLUTIdx     = first image index
         *   die_PPMP        = frame count << 16 | FPS
         *
         * Therefore die_PLUTIdx is NOT a PLUT index for these entries.
         * Do not validate or resolve it as one.
         */
        if (sccbIdx == 0xffffffffu)
            continue;

        if (idx != 0xffffffffu) {
            uint32 cursorBytes = 0;
            uint32 j;
            if (idx >= npluts)
                goto bad;

            for (j = 0; j < idx; ++j) {
                uint32 entries = be32(buf + off + j * 4u);
                if (entries > 0x7fffffffu / sizeof(uint16))
                    goto bad;
                cursorBytes += entries * (uint32)sizeof(uint16);
            }
            if (cursorBytes > plutTotal)
                goto bad;
            iev->iev_ImageEntries[i].ie_PLUT =
                (uint16*)((ubyte*)iev->iev_PLUTBuf + cursorBytes);
        }
    }

    /* Complete animation entries now that the image table is populated. */
    if (iev->iev_NAnimEntries) {
        uint32 ai;
        for (ai = 0; ai < (uint32)iev->iev_NAnimEntries; ++ai) {
            uint32 base = animBaseIdx[ai];
            if (base >= nimages)
                goto bad;
            iev->iev_AnimEntries[ai].ae_Base = iev->iev_ImageEntries + base;
            iev->iev_AnimEntries[ai].ae_NFrames = (int32)animFrames[ai];
            iev->iev_AnimEntries[ai].ae_FPS = (int32)animFPS[ai];
        }
    }

    if (animBaseIdx) FreeMem(animBaseIdx, sizeof(uint32) * iev->iev_NAnimEntries);
    if (animFrames) FreeMem(animFrames, sizeof(uint16) * iev->iev_NAnimEntries);
    if (animFPS) FreeMem(animFPS, sizeof(uint16) * iev->iev_NAnimEntries);
    if (imagePLUTIdx) FreeMem(imagePLUTIdx, sizeof(uint32) * nimages);
    FreeMem(buf, len);
    return iev;

bad:
    if (animBaseIdx && iev) FreeMem(animBaseIdx, sizeof(uint32) * iev->iev_NAnimEntries);
    if (animFrames && iev) FreeMem(animFrames, sizeof(uint16) * iev->iev_NAnimEntries);
    if (animFPS && iev) FreeMem(animFPS, sizeof(uint16) * iev->iev_NAnimEntries);
    if (imagePLUTIdx) FreeMem(imagePLUTIdx, sizeof(uint32) * nimages);
    if (iev) freeloaf(iev);
    if (buf) FreeMem(buf, len);
    return NULL;
}

void freeloaf(iev)
register struct ImageEnv* iev;
{
    if (!iev) return;
    if (iev->iev_PLUTBuf) FreeMem(iev->iev_PLUTBuf, iev->iev_PLUTBufSiz);
    if (iev->iev_PDATBuf) FreeMem(iev->iev_PDATBuf, iev->iev_PDATBufSiz);
    if (iev->iev_SCCBs) FreeMem(iev->iev_SCCBs, sizeof(ShortCCB) * iev->iev_NSCCBs);
    if (iev->iev_AnimEntries) FreeMem(iev->iev_AnimEntries, sizeof(AnimEntry) * iev->iev_NAnimEntries);
    if (iev->iev_ImageEntries) FreeMem(iev->iev_ImageEntries, sizeof(ImageEntry) * iev->iev_NImageEntries);
    FreeMem(iev, sizeof(*iev));
}
