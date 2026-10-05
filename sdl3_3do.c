#include "sdl3_3do.h"
#include "castle.h"
#include "font.h"
#include "app_proto.h"
#include "operafs.h"

#include <errno.h>

extern uint32 SDL3_3DO_GetCelSourceBytes(const CCB *ccb);

#define EFMM_MAX_ITEMS 64
#define EFMM_SCREEN_COUNT 8
#define EFMM_FB_W 320
#define EFMM_FB_H 240
#define EFMM_PAGE_SIZE 2048

typedef struct SDL3DOItem
{
    int kind;
    void *ptr;
} SDL3DOItem;

static SDL_Window *gWindow;
static SDL_Renderer *gRenderer;
static SDL_Texture *gTexture;
static uint32 *gUploadBuffer;
static int gRunning;
static uint64 gNextTickNS;
static uint64 gFramePeriodNS;
static uint32 gFrameMs;
static uint32 gInputBits;
static uint32 gPrevInputBits;
static float gMouseDeltaX;
static int32 gMouseTurnF16;
static int gMouseGameMode;
static int gEscapeDown;
static int gAltEnterSuppressed;
static SDL_Gamepad *gGamepad;
static SDL_JoystickID gGamepadID;
static int gFullscreen;
static SDL3DOItem gItems[EFMM_MAX_ITEMS];
static int32 gNextItem = 1;
static Screen gScreens[EFMM_SCREEN_COUNT];
static Bitmap gBitmaps[EFMM_SCREEN_COUNT];
static int gScreenCount;
static int32 gScreenFade[EFMM_SCREEN_COUNT];
static int32 gCurrentScreen;
static int32 gPenColor;
static Font gCurrentFont;
static KernelBaseType gKernelBase;
static Task gKernelTask;
static GrafBaseType gGrafBase;
static char gConfiguredImage[1024];
static int gImageMountAttempted;

KernelBaseType *KernelBase = &gKernelBase;
GrafBaseType *GrafBase = &gGrafBase;

static void efmm_close_gamepad(void)
{
    if (gGamepad)
        SDL_CloseGamepad(gGamepad);
    gGamepad = NULL;
    gGamepadID = 0;
}

static void efmm_open_gamepad(void)
{
    int count = 0;
    SDL_JoystickID *ids;
    if (gGamepad)
        return;
    ids = SDL_GetGamepads(&count);
    if (ids && count > 0)
    {
        gGamepadID = ids[0];
        gGamepad = SDL_OpenGamepad(gGamepadID);
    }
    if (ids)
        SDL_free(ids);
}

static int32 alloc_item(int kind, void *ptr)
{
    int32 id;
    if (gNextItem >= EFMM_MAX_ITEMS)
        return -1;
    id = gNextItem++;
    gItems[id].kind = kind;
    gItems[id].ptr = ptr;
    return id;
}

static void *lookup_item_ptr(Item item, int kind)
{
    if (item <= 0 || item >= EFMM_MAX_ITEMS)
        return NULL;
    if (kind && gItems[item].kind != kind)
        return NULL;
    return gItems[item].ptr;
}

static uint32 rgb15_to_rgba(uint16 c, int32 fade16)
{
    int r = (c >> 10) & 31;
    int g = (c >> 5) & 31;
    int b = c & 31;
    if (fade16 < 0)
        fade16 = 0;
    if (fade16 > 65536)
        fade16 = 65536;
    r = (r * fade16) >> 16;
    g = (g * fade16) >> 16;
    b = (b * fade16) >> 16;
    return 0xFF000000u | ((uint32)(r * 255 / 31) << 16) |
           ((uint32)(g * 255 / 31) << 8) | (uint32)(b * 255 / 31);
}

static uint16 rgba_to_rgb15(uint32 c)
{
    int r = (int)((c >> 16) & 255) >> 3;
    int g = (int)((c >> 8) & 255) >> 3;
    int b = (int)(c & 255) >> 3;
    return MakeRGB15(r, g, b);
}

static uint16 read_be16(const uint8 *p)
{
    return (uint16)(((uint16)p[0] << 8) | p[1]);
}

static uint32 read_be32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) | (uint32)p[3];
}

