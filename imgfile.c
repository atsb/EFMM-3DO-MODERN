#include <types.h>
#include <mem.h>
#include <graphics.h>
#include <form3do.h>
#include <string.h>

#include "castle.h"
#include "imgfile.h"
#include "app_proto.h"

static uint16 be16(const ubyte* p)
{
    return (uint16)(((uint16)p[0] << 8) | p[1]);
}
static uint32 be32(const ubyte* p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}
static int32 as_signed32(uint32 v) { return (int32)v; }
static uint32 chunk_type(const ubyte* p) { return be32(p); }
static uint32 chunk_size(const ubyte* p) { return be32(p + 4); }

/* Keep CEL source lengths outside CCB so the 3DO CCB layout stays unchanged. */
#define CEL_SOURCE_REGISTRY_CAPACITY 2048
static struct {
    const CCB* ccb;
    uint32 bytes;
} gCelSourceRegistry[CEL_SOURCE_REGISTRY_CAPACITY];

static void register_cel_source(const CCB* ccb, uint32 bytes)
{
    int i, freeSlot = -1;
    if (!ccb || !bytes) return;
    for (i = 0; i < CEL_SOURCE_REGISTRY_CAPACITY; ++i) {
        if (gCelSourceRegistry[i].ccb == ccb) {
            gCelSourceRegistry[i].bytes = bytes;
            return;
        }
        if (!gCelSourceRegistry[i].ccb && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot >= 0) {
        gCelSourceRegistry[freeSlot].ccb = ccb;
        gCelSourceRegistry[freeSlot].bytes = bytes;
    }
}

static void unregister_cel_source(const CCB* ccb)
{
    int i;
    if (!ccb) return;
    for (i = 0; i < CEL_SOURCE_REGISTRY_CAPACITY; ++i) {
        if (gCelSourceRegistry[i].ccb == ccb) {
            gCelSourceRegistry[i].ccb = NULL;
            gCelSourceRegistry[i].bytes = 0;
            return;
        }
    }
}

uint32 SDL3_3DO_GetCelSourceBytes(const CCB* ccb)
{
    int i;
    if (!ccb) return 0;
    for (i = 0; i < CEL_SOURCE_REGISTRY_CAPACITY; ++i)
        if (gCelSourceRegistry[i].ccb == ccb)
            return gCelSourceRegistry[i].bytes;
    return 0;
}

static void read_ccb_fields(CCB* c, const ubyte* p)
{

    memset(c, 0, sizeof(*c));
    c->ccb_Flags = be32(p + 4);
    c->ccb_XPos = as_signed32(be32(p + 20));
    c->ccb_YPos = as_signed32(be32(p + 24));
    c->ccb_HDX = as_signed32(be32(p + 28));
    c->ccb_HDY = as_signed32(be32(p + 32));
    c->ccb_VDX = as_signed32(be32(p + 36));
    c->ccb_VDY = as_signed32(be32(p + 40));
    c->ccb_HDDX = as_signed32(be32(p + 44));
    c->ccb_HDDY = as_signed32(be32(p + 48));
    c->ccb_PIXC = be32(p + 52);
    c->ccb_PRE0 = be32(p + 56);
    c->ccb_PRE1 = be32(p + 60);
    c->ccb_Width = as_signed32(be32(p + 64));
    c->ccb_Height = as_signed32(be32(p + 68));
    c->ccb_SDLAssetBE = 1;
    c->ccb_SDLSourceHasPreamble = ((c->ccb_Flags & CCB_CCBPRE) == 0);
}

struct CelArray* alloccelarray(nentries)
    int32 nentries;
{
    if (nentries <= 0)nentries = 1;
    return (CelArray*)AllocMem(sizeof(CelArray) + (nentries - 1) * sizeof(CCB*), MEMTYPE_FILL);
}

static void prepare_ccb(CCB* ccb, void* plut, void* pdat)
{
    if (ccb->ccb_Flags & CCB_LDPLUT) { ccb->ccb_PLUTPtr = plut;ccb->ccb_Flags |= CCB_PPABS; }
    ccb->ccb_SourcePtr = (CelData*)pdat;
    ccb->ccb_Flags |= CCB_SPABS;
    ccb->ccb_HDX = ONE_HD;ccb->ccb_VDY = ONE_VD;
    ccb->ccb_HDY = ccb->ccb_VDX = ccb->ccb_HDDX = ccb->ccb_HDDY = 0;
    ccb->ccb_Width = cvt2power(ccb->ccb_Width);
    ccb->ccb_Height = cvt2power(ccb->ccb_Height);
}

static int count_cat(const ubyte* base, int32 len)
{
    int32 off = 0, n = 0;uint32 sz, type;
    while (off + 8 <= len) { type = chunk_type(base + off);sz = chunk_size(base + off);if (sz < 8 || sz > (uint32)(len - off))break;if (type == CHUNK_ANIM)break;if (type == CHUNK_CCB)n++;off += (int32)sz; }
    return (int)n;
}

static int find_chunk(const ubyte* base, int32 len, int32 start, uint32 wanted, int32* offOut)
{
    int32 off;
    if (!base || !offOut || start < 0 || start > len - 8) return 0;
    off = (start + 3) & ~3;
    while (off <= len - 8) {
        uint32 type = chunk_type(base + off);
        uint32 sz = chunk_size(base + off);
        if (type == wanted && sz >= 8 && off + (int32)sz <= len) {
            *offOut = off;
            return 1;
        }
        off += 4;
    }
    return 0;
}

static int collect_chunks(const ubyte* base, int32 len, int32 start, uint32 wanted,
    int32* offsets, int maxOffsets)
{
    int32 search = start;
    int count = 0;
    int32 off;

    if (!offsets || maxOffsets <= 0) return 0;
    while (count < maxOffsets && find_chunk(base, len, search, wanted, &off)) {
        offsets[count++] = off;

        search = off + (int32)chunk_size(base + off);
        if (search <= off) break;
    }
    return count;
}

struct CelArray* parse3DO(filename)
    char* filename;
{
    ubyte* buf;
    int32 len;
    intptr_t errlen;
    uint32 first;

    if (!(buf = (ubyte*)allocloadfile(filename, MEMTYPE_CEL, &errlen))) {
        filerr(filename, errlen);
        return NULL;
    }

    len = (int32)errlen;
    if (len < 8) {
        FreeMem(buf, len);
        return NULL;
    }

    first = be32(buf);

    if (first == CHUNK_ANIM) {
        uint32 acSize, version, animType, frames;
        int32 ccbOff, plutOff;
        int32 pdatOffsets[256];
        int pdatCount, i, n;
        CCB template;
        void* plut = NULL;
        CelArray* ca;

        acSize = chunk_size(buf);
        if (acSize < 32 || acSize >(uint32)len)
        {
            FreeMem(buf, len); return NULL;
        }

        version = be32(buf + 8);
        animType = be32(buf + 12);
        frames = be32(buf + 16);

        if (version > 1 || frames == 0 || frames > 256u ||
            (animType != 0u && animType != 1u)) {
            FreeMem(buf, len);
            return NULL;
        }

        memset(&template, 0, sizeof(template));
        ccbOff = -1;
        plutOff = -1;

        if (find_chunk(buf, len, (int32)acSize, CHUNK_CCB, &ccbOff))
            read_ccb_fields(&template, buf + ccbOff + 8);
        if (find_chunk(buf, len, (int32)acSize, CHUNK_PLUT, &plutOff) &&
            chunk_size(buf + plutOff) >= 12)
            plut = (void*)(buf + plutOff + 12);

        if (ccbOff < 0) {
            FreeMem(buf, len);
            return NULL;
        }

        pdatCount = collect_chunks(buf, len, (int32)acSize, CHUNK_PDAT,
            pdatOffsets,
            (int)(sizeof(pdatOffsets) / sizeof(pdatOffsets[0])));
        if (pdatCount <= 0) {
            FreeMem(buf, len);
            return NULL;
        }

        if (animType == 1u) {

            n = (int)frames;
            ca = alloccelarray(n);
            if (!ca) { FreeMem(buf, len); return NULL; }
            ca->ca_Buffer = buf;
            ca->ca_BufSiz = len;
            ca->ca_Type = CHUNK_ANIM;
            ca->ncels = n;
            for (i = 0; i < n; ++i) {
                int pi = (i < pdatCount) ? i : (pdatCount - 1);
                CCB* c;
                if (pi < 0) {
                    freecelarray(ca);
                    return NULL;
                }
                c = (CCB*)ALLOCMEM(sizeof(CCB), MEMTYPE_CEL | MEMTYPE_FILL);
                if (!c) {
                    freecelarray(ca);
                    return NULL;
                }
                *c = template;
                prepare_ccb(c, plut, buf + pdatOffsets[pi] + 8);
                register_cel_source(c, chunk_size(buf + pdatOffsets[pi]) - 8u);
                ca->celptrs[i] = c;
            }
            return ca;
        }

        {
            int32 ccbOffsets[256];
            int ccbCount = collect_chunks(buf, len, (int32)acSize, CHUNK_CCB,
                ccbOffsets,
                (int)(sizeof(ccbOffsets) / sizeof(ccbOffsets[0])));
            n = (int)frames;
            if (ccbCount < n) n = ccbCount;
            if (pdatCount < n) n = pdatCount;
            if (n <= 0) { FreeMem(buf, len); return NULL; }

            ca = alloccelarray(n);
            if (!ca) { FreeMem(buf, len); return NULL; }
            ca->ca_Buffer = buf;
            ca->ca_BufSiz = len;
            ca->ca_Type = CHUNK_ANIM;
            ca->ncels = n;

            for (i = 0; i < n; ++i) {
                CCB* c = (CCB*)ALLOCMEM(sizeof(CCB), MEMTYPE_CEL | MEMTYPE_FILL);
                if (!c) {
                    freecelarray(ca);
                    return NULL;
                }
                read_ccb_fields(c, buf + ccbOffsets[i] + 8);
                prepare_ccb(c, plut, buf + pdatOffsets[i] + 8);
                register_cel_source(c, chunk_size(buf + pdatOffsets[i]) - 8u);
                ca->celptrs[i] = c;
            }
            return ca;
        }
    }

    if (first == CHUNK_CCB) {
        int n = count_cat(buf, len), i = 0;
        CelArray* ca = alloccelarray(n);
        CCB* cur = NULL;
        void* plut = NULL, * pdat = NULL;
        int32 off = 0;
        uint32 sz, type;
        uint32 pdatBytes = 0;

        ca->ca_Buffer = buf;
        ca->ca_BufSiz = len;
        ca->ca_Type = CHUNK_CCB;
        ca->ncels = n;
        memset(ca->celptrs, 0, (size_t)n * sizeof(CCB*));

        while (off + 8 <= len) {
            type = chunk_type(buf + off);
            sz = chunk_size(buf + off);
            if (sz < 8 || off + (int32)sz > len) break;
            if (type == CHUNK_CCB && sz >= 80) {
                if (cur && i < n) {
                    prepare_ccb(cur, plut, pdat);
                    register_cel_source(cur, pdatBytes);
                    ca->celptrs[i++] = cur;
                }
                cur = (CCB*)ALLOCMEM(sizeof(CCB), MEMTYPE_CEL | MEMTYPE_FILL);
                if (cur) read_ccb_fields(cur, buf + off + 8);
            }
            else if (type == CHUNK_PLUT && sz >= 12) {
                plut = (void*)(buf + off + 12);
            }
            else if (type == CHUNK_PDAT) {
                pdat = (void*)(buf + off + 8);
                pdatBytes = sz - 8u;
            }
            off += (int32)sz;
        }
        if (cur && i < n) {
            prepare_ccb(cur, plut, pdat);
            register_cel_source(cur, pdatBytes);
            ca->celptrs[i++] = cur;
        }
        ca->ncels = i;
        return ca;
    }

    FreeMem(buf, len);
    return NULL;
}

void freecelarray(ca)
register struct CelArray* ca;
{
    int i;
    if (!ca)return;

    if (ca->ca_Buffer && ca->celptrs[0]) {
        for (i = 0;i < ca->ncels;i++) if (ca->celptrs[i]) {
            unregister_cel_source(ca->celptrs[i]);
            FreeMem(ca->celptrs[i], sizeof(CCB));
        }
    }
    if (ca->ca_Buffer)FreeMem(ca->ca_Buffer, ca->ca_BufSiz);
    FreeMem(ca, sizeof(*ca) + sizeof(CCB*) * (ca->ncels - 1));
}

int32 cvt2power(val)
register int32 val;
{
    register int32 i;
    for (i = 32;--i >= 0;)if (val == (1 << i))return -i;
    return val;
}
