#include "sdl3_3do.h"
#include "castle.h"
#include "font.h"
#include "app_proto.h"

static int32 gFontMaxLetters;
static void* gFontStorage;

static uint32 be32(const uint8* p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static uint32 font_read_bits(const uint8* p, int maxBits, int* bitpos, int nbits)
{
    uint32 out = 0;
    int i, bpos;
    if (!p || !bitpos || nbits <= 0 || nbits > 24) return 0;
    for (i = 0;i < nbits;i++) {
        bpos = *bitpos;
        if (bpos < 0 || bpos >= maxBits) return out;
        out = (out << 1) | ((p[bpos >> 3] >> (7 - (bpos & 7))) & 1);
        *bitpos = bpos + 1;
    }
    return out;
}

static int font_bpp(uint32 pre0)
{
    switch (pre0 & PRE0_BPP_MASK) {
    case PRE0_BPP_1:return 1;
    case PRE0_BPP_2:return 2;
    case PRE0_BPP_4:return 4;
    case PRE0_BPP_6:return 6;
    case PRE0_BPP_8:return 8;
    case PRE0_BPP_16:return 16;
    }
    return 4;
}

/* Count the pixels produced by one packed CEL scan line.  Packed CELs do not
 * have a TLHPCNT width; each line terminates with EOL or the end of the line
 * transfer area.  The line transfer size is carried in the leading offset
 * byte/word. */
static int font_count_packed_line(const uint8* line, int lineBytes, int bpp)
{
    int bitpos = 0, maxBits = lineBytes * 8, out = 0;
    while (bitpos + 8 <= maxBits) {
        uint32 ctl = font_read_bits(line, maxBits, &bitpos, 8);
        int type = (int)(ctl >> 6);
        int count = (int)(ctl & 0x3f) + 1;
        if (type == 0) break;                 /* EOL */
        if (type == 1) {                        /* literal */
            if (bitpos + count * bpp > maxBits) break;
            bitpos += count * bpp;
            out += count;
        }
        else if (type == 2) {                 /* transparent */
            out += count;
        }
        else {                              /* repeat */
            if (bitpos + bpp > maxBits) break;
            bitpos += bpp;
            out += count;
        }
        if (out > 4096) break;
    }
    return out;
}

static int font_get_dimensions(const uint8* cel, int* widthOut, int* heightOut)
{
    uint32 pre0;
    int bpp, height, row;
    const uint8* line;
    int maxWidth = 0;

    if (!cel || !widthOut || !heightOut) return 0;
    pre0 = be32(cel);
    bpp = font_bpp(pre0);
    height = (int)((pre0 & PRE0_VCNT_MASK) >> PRE0_VCNT_SHIFT) + 1;
    if (height <= 0 || height > 1024) return 0;

    line = cel + 4; /* Packed font CEL: one preamble word, then packed scan lines. */
    for (row = 0;row < height;row++) {
        uint32 offsetWord;
        uint32 offsetWords;
        int headerBytes;
        int strideBytes;
        int lineBytes;
        int width;

        /* 1-byte line offset for <=6bpp, 2-byte offset otherwise.  The
         * offset value is the number of words beyond the mandatory two-word
         * minimum line transfer. */
        if (bpp <= 6) {
            offsetWord = (uint32)line[0];
            headerBytes = 1;
        }
        else {
            offsetWord = (uint32)((line[0] << 8) | line[1]);
            headerBytes = 2;
        }
        offsetWords = offsetWord + 2;
        if (offsetWords == 0 || offsetWords > 1024) return 0;
        strideBytes = (int)(offsetWords * 4);
        lineBytes = strideBytes - headerBytes;
        if (lineBytes <= 0) return 0;

        width = font_count_packed_line(line + headerBytes, lineBytes, bpp);
        if (width > maxWidth) maxWidth = width;
        line += strideBytes;
    }

    if (maxWidth <= 0) return 0;
    *widthOut = maxWidth;
    *heightOut = height;
    return 1;
}

int32 FontStringWidth(void* fontPtr, const char* str)
{
    const uint8* base = (const uint8*)fontPtr;
    int32 width = 0;
    if (!base || !str) return 0;
    while (*str && *str != '\n' && *str != '\r') {
        unsigned char ch = (unsigned char)*str++;
        uint32 off = be32(base + ((uint32)ch * 4));
        int32 cw, chh;
        if (off >= 0x7fffffffU || off < 4 ||
            !font_get_dimensions(base + off, &cw, &chh))
            width += 8;
        else
            width += chh + 2;
    }
    return width;
}

void FontInit(int32 num)
{
    gFontMaxLetters = num;
    gFontStorage = NULL;
}

void FontFree(void)
{
    if (gFontStorage) {
        free(gFontStorage);
        gFontStorage = NULL;
    }
    gFontMaxLetters = 0;
}

void FontPrint(FontStruct* text)
{
    uint8* base;
    const char* str;
    int32 x, y, line;
    int count = 0;
    CCB c;
    if (!text || !text->TextPtr || !text->FontPtr || !text->BItem) return;
    base = (uint8*)text->FontPtr;
    str = text->TextPtr;
    x = text->CoordX; y = text->CoordY; line = text->LineFeedOffset;

    while (*str && (gFontMaxLetters <= 0 || count < gFontMaxLetters)) {
        unsigned char ch = (unsigned char)*str++;
        uint32 off;
        int32 cw, chh;
        uint8* entry;

        if (ch == '\n' || ch == '\r') { x = text->CoordX; y += line; count++; continue; }

        /* mm.font is a table of BE32 offsets to packed CEL source data. */
        off = be32(base + ((uint32)ch * 4));
        if (off >= 0x7fffffffU || off < 4) { x += 8; count++; continue; }
        entry = base + off;

        if (!font_get_dimensions(entry, &cw, &chh)) {
            x += 8;
            count++;
            continue;
        }

        memset(&c, 0, sizeof(c));
        c.ccb_Flags = CCB_SPABS | CCB_PPABS | CCB_LDSIZE | CCB_LDPRS |
            CCB_LDPPMP | CCB_LDPLUT | CCB_YOXY | CCB_ACW | CCB_ACCW |
            CCB_ACE | CCB_PACKED | CCB_BGND;
        /* The original ARM FontPrint points SourcePtr at the character
         * entry itself.  The first 32-bit word of the entry is PRE0 and,
         * because CCB_CCBPRE is deliberately clear, the Cel Engine consumes
         * that source-resident preamble before decoding the packed pixels.
         * Keep that exact convention here rather than skipping PRE0 and
         * asking the renderer to reinterpret the first pixel bytes as PRE0. */
        c.ccb_SourcePtr = (CelData*)entry;
        c.ccb_PLUTPtr = text->PLUTPtr;
        c.ccb_XPos = x << 16;
        c.ccb_YPos = y << 16;
        c.ccb_HDX = 0;
        c.ccb_HDY = ONE_HD;
        c.ccb_VDX = ONE_VD;
        c.ccb_VDY = 0;
        c.ccb_HDDX = 0;
        c.ccb_HDDY = 0;
        c.ccb_PIXC = 0x1F001F00;
        c.ccb_PRE0 = be32(entry);
        c.ccb_PRE1 = 0;
        c.ccb_Width = cw;
        c.ccb_Height = chh;
        /* Font CEL source bytes are genuine 3DO big-endian data.  The font
         * palettes used by EFMM, however, are C int32 arrays such as:
         *
         *     (RGB5(a)<<16) | RGB5(b)
         *
         * On Windows those numeric words are stored little-endian.  Do not
         * feed that host byte stream to the ordinary BE16 PLUT reader: each
         * 16-bit colour would have its bytes swapped.  Instead tell the SDL
         * renderer to reconstruct the logical [high16,low16] PLUT pair. */
        c.ccb_SDLAssetBE = 1;
        c.ccb_SDLPLUT32Pairs = 1;
        c.ccb_SDLSourceHasPreamble = 0;
        DrawCels(text->BItem, &c);
        /* The font CELs are stored rotated 90 degrees.  cw is therefore the
         * vertical source dimension, while chh is the projected screen
         * width.  Advance by the projected width, matching the original
         * seven-pixel-class font metrics used throughout EFMM. */
        x += chh + 2;
        count++;
    }
}