static uint32 read_u32_native(const void *p)
{
    uint32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint16 read_u16_native(const void *p)
{
    uint16 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int ccb_width(const CCB *ccb)
{
    int v = ccb->ccb_Width;
    return v <= 0 ? 1 << (-v) : v;
}

static int ccb_height(const CCB *ccb)
{
    int v = ccb->ccb_Height;
    return v <= 0 ? 1 << (-v) : v;
}

static uint32 get_bits_msb(const uint8 *row, int bitpos, int nbits)
{
    uint32 out = 0;
    int i;
    for (i = 0; i < nbits; i++)
    {
        int p = bitpos + i;
        uint8 b = row[p >> 3];
        out = (out << 1) | ((b >> (7 - (p & 7))) & 1);
    }
    return out;
}

static uint16 plut_at(const CCB *ccb, uint32 idx)
{
    const uint8 *p;
    uint16 v;
    if (!ccb->ccb_PLUTPtr)
        return (uint16)idx;
    p = (const uint8 *)ccb->ccb_PLUTPtr;

    if (ccb->ccb_SDLPLUT32Pairs)
    {
        uint32 word = read_u32_native(p + ((idx >> 1) * 4));
        return (uint16)((idx & 1u) ? (word & 0xFFFFu) : (word >> 16));
    }

    if (ccb->ccb_SDLAssetBE)
        return read_be16(p + (idx * 2));
    memcpy(&v, p + (idx * 2), sizeof(v));
    return v;
}

static int ccb_bpp_from_pre0(uint32 pre0)
{
    switch (pre0 & PRE0_BPP_MASK)
    {
    case PRE0_BPP_1:
        return 1;
    case PRE0_BPP_2:
        return 2;
    case PRE0_BPP_4:
        return 4;
    case PRE0_BPP_6:
        return 6;
    case PRE0_BPP_8:
        return 8;
    case PRE0_BPP_16:
        return 16;
    }
    return 16;
}

static int ccb_bpp(const CCB *ccb)
{
    return ccb_bpp_from_pre0(ccb->ccb_PRE0);
}

/* The PDC produces a 15-bit RGB value plus a per-pixel P-mode bit.
 */
typedef struct CelPixelInfo
{
    uint16 rgb15;
    uint8 amvR;
    uint8 amvG;
    uint8 amvB;
    uint8 pMode;
    uint8 transparent;
} CelPixelInfo;

static void finalize_cel_pixel(const CCB *ccb, CelPixelInfo *out)
{
    if (!ccb || !out)
        return;
    /* Do not treat coded palette index 0 as transparent.  EFMM
     * sprite PLUTs use index 0 for opaque colours (including
     * white); transparency is determined by the decoded CEL/PDC output
     * not by an index 0 rule. */
    out->transparent = ((out->rgb15 & 0x7FFFu) == 0u &&
                        !(ccb->ccb_Flags & CCB_BGND));
}

static void decode_raw_pixel(const CCB *ccb, uint32 raw, CelPixelInfo *out)
{
    int bpp;
    uint32 idx;
    uint16 c;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!ccb)
        return;
    bpp = ccb_bpp(ccb);

    if (bpp == 16 && (ccb->ccb_PRE0 & PRE0_LINEAR))
    {
        out->rgb15 = (uint16)(raw & 0x7FFFu);
        out->pMode = (int)((raw >> 15) & 1u);
        finalize_cel_pixel(ccb, out);
        return;
    }

    if (bpp == 16)
    {
        idx = raw & 31u;
        c = plut_at(ccb, idx);
        out->rgb15 = (uint16)(c & 0x7FFFu);
        out->amvR = (uint8)((raw >> 11) & 7u);
        out->amvG = (uint8)((raw >> 8) & 7u);
        out->amvB = (uint8)((raw >> 5) & 7u);
        out->pMode = (int)((raw >> 15) & 1u);
        finalize_cel_pixel(ccb, out);
        return;
    }

    if (bpp == 8 && (ccb->ccb_PRE0 & PRE0_LINEAR))
    {
        int r = (int)((raw >> 5) & 7u);
        int g = (int)((raw >> 2) & 7u);
        int b = (int)(raw & 3u);
        if (ccb->ccb_PRE0 & PRE0_REP8)
        {
            r = (r << 2) | (r >> 1);
            g = (g << 2) | (g >> 1);
            b = (b << 3) | (b << 1) | (b >> 1);
        }
        else
        {
            r <<= 2;
            g <<= 2;
            b <<= 3;
        }
        out->rgb15 = MakeRGB15(r, g, b);
        finalize_cel_pixel(ccb, out);
        return;
    }

    if (bpp == 8)
    {
        idx = raw & 31u;
        c = plut_at(ccb, idx);
        out->rgb15 = (uint16)(c & 0x7FFFu);
        out->amvR = out->amvG = out->amvB = (uint8)((raw >> 5) & 7u);
        out->pMode = (int)((c >> 15) & 1u);
        finalize_cel_pixel(ccb, out);
        return;
    }

    if (bpp == 6)
    {
        idx = raw & 31u;
        c = plut_at(ccb, idx);
        out->rgb15 = (uint16)(c & 0x7FFFu);
        out->pMode = (int)((raw >> 5) & 1u);
        finalize_cel_pixel(ccb, out);
        return;
    }

    idx = raw;
    if (bpp < 5)
    {
        int missing = 5 - bpp;
        uint32 pluta = ccb->ccb_Flags & CCB_PLUTA_MASK;
        uint32 hi = (pluta >> (bpp - 1)) & ((1u << missing) - 1u);
        idx |= hi << bpp;
    }
    c = plut_at(ccb, idx);
    out->rgb15 = (uint16)(c & 0x7FFFu);
    out->pMode = (int)((c >> 15) & 1u);
    finalize_cel_pixel(ccb, out);
}

static uint32 font_read_bits_compat(const uint8 *p, int maxBits, int *bitpos, int nbits);

#define EFMM_CEL_CACHE_MAX 32

typedef struct CelCacheEntry
{
    int valid;
    uint64 age;
    const uint8 *sourceKey;
    const void *plutKey;
    uint32 pre0;
    uint32 pre1;
    uint32 flagsKey;
    uint32 sourceBytesKey;
    int assetBE;
    int plut32Pairs;
    int width;
    int height;
    CelPixelInfo *pixels;
} CelCacheEntry;

static CelCacheEntry gCelCache[EFMM_CEL_CACHE_MAX];
static uint64 gCelCacheAge;
static uint32 gRGB15Table[32768];
static int gRGB15TableReady;

static void rgb15_table_init(void)
{
    int c;
    if (gRGB15TableReady)
        return;
    for (c = 0; c < 32768; ++c)
        gRGB15Table[c] = 0xFF000000u |
                         ((uint32)(((c >> 10) & 31) * 255 / 31) << 16) |
                         ((uint32)(((c >> 5) & 31) * 255 / 31) << 8) |
                         (uint32)((c & 31) * 255 / 31);
    gRGB15TableReady = 1;
}

static void cel_cache_free_entry(CelCacheEntry *e)
{
    if (!e)
        return;
    if (e->pixels)
        free(e->pixels);
    memset(e, 0, sizeof(*e));
}

static void cel_cache_clear(void)
{
    int i;
    for (i = 0; i < EFMM_CEL_CACHE_MAX; ++i)
        cel_cache_free_entry(&gCelCache[i]);
    gCelCacheAge = 0;
}

static int cel_cache_same(const CelCacheEntry *e, const CCB *ccb,
                          const uint8 *source, const CCB *decode)
{
    uint32 keyFlags;
    uint32 sourceBytes;
    if (!e || !e->valid || !ccb || !decode)
        return 0;
    keyFlags = ccb->ccb_Flags & (CCB_PACKED | CCB_CCBPRE | CCB_PLUTA_MASK);
    sourceBytes = SDL3_3DO_GetCelSourceBytes(ccb);
    return e->sourceKey == source &&
           e->sourceBytesKey == sourceBytes &&
           e->plutKey == decode->ccb_PLUTPtr &&
           e->pre0 == decode->ccb_PRE0 &&
           e->pre1 == decode->ccb_PRE1 &&
           e->flagsKey == keyFlags &&
           e->assetBE == decode->ccb_SDLAssetBE &&
           e->plut32Pairs == decode->ccb_SDLPLUT32Pairs;
}

static CelCacheEntry *cel_cache_find(const CCB *ccb, const uint8 *source,
                                     const CCB *decode)
{
    int i;
    for (i = 0; i < EFMM_CEL_CACHE_MAX; ++i)
    {
        if (cel_cache_same(&gCelCache[i], ccb, source, decode))
        {
            gCelCache[i].age = ++gCelCacheAge;
            return &gCelCache[i];
        }
    }
    return NULL;
}

static CelCacheEntry *cel_cache_slot(void)
{
    int i, best = -1;
    uint64 oldest = ~0ULL;
    for (i = 0; i < EFMM_CEL_CACHE_MAX; ++i)
    {
        if (!gCelCache[i].valid)
            return &gCelCache[i];
        if (gCelCache[i].age < oldest)
        {
            oldest = gCelCache[i].age;
            best = i;
        }
    }
    cel_cache_free_entry(&gCelCache[best]);
    return &gCelCache[best];
}

static CelPixelInfo *cel_cache_pixel(CelCacheEntry *e, int sx, int sy)
{
    if (!e || !e->pixels || sx < 0 || sy < 0 ||
        sx >= e->width || sy >= e->height)
        return NULL;
    return &e->pixels[(size_t)sy * (size_t)e->width + (size_t)sx];
}

static int cel_cache_build(CelCacheEntry *e, const CCB *ccb,
                           const uint8 *source, const CCB *decode,
                           int srcW, int srcH, int rowBytes)
{
    int sy, sx;
    if (!e || !ccb || !decode || !source || srcW <= 0 || srcH <= 0 ||
        srcW > 2048 || srcH > 2048)
        return 0;

    e->pixels = (CelPixelInfo *)calloc((size_t)srcW * (size_t)srcH,
                                       sizeof(CelPixelInfo));
    if (!e->pixels)
        return 0;
    {
        size_t count = (size_t)srcW * (size_t)srcH;
        size_t i;
        for (i = 0; i < count; ++i)
            e->pixels[i].transparent = 1;
    }

    e->width = srcW;
    e->height = srcH;

    if (decode->ccb_Flags & CCB_PACKED)
    {
        const uint8 *line = source;
        uint32 registeredBytes = SDL3_3DO_GetCelSourceBytes(ccb);
        size_t remaining = registeredBytes;
        int bounded = registeredBytes != 0;
        int bpp = ccb_bpp(decode);
        if (bounded)
        {
            const uint8 *sourceStart = (const uint8 *)ccb->ccb_SourcePtr;
            size_t consumed;
            if (!sourceStart || source < sourceStart)
                return 0;
            consumed = (size_t)(source - sourceStart);
            if (consumed > remaining)
                return 0;
            remaining -= consumed;
        }
        for (sy = 0; sy < srcH; ++sy)
        {
            uint32 offsetValue;
            uint32 strideWords;
            int headerBytes;
            int lineBytes;
            int bitpos = 0;
            int maxBits;
            int outPos = 0;

            if (bpp <= 6)
            {
                if (bounded && remaining < 1)
                    break;
                offsetValue = (uint32)line[0];
                headerBytes = 1;
            }
            else
            {
                if (bounded && remaining < 2)
                    break;
                offsetValue = (uint32)(((uint32)line[0] << 8) | line[1]);
                headerBytes = 2;
            }
            strideWords = offsetValue + 2u;
            if (strideWords < 2u || strideWords > 1024u)
                break;
            if (bounded && (size_t)strideWords * 4u > remaining)
                break;
            lineBytes = (int)(strideWords * 4u) - headerBytes;
            if (lineBytes <= 0)
                break;

            line += headerBytes;
            maxBits = lineBytes * 8;
            while (bitpos + 8 <= maxBits && outPos < srcW)
            {
                uint32 ctl = font_read_bits_compat(line, maxBits, &bitpos, 8);
                int type = (int)(ctl >> 6);
                int count = (int)(ctl & 0x3f) + 1;
                int n;

                if (type == 0)
                    break;
                if (type == 1)
                {
                    for (n = 0; n < count && outPos < srcW; ++n, ++outPos)
                    {
                        uint32 raw = font_read_bits_compat(line, maxBits, &bitpos, bpp);
                        CelPixelInfo *dst = cel_cache_pixel(e, outPos, sy);
                        if (dst)
                            decode_raw_pixel(decode, raw, dst);
                    }
                }
                else if (type == 2)
                {
                    for (n = 0; n < count && outPos < srcW; ++n, ++outPos)
                    {
                        CelPixelInfo *dst = cel_cache_pixel(e, outPos, sy);
                        if (dst)
                        {
                            dst->pMode = 0;
                            dst->rgb15 = 0;
                            dst->transparent = 1;
                        }
                    }
                }
                else
                {
                    uint32 raw = font_read_bits_compat(line, maxBits, &bitpos, bpp);
                    CelPixelInfo repeat;
                    decode_raw_pixel(decode, raw, &repeat);
                    for (n = 0; n < count && outPos < srcW; ++n, ++outPos)
                    {
                        CelPixelInfo *dst = cel_cache_pixel(e, outPos, sy);
                        if (dst)
                            *dst = repeat;
                    }
                }
            }
            line += (size_t)(strideWords * 4u - headerBytes);
            if (bounded)
                remaining -= (size_t)strideWords * 4u;
        }
        if (bounded && sy < srcH)
        {
            free(e->pixels);
            e->pixels = NULL;
            return 0;
        }
    }
    else
    {
        int bpp = ccb_bpp(decode);
        for (sy = 0; sy < srcH; ++sy)
        {
            const uint8 *row = source + (size_t)sy * (size_t)rowBytes;
            for (sx = 0; sx < srcW; ++sx)
            {
                uint32 raw;
                CelPixelInfo *dst = cel_cache_pixel(e, sx, sy);
                if (!dst)
                    continue;
                if (bpp == 6)
                {
                    raw = (uint32)(row[sx] & 0x3fu);
                }
                else if (bpp == 8)
                {
                    raw = row[sx];
                }
                else if (bpp == 16)
                {
                    raw = decode->ccb_SDLAssetBE ? (uint32)read_be16(row + sx * 2) : (uint32)read_u16_native(row + sx * 2);
                }
                else
                {
                    raw = get_bits_msb(row, sx * bpp, bpp);
                }
                decode_raw_pixel(decode, raw, dst);
            }
        }
    }

    e->sourceKey = source;
    e->sourceBytesKey = SDL3_3DO_GetCelSourceBytes(ccb);
    e->plutKey = decode->ccb_PLUTPtr;
    e->pre0 = decode->ccb_PRE0;
    e->pre1 = decode->ccb_PRE1;
    e->flagsKey = ccb->ccb_Flags & (CCB_PACKED | CCB_CCBPRE | CCB_PLUTA_MASK);
    e->assetBE = decode->ccb_SDLAssetBE;
    e->plut32Pairs = decode->ccb_SDLPLUT32Pairs;
    e->age = ++gCelCacheAge;
    e->valid = 1;
    return 1;
}

static CelCacheEntry *cel_decode_cached(const CCB *ccb, const uint8 *source,
                                        const CCB *decode, int srcW, int srcH,
                                        int rowBytes)
{
    CelCacheEntry *e = cel_cache_find(ccb, source, decode);
    if (e)
        return e;
    e = cel_cache_slot();
    if (!cel_cache_build(e, ccb, source, decode, srcW, srcH, rowBytes))
    {
        cel_cache_free_entry(e);
        return NULL;
    }
    return e;
}

static uint32 font_read_bits_compat(const uint8 *p, int maxBits, int *bitpos, int nbits);

static uint16 packed_pixel(const CCB *ccb, const uint8 *source, int sx, int sy,
                           int *transparentOut, CelPixelInfo *infoOut)
{
    const uint8 *line;
    int bpp, row, headerBytes;
    int lineBytes, bitpos, maxBits, outPos;
    uint32 offsetValue, strideWords, ctl, raw;
    CelPixelInfo info;

    if (transparentOut)
        *transparentOut = 0;
    if (infoOut)
        memset(infoOut, 0, sizeof(*infoOut));
    if (!ccb || !source || sx < 0 || sy < 0)
    {
        if (transparentOut)
            *transparentOut = 1;
        return 0;
    }

    bpp = ccb_bpp(ccb);
    line = source;

    for (row = 0; row < sy; ++row)
    {
        if (bpp <= 6)
        {
            offsetValue = (uint32)line[0];
            headerBytes = 1;
        }
        else
        {
            offsetValue = ((uint32)line[0] << 8) | (uint32)line[1];
            headerBytes = 2;
        }
        strideWords = offsetValue + 2u;
        if (strideWords > 1024u)
        {
            if (transparentOut)
                *transparentOut = 1;
            return 0;
        }
        line += strideWords * 4u;
    }

    if (bpp <= 6)
    {
        offsetValue = (uint32)line[0];
        headerBytes = 1;
    }
    else
    {
        offsetValue = ((uint32)line[0] << 8) | (uint32)line[1];
        headerBytes = 2;
    }
    strideWords = offsetValue + 2u;
    if (strideWords == 0 || strideWords > 1024u)
    {
        if (transparentOut)
            *transparentOut = 1;
        return 0;
    }

    lineBytes = (int)(strideWords * 4u) - headerBytes;
    if (lineBytes <= 0)
    {
        if (transparentOut)
            *transparentOut = 1;
        return 0;
    }

    line += headerBytes;
    bitpos = 0;
    maxBits = lineBytes * 8;
    outPos = 0;

    while (bitpos + 8 <= maxBits)
    {
        ctl = font_read_bits_compat(line, maxBits, &bitpos, 8);
        {
            int type = (int)(ctl >> 6);
            int count = (int)(ctl & 0x3f) + 1;
            int n;

            if (type == 0)
                break;

            if (type == 1)
            {
                for (n = 0; n < count; ++n)
                {
                    raw = font_read_bits_compat(line, maxBits, &bitpos, bpp);
                    if (outPos == sx)
                    {
                        decode_raw_pixel(ccb, raw, &info);
                        if (infoOut)
                            *infoOut = info;
                        return info.rgb15;
                    }
                    ++outPos;
                }
            }
            else if (type == 2)
            {
                if (sx >= outPos && sx < outPos + count)
                {
                    if (transparentOut)
                        *transparentOut = 1;
                    return 0;
                }
                outPos += count;
            }
            else
            {
                raw = font_read_bits_compat(line, maxBits, &bitpos, bpp);
                if (sx >= outPos && sx < outPos + count)
                {
                    decode_raw_pixel(ccb, raw, &info);
                    if (infoOut)
                        *infoOut = info;
                    return info.rgb15;
                }
                outPos += count;
            }
            if (outPos > 4096)
                break;
        }
    }

    if (transparentOut)
        *transparentOut = 1;
    return 0;
}

static int unpacked_row_bytes(const CCB *ccb, int bpp)
{
    uint32 words;
    if (bpp >= 8)
        words = (ccb->ccb_PRE1 & PRE1_WOFFSET10_MASK) >> PRE1_WOFFSET10_SHIFT;
    else
        words = (ccb->ccb_PRE1 & PRE1_WOFFSET8_MASK) >> PRE1_WOFFSET8_SHIFT;
    words += PRE1_WOFFSET_PREFETCH;
    if (words < 2u || words > 1024u)
        return 0;
    return (int)(words * 4u);
}

static int cel_source_width(const CCB *ccb, int bpp)
{
    int n;
    if (!ccb)
        return 0;
    if (ccb->ccb_Flags & CCB_PACKED)
        return ccb_width(ccb);
    n = (int)((ccb->ccb_PRE1 & PRE1_TLHPCNT_MASK) >> PRE1_TLHPCNT_SHIFT) + 1;
    return n > 0 ? n : ccb_width(ccb);
}

static int cel_source_height(const CCB *ccb)
{
    int n;
    if (!ccb)
        return 0;
    n = (int)((ccb->ccb_PRE0 & PRE0_VCNT_MASK) >> PRE0_VCNT_SHIFT) + 1;
    return n > 0 ? n : ccb_height(ccb);
}

static uint16 cel_pixel(const CCB *ccb, const uint8 *source, int sx, int sy,
                        int rowBytes, int *transparentOut, CelPixelInfo *infoOut)
{
    int bpp;
    const uint8 *src;
    uint32 raw;
    CelPixelInfo info;

    if (transparentOut)
        *transparentOut = 0;
    if (infoOut)
        memset(infoOut, 0, sizeof(*infoOut));
    if (!ccb || !source || sx < 0 || sy < 0)
    {
        if (transparentOut)
            *transparentOut = 1;
        return 0;
    }

    bpp = ccb_bpp(ccb);
    if (ccb->ccb_Flags & CCB_PACKED)
        return packed_pixel(ccb, source, sx, sy, transparentOut, infoOut);

    if (rowBytes <= 0)
    {
        if (transparentOut)
            *transparentOut = 1;
        return 0;
    }

    src = source + (size_t)sy * (size_t)rowBytes;

    if (bpp == 6)
    {
        raw = (uint32)(src[sx] & 0x3Fu);
        decode_raw_pixel(ccb, raw, &info);
        if (infoOut)
            *infoOut = info;
        return info.rgb15;
    }

    if (bpp == 8)
    {
        raw = (uint32)src[sx];
        decode_raw_pixel(ccb, raw, &info);
        if (infoOut)
            *infoOut = info;
        return info.rgb15;
    }

    if (bpp == 16)
    {
        raw = ccb->ccb_SDLAssetBE ? read_be16(src + sx * 2) : read_u16_native(src + sx * 2);
        decode_raw_pixel(ccb, raw, &info);
        if (infoOut)
            *infoOut = info;
        return info.rgb15;
    }

    raw = get_bits_msb(src, sx * bpp, bpp);
    decode_raw_pixel(ccb, raw, &info);
    if (infoOut)
        *infoOut = info;
    return info.rgb15;
}

static uint32 font_read_bits_compat(const uint8 *p, int maxBits, int *bitpos, int nbits);

/* Local MSB-first bit reader used by packed CEL data. */
static uint32 font_read_bits_compat(const uint8 *p, int maxBits, int *bitpos, int nbits)
{
    uint32 out = 0;
    int i, b;
    if (!p || !bitpos || nbits <= 0 || nbits > 24)
        return 0;
    for (i = 0; i < nbits; ++i)
    {
        b = *bitpos;
        if (b < 0 || b >= maxBits)
            return out;
        out = (out << 1) | ((p[b >> 3] >> (7 - (b & 7))) & 1u);
        *bitpos = b + 1;
    }
    return out;
}

static uint32 read_native_or_be32(const CCB *ccb, const uint8 *p)
{
    return ccb->ccb_SDLAssetBE ? read_be32(p) : read_u32_native(p);
}

static int pixc_df(uint32 half)
{
    switch ((half & PPMPC_SF_MASK) >> PPMPC_SF_SHIFT)
    {
    case 1:
        return 2;
    case 2:
        return 4;
    case 3:
        return 8;
    default:
        return 16;
    }
}

static int pixc_pmv(uint32 half, int amv, uint16 decoded)
{
    switch ((half & PPMPC_MS_MASK) >> PPMPC_MS_SHIFT)
    {
    case 1:
        return amv;
    case 2:
        return decoded & 7u;
    case 3:
        return decoded & 7u;
    default:
        return (int)(((half & PPMPC_MF_MASK) >> PPMPC_MF_SHIFT) + 1);
    }
}

static int pixc_pdv(uint32 half, uint16 decoded)
{
    int ms = (int)((half & PPMPC_MS_MASK) >> PPMPC_MS_SHIFT);
    if (ms == 2)
    {
        switch ((decoded >> 3) & 3u)
        {
        case 1:
            return 2;
        case 2:
            return 4;
        case 3:
            return 8;
        default:
            return 16;
        }
    }
    return pixc_df(half);
}

static int pixc_source(uint32 half, int which, int decoded, int dst, int constant)
{
    (void)which;
    switch ((half & PPMPC_2S_MASK) >> PPMPC_2S_SHIFT)
    {
    case 1:
        return constant;
    case 2:
        return dst;
    case 3:
        return decoded;
    default:
        return 0;
    }
}

static int pixc_component(uint32 half, int decoded, int dst, int amv, int useav)
{
    int primary;
    int secondary;
    int pmv = pixc_pmv(half, amv, (uint16)decoded);
    int pdv = pixc_pdv(half, (uint16)decoded);
    int sdv = 1;
    int av = (int)((half & PPMPC_AV_MASK) >> PPMPC_AV_SHIFT);
    int ms = (int)((half & PPMPC_MS_MASK) >> PPMPC_MS_SHIFT);

    (void)ms;
    primary = decoded;
    primary = (primary * pmv + (pdv >> 1)) / pdv;

    if ((half & PPMPC_2S_MASK) == PPMPC_2S_CCB)
        secondary = av;
    else
        secondary = pixc_source(half, 0, decoded, dst, av);

    if (half & PPMPC_2D_MASK)
        secondary /= 2;

    if (useav)
    {
        int avctl = av;
        int sdvSel = (avctl >> 3) & 3;
        if (sdvSel == 1)
            sdv = 2;
        else if (sdvSel == 2)
            sdv = 4;
        else if (sdvSel == 3)
            sdv = ((decoded & 3) + 1);
        if (sdv < 1)
            sdv = 1;
    }

    secondary /= sdv;
    if (useav)
    {
        int avctl = av;
        if (avctl & 0x2)
        {
            /* The low five bits carry the value. */
            if (secondary & 0x10)
                secondary |= ~0x1F;
        }
    }
    if (useav && (av & 0x1))
        primary -= secondary;
    else
        primary += secondary;

    if (primary < 0)
        primary = 0;
    if (primary > 31)
        primary = 31;
    return primary;
}

static uint16 pixc_apply(const CCB *ccb, uint16 src, uint16 dst, int amvR, int amvG, int amvB, int pixelPMode)
{
    uint32 pixc = ccb->ccb_PIXC;
    int useav = (ccb->ccb_Flags & CCB_USEAV) != 0;

    /* The common opaque P-mode. */
    if (pixc == 0x1F001F00u && !(ccb->ccb_Flags & CCB_PXOR))
        return src;
    int pMode = (int)((ccb->ccb_Flags & CCB_POVER_MASK) >> CCB_POVER_SHIFT);
    uint32 half;
    int sr, sg, sb, dr, dg, db, r, g, b;

    /* POVER=10/11 pins P-mode 0/1 */
    if (pMode == 3)
        half = pixc >> 16;
    else if (pMode == 2)
        half = pixc;
    else
        half = pixelPMode ? (pixc >> 16) : pixc;

    sr = (src >> 10) & 31;
    sg = (src >> 5) & 31;
    sb = src & 31;
    dr = (dst >> 10) & 31;
    dg = (dst >> 5) & 31;
    db = dst & 31;
    r = pixc_component(half, sr, dr, amvR, useav);
    g = pixc_component(half, sg, dg, amvG, useav);
    b = pixc_component(half, sb, db, amvB, useav);

    if (ccb->ccb_Flags & CCB_PXOR)
        return MakeRGB15(r ^ dr, g ^ dg, b ^ db);
    return MakeRGB15(r, g, b);
}

static void blend_pixel(Bitmap *bm, int x, int y, uint16 src, int32 fade16,
                        int average, int opaqueZero, const CCB *ccb, int amvR, int amvG, int amvB, int pixelPMode)
{
    uint32 *dst;
    uint32 out;
    uint16 dc;
    if (!bm || x < 0 || y < 0 || x >= bm->bm_Width || y >= bm->bm_Height)
        return;
    if (!(src & 0x7FFF) && !average && !opaqueZero)
        return;
    dst = bm->bm_Buffer + y * bm->bm_Width + x;
    dc = rgba_to_rgb15(*dst);
    if (average)
    {
        int sr = (src >> 10) & 31, sg = (src >> 5) & 31, sb = src & 31;
        int dr = (dc >> 10) & 31, dg = (dc >> 5) & 31, db = dc & 31;
        src = MakeRGB15((sr + dr) >> 1, (sg + dg) >> 1, (sb + db) >> 1);
    }
    else if (ccb)
    {
        src = pixc_apply(ccb, src, dc, amvR, amvG, amvB, pixelPMode);
    }
    out = rgb15_to_rgba(src, fade16);
    *dst = out;
}

static int32 default_fade(const CCB *ccb)
{
    return ccb->ccb_SDLFade16 ? ccb->ccb_SDLFade16 : 65536;
}

static void draw_ccb(Bitmap *bm, CCB *ccb)
{
    int w, h, bpp, rowBytes;
    int srcW, srcH;
    const uint8 *source;
    CCB decode;
    CelCacheEntry *cache;
    float x0, y0, ax, ay, bx, by, cx, cy;
    float det, invDet;
    int minx, maxx, miny, maxy, x, y;
    int identityPixc;
    int32 fade16;
    int affine;

    if (!ccb || !ccb->ccb_SourcePtr || !bm)
        return;

    decode = *ccb;
    source = (const uint8 *)ccb->ccb_SourcePtr;

    /* CCBPRE clear PRE0/PRE1 live at the start of the CEL source stream. */
    if (!(ccb->ccb_Flags & CCB_CCBPRE))
    {
        decode.ccb_PRE0 = read_native_or_be32(ccb, source);
        source += 4;
        if (!(ccb->ccb_Flags & CCB_PACKED))
        {
            decode.ccb_PRE1 = read_native_or_be32(ccb, source);
            source += 4;
        }
    }

    bpp = ccb_bpp(&decode);
    w = ccb_width(ccb);
    h = ccb_height(ccb);
    if (w <= 0 || h <= 0)
        return;

    rowBytes = 0;
    if (!(decode.ccb_Flags & CCB_PACKED))
        rowBytes = unpacked_row_bytes(&decode, bpp);

    srcW = cel_source_width(&decode, bpp);
    srcH = cel_source_height(&decode);
    if (srcW <= 0 || srcH <= 0)
        return;

    cache = cel_decode_cached(ccb, source, &decode, srcW, srcH, rowBytes);
    if (!cache)
        return;

    x0 = (float)ccb->ccb_XPos / 65536.0f;
    y0 = (float)ccb->ccb_YPos / 65536.0f;
    ax = (float)ccb->ccb_HDX / 1048576.0f * (float)w;
    ay = (float)ccb->ccb_HDY / 1048576.0f * (float)w;
    bx = (float)ccb->ccb_VDX / 65536.0f * (float)h;
    by = (float)ccb->ccb_VDY / 65536.0f * (float)h;
    cx = (float)ccb->ccb_HDDX / 1048576.0f * (float)w * (float)h;
    cy = (float)ccb->ccb_HDDY / 1048576.0f * (float)w * (float)h;

    {
        float x1 = x0 + ax, y1 = y0 + ay, x3 = x0 + bx, y3 = y0 + by;
        float x2 = x1 + bx + cx, y2 = y1 + by + cy;
        float xmin = fminf(fminf(x0, x1), fminf(x2, x3));
        float xmax = fmaxf(fmaxf(x0, x1), fmaxf(x2, x3));
        float ymin = fminf(fminf(y0, y1), fminf(y2, y3));
        float ymax = fmaxf(fmaxf(y0, y1), fmaxf(y2, y3));
        minx = (int)floorf(xmin);
        maxx = (int)ceilf(xmax);
        miny = (int)floorf(ymin);
        maxy = (int)ceilf(ymax);
    }
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx > bm->bm_Width)
        maxx = bm->bm_Width;
    if (maxy > bm->bm_Height)
        maxy = bm->bm_Height;
    if (minx >= maxx || miny >= maxy)
        return;

    det = ax * by - ay * bx;
    if (fabsf(det) < 1e-12f)
        return;
    invDet = 1.0f / det;

    fade16 = default_fade(ccb);
    identityPixc = (ccb->ccb_PIXC == 0x1F001F00u && !(ccb->ccb_Flags & CCB_PXOR));
    affine = (fabsf(cx) < 0.000001f && fabsf(cy) < 0.000001f);
    rgb15_table_init();

    if (affine)
    {
        const float du_dx = by * invDet;
        const float dv_dx = -ay * invDet;
        for (y = miny; y < maxy; ++y)
        {
            uint32 *dstrow = bm->bm_Buffer + y * bm->bm_Width;
            float px0 = (float)minx + 0.5f;
            float py = (float)y + 0.5f;
            float tx = px0 - x0;
            float ty = py - y0;
            float u = (tx * by - ty * bx) * invDet;
            float v = (ax * ty - ay * tx) * invDet;
            for (x = minx; x < maxx; ++x)
            {
                if (u > -0.0005f && u < 1.0005f && v > -0.0005f && v < 1.0005f)
                {
                    float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
                    float cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    int sx = (int)floorf(cu * (float)srcW);
                    int sy = (int)floorf(cv * (float)srcH);
                    CelPixelInfo *info;
                    uint16 pix;
                    if (sx >= srcW)
                        sx = srcW - 1;
                    if (sy >= srcH)
                        sy = srcH - 1;
                    info = cel_cache_pixel(cache, sx, sy);
                    if (info && !info->transparent)
                    {
                        pix = info->rgb15;
                        if (identityPixc)
                        {
                            uint32 out = gRGB15Table[pix & 0x7FFFu];
                            if (fade16 != 65536)
                            {
                                int rr = (int)((out >> 16) & 255), gg = (int)((out >> 8) & 255), bb = (int)(out & 255);
                                rr = (rr * fade16) >> 16;
                                gg = (gg * fade16) >> 16;
                                bb = (bb * fade16) >> 16;
                                out = 0xFF000000u | ((uint32)rr << 16) | ((uint32)gg << 8) | (uint32)bb;
                            }
                            dstrow[x] = out;
                        }
                        else
                        {
                            uint16 dc = rgba_to_rgb15(dstrow[x]);
                            pix = pixc_apply(ccb, pix, dc, info->amvR, info->amvG, info->amvB, info->pMode);
                            dstrow[x] = rgb15_to_rgba(pix, fade16);
                        }
                    }
                }
                u += du_dx;
                v += dv_dx;
            }
        }
        return;
    }

#define EFMM_BILIN_BLOCK 1
    for (y = miny; y < maxy; ++y)
    {
        uint32 *dstrow = bm->bm_Buffer + y * bm->bm_Width;
        float py = (float)y + 0.5f;
        x = minx;
        while (x < maxx)
        {
            int blockEnd = x + EFMM_BILIN_BLOCK;
            float px = (float)x + 0.5f;
            float tx = px - x0, ty = py - y0;
            float u = (tx * by - ty * bx) * invDet;
            float v = (ax * ty - ay * tx) * invDet;
            float fx, fy, j11, j12, j21, j22, jd, du, dv;
            int bxpix, iter;

            if (blockEnd > maxx)
                blockEnd = maxx;

            for (iter = 0; iter < 2; ++iter)
            {
                fx = x0 + u * ax + v * bx + u * v * cx - px;
                fy = y0 + u * ay + v * by + u * v * cy - py;
                j11 = ax + v * cx;
                j12 = bx + u * cx;
                j21 = ay + v * cy;
                j22 = by + u * cy;
                jd = j11 * j22 - j12 * j21;
                if (fabsf(jd) <= 1e-12f)
                    break;
                du = (fx * j22 - fy * j12) / jd;
                dv = (-fx * j21 + fy * j11) / jd;
                u -= du;
                v -= dv;
            }

            for (bxpix = x; bxpix < blockEnd; ++bxpix)
            {
                float su = u, sv = v;
                if (su > -0.0005f && su < 1.0005f && sv > -0.0005f && sv < 1.0005f)
                {
                    int sx, sy;
                    CelPixelInfo *info;
                    uint16 pix;
                    if (su < 0.0f)
                        su = 0.0f;
                    else if (su > 1.0f)
                        su = 1.0f;
                    if (sv < 0.0f)
                        sv = 0.0f;
                    else if (sv > 1.0f)
                        sv = 1.0f;
                    sx = (int)floorf(su * (float)srcW);
                    sy = (int)floorf(sv * (float)srcH);
                    if (sx >= srcW)
                        sx = srcW - 1;
                    if (sy >= srcH)
                        sy = srcH - 1;
                    info = cel_cache_pixel(cache, sx, sy);
                    if (info && !info->transparent)
                    {
                        pix = info->rgb15;
                        if (identityPixc)
                        {
                            uint32 out = gRGB15Table[pix & 0x7FFFu];
                            if (fade16 != 65536)
                            {
                                int rr = (int)((out >> 16) & 255), gg = (int)((out >> 8) & 255), bb = (int)(out & 255);
                                rr = (rr * fade16) >> 16;
                                gg = (gg * fade16) >> 16;
                                bb = (bb * fade16) >> 16;
                                out = 0xFF000000u | ((uint32)rr << 16) | ((uint32)gg << 8) | (uint32)bb;
                            }
                            dstrow[bxpix] = out;
                        }
                        else
                        {
                            uint16 dc = rgba_to_rgb15(dstrow[bxpix]);
                            pix = pixc_apply(ccb, pix, dc, info->amvR, info->amvG, info->amvB, info->pMode);
                            dstrow[bxpix] = rgb15_to_rgba(pix, fade16);
                        }
                    }
                }
            }
            x = blockEnd;
        }
    }
#undef EFMM_BILIN_BLOCK
}
static void make_base_path(const char *name, char *out, size_t cap)
{
    const char *base;
    size_t n;
    if (!name || !*name || !out || cap == 0)
    {
        if (out && cap)
            out[0] = 0;
        return;
    }
    if ((name[0] >= 'A' && name[0] <= 'Z' && name[1] == ':') ||
        (name[0] >= 'a' && name[0] <= 'z' && name[1] == ':') ||
        (name[0] == '/' && name[1] == '/') || name[0] == '\\')
    {
        snprintf(out, cap, "%s", name);
        return;
    }

    base = SDL_GetBasePath();
    if (!base || !*base)
        base = "./";
    n = strlen(base);
    if (n && (base[n - 1] == '/' || base[n - 1] == '\\'))
        snprintf(out, cap, "%s%s", base, name);
    else
        snprintf(out, cap, "%s/%s", base, name);
}

static void efmm_config_path(char *path, size_t cap)
{
    const char *base = SDL_GetBasePath();
    size_t n;
    if (!path || !cap)
        return;
    if (!base || !*base)
        base = "./";
    n = strlen(base);
    if (n && (base[n - 1] == '/' || base[n - 1] == '\\'))
        snprintf(path, cap, "%sefmm.cfg", base);
    else
        snprintf(path, cap, "%s/efmm.cfg", base);
}

static void efmm_load_config(void)
{
    char path[1024];
    char line[128];
    FILE *f;
    gFullscreen = 0;
    efmm_config_path(path, sizeof(path));
    f = fopen(path, "rb");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        int value;
        if (sscanf(line, "fullscreen=%d", &value) == 1)
        {
            gFullscreen = value ? 1 : 0;
            break;
        }
    }
    fclose(f);
}

static void efmm_save_config(void)
{
    char path[1024];
    FILE *f;
    efmm_config_path(path, sizeof(path));
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "fullscreen=%d\n", gFullscreen ? 1 : 0);
    fclose(f);
}

static void efmm_set_fullscreen(int enabled)
{
    int value = enabled ? 1 : 0;
    if (!gWindow)
        return;
    if (!SDL_SetWindowFullscreen(gWindow, value ? true : false))
        return;
    gFullscreen = value;
    efmm_save_config();
    SDL_SyncWindow(gWindow);
}

void SDL3_3DO_SetImagePath(const char *path)
{
    if (!path)
    {
        gConfiguredImage[0] = 0;
        return;
    }
    snprintf(gConfiguredImage, sizeof(gConfiguredImage), "%s", path);
}

int SDL3_3DO_MountImage(const char *path)
{
    char resolved[1024];
    if (!path || !*path)
        return 0;
    make_base_path(path, resolved, sizeof(resolved));
    if (OperaFS_Mount(resolved))
    {
        fprintf(stderr, "OperaFS: mounted %s (%s)\n", resolved,
                OperaFS_GetCurrentDirectory());
        return 1;
    }
    fprintf(stderr, "OperaFS: unable to mount %s\n", resolved);
    return 0;
}

int SDL3_3DO_MountImageAuto(void)
{
    static const char *candidates[] = {
        "EEFM.img", "EFMM.img", "EEFM.iso", "EFMM.iso", "EEFM.bin", "EFMM.bin"};
    char base[1024];
    size_t i;

    if (gImageMountAttempted)
        return OperaFS_IsMounted();
    gImageMountAttempted = 1;

    if (gConfiguredImage[0])
        return SDL3_3DO_MountImage(gConfiguredImage);

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
    {
        make_base_path(candidates[i], base, sizeof(base));
        if (SDL3_3DO_MountImage(base))
            return 1;
    }
    return 0;
}

void SDL3_3DO_UnmountImage(void)
{
    OperaFS_Unmount();
    gImageMountAttempted = 0;
}

int SDL3_3DO_Init(int w, int h, const char *title)
{
    (void)w;
    (void)h;
    if (gWindow && gRenderer && gTexture)
        return 0;
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1.0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    gWindow = SDL_CreateWindow(title ? title : "Escape from Monster Manor", 1280, 960, SDL_WINDOW_RESIZABLE);
    if (!gWindow)
        return -1;

    gRenderer = SDL_CreateRenderer(gWindow, NULL);
    if (!gRenderer)
        return -1;

    if (!SDL_SetRenderVSync(gRenderer, 1))
    {
        fprintf(stderr, "SDL_SetRenderVSync(1) failed: %s\n", SDL_GetError());
        return -1;
    }

    gTexture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, EFMM_FB_W, EFMM_FB_H);
    if (!gTexture)
        return -1;
    if (!SDL_SetTextureScaleMode(gTexture, SDL_SCALEMODE_LINEAR))
    {
        fprintf(stderr, "SDL_SetTextureScaleMode(LINEAR) failed: %s\n", SDL_GetError());
        return -1;
    }

    if (!SDL_SetRenderLogicalPresentation(gRenderer, EFMM_FB_W, EFMM_FB_H,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX))
    {
        fprintf(stderr, "SDL_SetRenderLogicalPresentation(LETTERBOX) failed: %s\n", SDL_GetError());
        return -1;
    }

    efmm_load_config();

    if (gFullscreen && !SDL_SetWindowFullscreen(gWindow, true))
        gFullscreen = 0;

    if (gFullscreen)
        SDL_SyncWindow(gWindow);

    gUploadBuffer = (uint32 *)malloc(EFMM_FB_W * EFMM_FB_H * sizeof(uint32));
    if (!gUploadBuffer)
        return -1;

    memset(gUploadBuffer, 0, EFMM_FB_W * EFMM_FB_H * sizeof(uint32));
    gFrameMs = 1000 / 60;
    gFramePeriodNS = 1000000000ULL / 60ULL;

    rgb15_table_init();
    cel_cache_clear();
    gNextTickNS = SDL_GetTicksNS();

    gMouseDeltaX = 0.0f;
    gMouseTurnF16 = 0;
    gMouseGameMode = 0;
    gEscapeDown = 0;
    gAltEnterSuppressed = 0;
    gRunning = 1;

    efmm_open_gamepad();

    gGrafBase.gf_VBLNumber = 0;
    gGrafBase.gf_VRAMPageSize = EFMM_PAGE_SIZE;
    gGrafBase.gf_ZeroPage = 0;
    gKernelBase.kb_CPUFlags = 0;
    gKernelTask.t.n_Item = 0;
    gKernelBase.kb_CurrentTask = &gKernelTask;

    SDL3_3DO_MountImageAuto();
    return 0;
}

void SDL3_3DO_Shutdown(void)
{
    int i;
    for (i = 0; i < gScreenCount; i++)
    {
        if (gBitmaps[i].bm_Buffer)
            free(gBitmaps[i].bm_Buffer);
        gBitmaps[i].bm_Buffer = NULL;
        gScreens[i].scr_TempBitmap = NULL;
    }
    gScreenCount = 0;
    if (gTexture)
        SDL_DestroyTexture(gTexture);
    if (gRenderer)
        SDL_DestroyRenderer(gRenderer);
    efmm_close_gamepad();
    if (gWindow)
        SDL_DestroyWindow(gWindow);
    if (gUploadBuffer)
        free(gUploadBuffer);
    gTexture = NULL;
    gRenderer = NULL;
    gWindow = NULL;
    gUploadBuffer = NULL;
    cel_cache_clear();
    SDL3_3DO_UnmountImage();
    SDL_Quit();
}

int32 OpenGraphicsFolio(void)
{
    if (gWindow && gRenderer && gTexture)
        return 0;
    return SDL3_3DO_Init(EFMM_FB_W, EFMM_FB_H, "Escape from Monster Manor");
}
int32 OpenMathFolio(void) { return 0; }
int32 OpenAudioFolio(void) { return 0; }
void CloseAudioFolio(void) {}
void InitFileFolioGlue(void) {}

int32 CreateScreenGroup(Item *items, TagArg *tags)
{
    int count = 2, i;
    if (tags)
    {
        int n;
        for (n = 0; tags[n].ta_Tag != CSG_TAG_DONE && n < 16; n++)
            if (tags[n].ta_Tag == CSG_TAG_SCREENCOUNT)
                count = (int)(intptr_t)tags[n].ta_Arg;
    }
    if (count > EFMM_SCREEN_COUNT)
        count = EFMM_SCREEN_COUNT;
    gScreenCount = count;
    for (i = 0; i < count; i++)
    {
        memset(&gBitmaps[i], 0, sizeof(Bitmap));
        memset(&gScreens[i], 0, sizeof(Screen));
        gBitmaps[i].bm_nItem = alloc_item(2, &gBitmaps[i]);
        gBitmaps[i].bm.n_Item = gBitmaps[i].bm_nItem;
        gBitmaps[i].bm_Width = EFMM_FB_W;
        gBitmaps[i].bm_Height = EFMM_FB_H;
        gBitmaps[i].bm_ClipWidth = EFMM_FB_W;
        gBitmaps[i].bm_ClipHeight = EFMM_FB_H;
        gBitmaps[i].bm_Buffer = (uint32 *)malloc(EFMM_FB_W * EFMM_FB_H * sizeof(uint32));
        if (!gBitmaps[i].bm_Buffer)
            return -1;
        memset(gBitmaps[i].bm_Buffer, 0, EFMM_FB_W * EFMM_FB_H * sizeof(uint32));
        gScreens[i].scr_nItem = alloc_item(1, &gScreens[i]);
        gScreens[i].scr_TempBitmap = &gBitmaps[i];
        gScreenFade[i] = 65536;
        items[i] = gScreens[i].scr_nItem;
    }
    gCurrentScreen = 0;
    (void)tags;
    return alloc_item(3, NULL);
}

void DeleteScreenGroup(Item item) { (void)item; }

void *LookupItem(Item item)
{
    return lookup_item_ptr(item, 0);
}

Item OpenItem(Item item, uint32 tags)
{
    (void)tags;
    return item;
}
Item FindNamedItem(int node, const char *name)
{
    (void)node;
    (void)name;
    return 1;
}
Item CreateItem(int node, TagArg *tags)
{
    (void)node;
    (void)tags;
    return alloc_item(4, NULL);
}
Item CreateThread(const char *name, int prio, void (*func)(void), int stack)
{
    (void)name;
    (void)prio;
    (void)func;
    (void)stack;
    return alloc_item(5, NULL);
}
void DeleteThread(Item item) { (void)item; }
int32 DoIO(Item ioItem, IOInfo *ioi)
{
    (void)ioItem;
    (void)ioi;
    return 0;
}
Item GetVBLIOReq(void) { return 1; }
Item GetVRAMIOReq(void) { return 1; }
void EnableHAVG(Item screen) { (void)screen; }
void EnableVAVG(Item screen) { (void)screen; }
void SetCEControl(Item bitmap, uint32 x)
{
    (void)bitmap;
    (void)x;
}

void SDL3_3DO_PumpInput(void)
{
    SDL_Event ev;
    const bool *keys;
    uint32 bits = 0;

    while (SDL_PollEvent(&ev))
    {
        if (ev.type == SDL_EVENT_QUIT)
        {
            gRunning = 0;
        }
        else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_RETURN &&
                 (ev.key.mod & SDL_KMOD_ALT) && !ev.key.repeat)
        {
            efmm_set_fullscreen(!gFullscreen);
            gAltEnterSuppressed = 1;
        }
        else if (ev.type == SDL_EVENT_MOUSE_MOTION && gMouseGameMode)
        {
            gMouseDeltaX += ev.motion.xrel;
        }
        else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !gGamepad)
        {
            gGamepadID = ev.gdevice.which;
            gGamepad = SDL_OpenGamepad(gGamepadID);
        }
        else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && ev.gdevice.which == gGamepadID)
        {
            efmm_close_gamepad();
            efmm_open_gamepad();
        }
    }

    keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_LEFT])
        bits |= ControlLeft;
    if (keys[SDL_SCANCODE_RIGHT])
        bits |= ControlRight;
    if (keys[SDL_SCANCODE_UP])
        bits |= ControlUp;
    if (keys[SDL_SCANCODE_DOWN])
        bits |= ControlDown;

    if (keys[SDL_SCANCODE_W])
        bits |= ControlUp;
    if (keys[SDL_SCANCODE_S])
        bits |= ControlDown;
    if (keys[SDL_SCANCODE_A])
        bits |= ControlLeftShift;
    if (keys[SDL_SCANCODE_D])
        bits |= ControlRightShift;

    if (keys[SDL_SCANCODE_F])
        bits |= ControlX;
    if (keys[SDL_SCANCODE_Q])
        bits |= ControlLeftShift;
    if (keys[SDL_SCANCODE_E])
        bits |= ControlRightShift;
    if (keys[SDL_SCANCODE_C])
        bits |= ControlC;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])
        bits |= ControlA;
    if (keys[SDL_SCANCODE_SPACE])
        bits |= ControlB;
    if (keys[SDL_SCANCODE_RETURN] && !gAltEnterSuppressed)
        bits |= ControlStart;
    if (!keys[SDL_SCANCODE_RETURN])
        gAltEnterSuppressed = 0;
    if (keys[SDL_SCANCODE_ESCAPE])
    {
        if (!gEscapeDown)
            bits |= ControlX;
        gEscapeDown = 1;
    }
    else
    {
        gEscapeDown = 0;
    }

    if (gGamepad && SDL_GamepadConnected(gGamepad))
    {
        int leftX = SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_LEFTX);
        int leftY = SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_LEFTY);
        int rightX = SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))
            bits |= ControlUp;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
            bits |= ControlDown;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
            bits |= ControlLeft;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
            bits |= ControlRight;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_SOUTH))
            bits |= ControlA;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_EAST))
            bits |= ControlB;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_WEST))
            bits |= ControlC;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_NORTH))
            bits |= ControlX;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_START))
            bits |= ControlStart;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_BACK))
            bits |= ControlX;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
            bits |= ControlLeftShift;
        if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
            bits |= ControlRightShift;
        if (leftX < -8000)
            bits |= ControlLeftShift;
        else if (leftX > 8000)
            bits |= ControlRightShift;
        if (leftY < -8000)
            bits |= ControlUp;
        else if (leftY > 8000)
            bits |= ControlDown;
        if (rightX < -8000)
            bits |= ControlLeft;
        else if (rightX > 8000)
            bits |= ControlRight;
        if (SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)
            bits |= ControlB;
        if (SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
            bits |= ControlA;
    }

    gMouseTurnF16 = 0;
    if (gMouseGameMode)
    {
        uint32 mouseButtons = SDL_GetRelativeMouseState(NULL, NULL);
        gMouseTurnF16 = (int32)(-gMouseDeltaX * 0.20f * 65536.0f);
        gMouseDeltaX = 0.0f;
        if (mouseButtons & SDL_BUTTON_LMASK)
            bits |= ControlA;
        if (mouseButtons & SDL_BUTTON_RMASK)
            bits |= ControlB;
    }
    else
    {
        gMouseDeltaX = 0.0f;
    }

    gPrevInputBits = gInputBits;
    gInputBits = bits;
}

uint32 SDL3_3DO_GetInputBits(void) { return gInputBits; }

void SDL3_3DO_AdvanceInput(int32 frames)
{
    (void)frames;
}

void SDL3_3DO_FrameSync(void)
{
    uint64 now;
    uint64 frames64;
    int32 frames;
    uint32 bits;
    int32 turnLeft, turnRight;

    now = SDL_GetTicksNS();
    if (!gNextTickNS)
        gNextTickNS = now;

    if (now < gNextTickNS)
        SDL_DelayNS(gNextTickNS - now);

    now = SDL_GetTicksNS();
    SDL3_3DO_PumpInput();
    frames64 = ((now >= gNextTickNS) ? ((now - gNextTickNS) / gFramePeriodNS) : 0) + 1;
    if (frames64 < 1)
        frames64 = 1;
    if (frames64 > 255)
        frames64 = 255;
    frames = (int32)frames64;

    bits = gInputBits;
    {
        extern JoyData jd;
        extern int32 joytrigger, oldjoybits;

        turnLeft = (bits & ControlLeft) ? frames : 0;
        turnRight = (bits & ControlRight) ? frames : 0;
        if (turnLeft && !turnRight)
            jd.jd_DAng += turnLeft;
        else if (turnRight && !turnLeft)
            jd.jd_DAng -= turnRight;
        else if (turnLeft && turnRight)
        {
            /* Match the original left-before-right priority. */
            jd.jd_DAng += turnLeft;
        }

        if (bits & ControlUp)
            jd.jd_DZ += frames;
        else if (bits & ControlDown)
            jd.jd_DZ -= frames;
        if (bits & ControlLeftShift)
            jd.jd_DX -= frames;
        else if (bits & ControlRightShift)
            jd.jd_DX += frames;
        if (bits & ControlA)
            jd.jd_ADown += frames;
        if (bits & ControlB)
            jd.jd_BDown += frames;
        if (bits & ControlC)
            jd.jd_CDown += frames;
        if (bits & ControlX)
            jd.jd_XDown += frames;
        if (bits & ControlStart)
            jd.jd_StartDown += frames;
        jd.jd_FrameCount += frames;
        joytrigger |= (int32)((bits ^ (uint32)oldjoybits) & bits);
        oldjoybits = (int32)bits;
    }
    gGrafBase.gf_VBLNumber += (uint32)frames;
    gNextTickNS += (uint64)frames * gFramePeriodNS;
    SDL3_3DO_AudioPump();
}

uint32 SDL3_3DO_GetHeldInput(void) { return gInputBits; }
uint32 SDL3_3DO_GetTriggerInput(void) { return gInputBits & ~gPrevInputBits; }
int32 SDL3_3DO_GetFrameCount(void)
{
    uint64 now = SDL_GetTicksNS();
    uint64 base = gNextTickNS;
    int32 f = (int32)((now > base ? now - base : 0) / gFramePeriodNS);
    if (f < 1)
        f = 1;
    if (f > 255)
        f = 255;
    return f;
}

int32 SDL3_3DO_GetMouseTurnF16(void)
{
    int32 value = gMouseTurnF16;
    gMouseTurnF16 = 0;
    return value;
}

void SDL3_3DO_SetMouseGameMode(int enabled)
{
    gMouseGameMode = enabled ? 1 : 0;
    gMouseDeltaX = 0.0f;
    gMouseTurnF16 = 0;
    if (gWindow)
    {
        SDL_SetWindowRelativeMouseMode(gWindow, gMouseGameMode);
        {
            float mx = 0.0f, my = 0.0f;
            (void)SDL_GetRelativeMouseState(&mx, &my);
        }
    }
}

void SDL3_3DO_SetFade(Item screen, int32 level16)
{
    int i;
    (void)screen;
    if (level16 < 0)
        level16 = 0;
    if (level16 > 65536)
        level16 = 65536;
    for (i = 0; i < gScreenCount; i++)
        gScreenFade[i] = level16;
}

void SDL3_3DO_Present(Item screen)
{
    int i, found = -1;
    for (i = 0; i < gScreenCount; i++)
        if (gScreens[i].scr_nItem == screen)
        {
            found = i;
            break;
        }
    if (found < 0 || !gRenderer || !gTexture)
        return;
    gCurrentScreen = found;
    memcpy(gUploadBuffer, gBitmaps[found].bm_Buffer, EFMM_FB_W * EFMM_FB_H * sizeof(uint32));
    if (gScreenFade[found] != 65536)
    {
        for (i = 0; i < EFMM_FB_W * EFMM_FB_H; i++)
        {
            uint32 p = gUploadBuffer[i];
            int r = (int)((p >> 16) & 255), g = (int)((p >> 8) & 255), b = (int)(p & 255);
            r = (r * gScreenFade[found]) >> 16;
            g = (g * gScreenFade[found]) >> 16;
            b = (b * gScreenFade[found]) >> 16;
            gUploadBuffer[i] = 0xFF000000u | ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
        }
    }
    SDL_UpdateTexture(gTexture, NULL, gUploadBuffer, EFMM_FB_W * (int)sizeof(uint32));
    SDL_RenderClear(gRenderer);
    SDL_RenderTexture(gRenderer, gTexture, NULL, NULL);
    SDL_RenderPresent(gRenderer);
}

Bitmap *SDL3_3DO_GetBitmap(Item bitmapItem) { return (Bitmap *)lookup_item_ptr(bitmapItem, 2); }
Bitmap *SDL3_3DO_GetScreenBitmap(Item screenItem)
{
    Screen *s = (Screen *)lookup_item_ptr(screenItem, 1);
    return s ? s->scr_TempBitmap : NULL;
}

void WaitVBL(Item vblIO, int n)
{
    int i;
    (void)vblIO;
    for (i = 0; i < n; i++)
        SDL3_3DO_FrameSync();
}

void DisplayScreen(Item screenItem, int waitvbl)
{
    SDL3_3DO_Present(screenItem);
    if (waitvbl)
        WaitVBL(0, waitvbl);
}

void SetVRAMPages(Item io, void *buf, uint32 val, int32 pages, uint32 mask)
{
    uint32 *p = (uint32 *)buf;
    int64 bytes = (int64)pages * EFMM_PAGE_SIZE;
    int64 count = bytes / 4;
    uint16 c = (uint16)(val & 0xffff);
    uint32 rgba = rgb15_to_rgba(c, 65536);
    int64 i;
    (void)io;
    (void)mask;

    if (p == NULL)
        return;
    if (count > (int64)EFMM_FB_W * EFMM_FB_H)
        count = (int64)EFMM_FB_W * EFMM_FB_H;
    for (i = 0; i < count; i++)
        p[i] = rgba;
}

void CopyVRAMPages(Item io, void *dst, void *src, int32 pages, uint32 mask)
{
    (void)io;
    (void)mask;
    memcpy(dst, src, EFMM_FB_W * EFMM_FB_H * sizeof(uint32));
    (void)pages;
}

int32 DrawCels(Item bitmapItem, CCB *ccb)
{
    Bitmap *bm = SDL3_3DO_GetBitmap(bitmapItem);
    CCB *p = ccb;
    int guard = 0;
    if (!bm)
        return -1;
    while (p && guard++ < 4096)
    {
        draw_ccb(bm, p);
        if (p->ccb_Flags & CCB_LAST)
            break;
        p = p->ccb_NextPtr;
    }
    return 0;
}

void SetFGPen(GrafCon *gc, int32 pen)
{
    if (gc)
        gc->gc_Pen = pen;
    gPenColor = pen;
}
void MoveTo(GrafCon *gc, int32 x, int32 y)
{
    (void)gc;
    (void)x;
    (void)y;
}
void DrawText8(GrafCon *gc, Item bitmapItem, const char *text)
{
    if (text)
        SDL3_3DO_BlitText(bitmapItem, gPenColor, gPenColor, text, 0xffff);
    (void)gc;
}
void FillRect(Item bitmapItem, GrafCon *gc, Rect *r)
{
    Bitmap *bm = SDL3_3DO_GetBitmap(bitmapItem);
    int x, y, x1, y1;
    uint32 col;
    if (!bm || !r)
        return;
    x1 = r->r_XMin;
    y1 = r->r_YMin;
    if (r->r_XMax >= r->r_XMin)
        x = r->r_XMax;
    else
        x = r->r_XMin;
    if (r->r_YMax >= r->r_YMin)
        y = r->r_YMax;
    else
        y = r->r_YMin;
    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x > bm->bm_Width)
        x = bm->bm_Width;
    if (y > bm->bm_Height)
        y = bm->bm_Height;
    col = rgb15_to_rgba((uint16)(gc ? gc->gc_Pen : gPenColor), 65536);
    for (; y1 < y; y1++)
        for (; x1 < x; x1++)
            bm->bm_Buffer[y1 * bm->bm_Width + x1] = col;
}
int32 WritePixel(Item bitmapItem, GrafCon *gc, int32 x, int32 y)
{
    Bitmap *bm = SDL3_3DO_GetBitmap(bitmapItem);
    if (!bm)
        return -1;
    if (x >= 0 && y >= 0 && x < bm->bm_Width && y < bm->bm_Height)
        bm->bm_Buffer[y * bm->bm_Width + x] = rgb15_to_rgba((uint16)(gc ? gc->gc_Pen : gPenColor), 65536);
    return 0;
}
int32 ReadPixel(Item bitmapItem, GrafCon *gc, int32 x, int32 y)
{
    Bitmap *bm;
    (void)gc;
    bm = SDL3_3DO_GetBitmap(bitmapItem);
    if (!bm)
        return 0;
    if (x < 0 || y < 0 || x >= bm->bm_Width || y >= bm->bm_Height)
        return 0;
    return rgba_to_rgb15(bm->bm_Buffer[y * bm->bm_Width + x]);
}
void SetScreenColor(Item screen, int32 entry)
{
    (void)screen;
    (void)entry;
}
void ResetScreenColors(Item screen) { (void)screen; }

void SetCurrentFont(Font *font)
{
    if (font)
        gCurrentFont = *font;
}
void ResetCurrentFont(void) { memset(&gCurrentFont, 0, sizeof(gCurrentFont)); }
Font *GetCurrentFont(void) { return &gCurrentFont; }
void SetCurrentFontCCB(CCB *ccb)
{
    if (ccb)
        gCurrentFont.font_CCB = ccb;
}

int32 kprintf(const char *fmt, ...)
{
    va_list ap;
    int32 n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    return n;
}
void PrintfSysErr(int32 err) { fprintf(stderr, "error %d\n", err); }

void InitEventUtility(int a, int b, int c)
{
    (void)a;
    (void)b;
    (void)c;
}
int GetControlPad(int pad, int wait, ControlPadEventData *cped)
{
    (void)pad;
    (void)wait;
    SDL3_3DO_PumpInput();
    if (cped)
        cped->cped_ButtonBits = gInputBits;
    return 0;
}

void *AllocMem(int32 size, uint32 typebits)
{
    void *p;
    (void)typebits;
    p = malloc((size_t)size);
    if (p && (typebits & MEMTYPE_FILL))
        memset(p, 0, (size_t)size);
    return p;
}
void FreeMem(void *ptr, int32 size)
{
    (void)size;
    free(ptr);
}

int32 GetDirectory(char *buf, int32 len)
{
    const char *p = SDL_GetBasePath();
    if (!p)
        p = "./";
    if (len > 0)
    {
        snprintf(buf, (size_t)len, "%s", p);
        buf[len - 1] = 0;
    }
    return 0;
}
int32 CreateAlias(const char *name, const char *path)
{
    (void)name;
    (void)path;
    return 0;
}
void ChangeDirectory(const char *path)
{
    if (OperaFS_IsMounted())
    {
        if (!OperaFS_ChangeDirectory(path))
            fprintf(stderr, "OperaFS: ChangeDirectory failed: %s (cwd=%s)\n",
                    path ? path : "(null)", OperaFS_GetCurrentDirectory());
        return;
    }
    (void)path;
}

static const char *resolve_path(const char *path)
{
    static char out[1024];
    const char *base;
    if (!path)
        return NULL;
    if (path[0] == '$' && strncmp(path, "$progdir/", 9) == 0)
    {
        base = SDL_GetBasePath();
        if (!base)
            base = "./";
        snprintf(out, sizeof(out), "%s%s", base, path + 9);
        return out;
    }
    if (path[0] == '$' && strncmp(path, "$progdir", 8) == 0)
    {
        base = SDL_GetBasePath();
        if (!base)
            base = "./";
        snprintf(out, sizeof(out), "%s%s", base, path + 8);
        return out;
    }
    return path;
}

Stream *OpenDiskStream(const char *path, int mode)
{
    Stream *s;
    FILE *fp;
    OperaFile *of;
    int64 n;

    (void)mode;
    SDL3_3DO_MountImageAuto();

    if (OperaFS_IsMounted())
    {
        of = OperaFS_Open(path);
        if (of)
        {
            s = (Stream *)calloc(1, sizeof(Stream));
            if (!s)
            {
                OperaFS_Close(of);
                return NULL;
            }
            s->fp = NULL;
            s->vfs = of;
            s->kind = 1;
            s->st_FileLength = (int32)OperaFS_Size(of);
            return s;
        }
    }

    path = resolve_path(path);
    fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (_fseeki64(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return NULL;
    }
    n = (int64)_ftelli64(fp);
    if (n < 0 || n > 0x7fffffffLL)
    {
        fclose(fp);
        return NULL;
    }
    if (_fseeki64(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return NULL;
    }
    s = (Stream *)calloc(1, sizeof(Stream));
    if (!s)
    {
        fclose(fp);
        return NULL;
    }
    s->fp = fp;
    s->vfs = NULL;
    s->kind = 0;
    s->st_FileLength = (int32)n;
    return s;
}

int32 ReadDiskStream(Stream *s, void *buf, int32 len)
{
    if (!s || !buf || len < 0)
        return -1;
    if (s->kind == 1)
        return (int32)OperaFS_Read((OperaFile *)s->vfs, buf, (size_t)len);
    if (!s->fp)
        return -1;
    return (int32)fread(buf, 1, (size_t)len, s->fp);
}

int32 CloseDiskStream(Stream *s)
{
    if (!s)
        return -1;
    if (s->kind == 1)
    {
        OperaFS_Close((OperaFile *)s->vfs);
        free(s);
        return 0;
    }
    if (s->fp)
    {
        int r = fclose(s->fp);
        free(s);
        return r;
    }
    free(s);
    return 0;
}
FILE *OpenDiskFile(const char *path, int mode)
{
    (void)mode;
    return fopen(resolve_path(path), "rb");
}

int32 ReadDiskFile(FILE *f, void *buf, int32 len) { return f ? (int32)fread(buf, 1, (size_t)len, f) : -1; }
int32 CloseDiskFile(FILE *f) { return f ? fclose(f) : -1; }

int SDL3_3DO_LoadFile(const char *path, uint8 **data, size_t *size)
{
    uint8 *buffer;
    int64 n;
    size_t got;
    FILE *fp;
    const char *resolved;

    if (data)
        *data = NULL;
    if (size)
        *size = 0;
    SDL3_3DO_MountImageAuto();

    if (OperaFS_IsMounted())
    {
        if (OperaFS_LoadFile(path, data, size))
            return 1;
    }

    resolved = resolve_path(path);
    fp = fopen(resolved, "rb");
    if (!fp)
        return 0;
    if (_fseeki64(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return 0;
    }
    n = (int64)_ftelli64(fp);
    if (n <= 0 || n > 0x7fffffffLL)
    {
        fclose(fp);
        return 0;
    }
    if (_fseeki64(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return 0;
    }
    buffer = (uint8 *)malloc((size_t)n);
    if (!buffer)
    {
        fclose(fp);
        return 0;
    }
    got = fread(buffer, 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n)
    {
        free(buffer);
        return 0;
    }
    if (data)
        *data = buffer;
    if (size)
        *size = got;
    return 1;
}

void SDL3_3DO_SetFrame(Bitmap *bm, uint32 *pixels, int pitch)
{
    int y;
    if (!bm || !pixels)
        return;
    for (y = 0; y < bm->bm_Height; y++)
        memcpy(bm->bm_Buffer + y * bm->bm_Width, (uint8 *)pixels + y * pitch, bm->bm_Width * sizeof(uint32));
}

void SDL3_3DO_Clear(Bitmap *bm, uint16 colour)
{
    int i;
    uint32 c = rgb15_to_rgba(colour, 65536);
    if (!bm)
        return;
    for (i = 0; i < bm->bm_Width * bm->bm_Height; i++)
        bm->bm_Buffer[i] = c;
}

void SDL3_3DO_BlitText(Item bitmapItem, int x, int y, const char *text, uint16 color)
{
    Bitmap *bm = SDL3_3DO_GetBitmap(bitmapItem);
    (void)x;
    (void)y;
    (void)text;
    (void)color;
    (void)bm;
}
